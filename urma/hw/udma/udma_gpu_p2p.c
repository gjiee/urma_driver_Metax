// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU pin/unpin inside udma.ko — vendor-neutral.
 *
 * Host segments keep using pin_user_pages. GPU segments:
 *
 *   ioctl REGISTER_SEG (is_gpu_seg via udrv_data)
 *     → udma_register_seg()
 *     → udma_gpu_pin_seg_pages()
 *          ops->pin(va, len, invalidate, seg, &pin)
 *          take pin.sgt (phys SG, sg_page=NULL)
 *     → ummu_sva_matt_map_phys_sg(gpu_va, sgt)
 *
 * Vendor bridges (loaded last) register ops:
 *   MetaX  : acquire_mem → get_mem → clone to phys SG
 *   NVIDIA : get_pages   → build phys SG from BAR PAs
 *
 * udma.ko does not include nv-p2p.h or metax headers.
 */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/mm.h>

#include "udma_gpu_p2p.h"
#include "udma_gpu_p2p_ops.h"
#include "udma_common.h"

#define UDMA_GPU_SEL_AUTO	(-1)
#define UDMA_GPU_SEL_NONE	(-2)

static const struct udma_gpu_p2p_ops *g_gpu_p2p_ops[UDMA_GPU_VENDOR_MAX];
static DEFINE_MUTEX(g_gpu_p2p_ops_lock);

/*
 * Runtime switch: which GPU card URMA GPU-seg registration uses.
 *
 *   auto   — exactly one registered provider (default)
 *   metax  — 沐曦
 *   nvidia — NVIDIA (alias: nv)
 *   none   — disable GPU pin
 *
 * Both bridges may be loaded. pin() follows this param; unpin() uses
 * the vendor recorded on the segment.
 *
 *   insmod udma.ko gpu_vendor=metax
 *   echo nvidia > /sys/module/udma/parameters/gpu_vendor
 */
static char gpu_vendor[16] = "auto";

static int udma_gpu_parse_sel(const char *s)
{
	if (!s || !s[0] || !strcasecmp(s, "auto"))
		return UDMA_GPU_SEL_AUTO;
	if (!strcasecmp(s, "none") || !strcasecmp(s, "off"))
		return UDMA_GPU_SEL_NONE;
	if (!strcasecmp(s, "metax"))
		return UDMA_GPU_VENDOR_METAX;
	if (!strcasecmp(s, "nvidia") || !strcasecmp(s, "nv"))
		return UDMA_GPU_VENDOR_NVIDIA;
	return -EINVAL;
}

static const char *udma_gpu_vendor_name(int vendor)
{
	switch (vendor) {
	case UDMA_GPU_VENDOR_METAX:
		return "metax";
	case UDMA_GPU_VENDOR_NVIDIA:
		return "nvidia";
	case UDMA_GPU_SEL_NONE:
		return "none";
	case UDMA_GPU_SEL_AUTO:
		return "auto";
	default:
		return "?";
	}
}

static int gpu_vendor_set(const char *val, const struct kernel_param *kp)
{
	char buf[16];
	int sel;

	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -EINVAL;
	strim(buf);
	sel = udma_gpu_parse_sel(buf);
	if (sel == -EINVAL) {
		pr_err("UDMA: invalid gpu_vendor='%s' (use auto|metax|nvidia|none)\n",
		       buf);
		return -EINVAL;
	}
	strscpy(gpu_vendor, udma_gpu_vendor_name(sel), sizeof(gpu_vendor));
	pr_info("UDMA: gpu_vendor=%s\n", gpu_vendor);
	return 0;
}

static int gpu_vendor_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", gpu_vendor);
}

static const struct kernel_param_ops gpu_vendor_ops = {
	.set = gpu_vendor_set,
	.get = gpu_vendor_get,
};

module_param_cb(gpu_vendor, &gpu_vendor_ops, NULL, 0644);
MODULE_PARM_DESC(gpu_vendor,
		 "GPU card for URMA GPU segments: auto, metax, nvidia, none");

int udma_register_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->pin || !ops->unpin ||
	    ops->vendor >= UDMA_GPU_VENDOR_MAX)
		return -EINVAL;

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops[ops->vendor]) {
		pr_err("UDMA: GPU P2P %s already registered, reject %s\n",
		       udma_gpu_vendor_name(ops->vendor),
		       ops->name ? ops->name : "?");
		ret = -EBUSY;
		goto out;
	}
	g_gpu_p2p_ops[ops->vendor] = ops;
	pr_info("UDMA: GPU P2P ops registered by %s (gpu_vendor=%s)\n",
		ops->name ? ops->name : udma_gpu_vendor_name(ops->vendor),
		gpu_vendor);
out:
	mutex_unlock(&g_gpu_p2p_ops_lock);
	return ret;
}
EXPORT_SYMBOL(udma_register_gpu_p2p_ops);

void udma_unregister_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	if (!ops || ops->vendor >= UDMA_GPU_VENDOR_MAX)
		return;

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops[ops->vendor] != ops) {
		pr_warn("UDMA: udma_unregister_gpu_p2p_ops: %s mismatch, ignoring\n",
			ops->name ? ops->name : "?");
		goto out;
	}
	g_gpu_p2p_ops[ops->vendor] = NULL;
	pr_info("UDMA: GPU P2P ops unregistered (%s)\n",
		udma_gpu_vendor_name(ops->vendor));
out:
	mutex_unlock(&g_gpu_p2p_ops_lock);
}
EXPORT_SYMBOL(udma_unregister_gpu_p2p_ops);

static const struct udma_gpu_p2p_ops *
udma_gpu_p2p_try_get_slot(enum udma_gpu_vendor vendor)
{
	const struct udma_gpu_p2p_ops *ops;

	if (vendor >= UDMA_GPU_VENDOR_MAX)
		return NULL;
	ops = g_gpu_p2p_ops[vendor];
	if (ops && !try_module_get(ops->owner))
		return NULL;
	return ops;
}

static const struct udma_gpu_p2p_ops *
udma_gpu_p2p_ops_get_slot(enum udma_gpu_vendor vendor)
{
	const struct udma_gpu_p2p_ops *ops;

	mutex_lock(&g_gpu_p2p_ops_lock);
	ops = udma_gpu_p2p_try_get_slot(vendor);
	mutex_unlock(&g_gpu_p2p_ops_lock);
	return ops;
}

static const struct udma_gpu_p2p_ops *
udma_gpu_p2p_ops_get_selected(struct device *dev)
{
	const struct udma_gpu_p2p_ops *ops = NULL;
	int sel, i, found = -1, n = 0;

	sel = udma_gpu_parse_sel(gpu_vendor);
	if (sel == -EINVAL) {
		dev_err(dev, "invalid gpu_vendor=%s (use auto|metax|nvidia|none)\n",
			gpu_vendor);
		return ERR_PTR(-EINVAL);
	}
	if (sel == UDMA_GPU_SEL_NONE) {
		dev_err(dev, "GPU pin disabled (gpu_vendor=none)\n");
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (sel >= 0) {
		ops = udma_gpu_p2p_try_get_slot(sel);
		mutex_unlock(&g_gpu_p2p_ops_lock);
		if (!ops) {
			dev_err(dev,
				"gpu_vendor=%s but %s provider is not registered "
				"(load the matching udma_*_p2p_bridge.ko)\n",
				gpu_vendor, udma_gpu_vendor_name(sel));
			return ERR_PTR(-ENODEV);
		}
		return ops;
	}

	for (i = 0; i < UDMA_GPU_VENDOR_MAX; i++) {
		if (g_gpu_p2p_ops[i]) {
			n++;
			found = i;
		}
	}
	if (n == 1)
		ops = udma_gpu_p2p_try_get_slot(found);
	mutex_unlock(&g_gpu_p2p_ops_lock);

	if (n == 0) {
		dev_err(dev, "GPU P2P provider not registered "
			"(load udma_metax_p2p_bridge.ko or udma_nv_p2p_bridge.ko, "
			"then set gpu_vendor=metax|nvidia if both are loaded)\n");
		return ERR_PTR(-ENODEV);
	}
	if (n > 1) {
		dev_err(dev,
			"both metax and nvidia GPU providers are registered; "
			"set gpu_vendor=metax or gpu_vendor=nvidia\n");
		return ERR_PTR(-EINVAL);
	}
	if (!ops)
		return ERR_PTR(-ENODEV);
	return ops;
}

static void udma_gpu_p2p_ops_put(const struct udma_gpu_p2p_ops *ops)
{
	if (ops)
		module_put(ops->owner);
}

/*
 * Vendor is dropping the GPU buffer (cudaFree / MetaX recycle) while
 * the URMA segment is still registered.
 *
 * MetaX recycle frees the vendor handle after this returns, so the
 * handle pointer is cleared here. NVIDIA keeps a local wrapper that
 * unpin() still frees.
 *
 * TODO: propagate invalidation to ubcore so later WRITE/READ fail
 * instead of touching stale PAs. Test path is register → use →
 * unregister without early free.
 */
static int udma_gpu_invalidate(void *arg)
{
	struct udma_segment *seg = arg;

	if (!seg) {
		pr_err("UDMA: GPU invalidate with NULL seg\n");
		return -EINVAL;
	}

	pr_err("UDMA: GPU seg gpu_va=0x%llx force-freed by vendor "
	       "(App freed GPU memory before unregister_seg) — segment now stale, "
	       "TODO: propagate invalidation to ubcore\n",
	       seg->gpu.gpu_va);

	WRITE_ONCE(seg->gpu.force_freed, true);
	return 0;
}

int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg)
{
	const struct udma_gpu_p2p_ops *ops;
	struct udma_gpu_p2p_pin pin = { 0 };
	int ret;

	if (!udma_seg_cfg_is_gpu(cfg))
		return -EINVAL;

	if (!cfg->token_id) {
		dev_err(dev->dev, "GPU seg requires valid token_id\n");
		return -EINVAL;
	}

	ops = udma_gpu_p2p_ops_get_selected(dev->dev);
	if (IS_ERR(ops))
		return PTR_ERR(ops);

	ret = ops->pin(cfg->va, cfg->len, udma_gpu_invalidate, seg, &pin);
	if (ret) {
		dev_err(dev->dev,
			"GPU P2P pin failed va=0x%llx len=0x%llx provider=%s ret=%d\n",
			cfg->va, cfg->len, ops->name ? ops->name : "?", ret);
		udma_gpu_p2p_ops_put(ops);
		return ret;
	}

	if (!pin.mapped_size || !pin.sgt.nents || !pin.sgt.sgl) {
		dev_err(dev->dev, "GPU P2P pin returned empty range/sgt\n");
		if (pin.sgt.sgl)
			sg_free_table(&pin.sgt);
		ops->unpin(pin.handle, pin.gpu_va, false);
		udma_gpu_p2p_ops_put(ops);
		return -EINVAL;
	}

	seg->umem = kzalloc(sizeof(*seg->umem), GFP_KERNEL);
	if (!seg->umem) {
		sg_free_table(&pin.sgt);
		ops->unpin(pin.handle, pin.gpu_va, false);
		udma_gpu_p2p_ops_put(ops);
		return -ENOMEM;
	}

	seg->umem->ub_dev = &dev->ub_dev;
	seg->umem->va = cfg->va;
	seg->umem->length = cfg->len;
	seg->umem->append.sgt = pin.sgt;

	seg->gpu.handle = pin.handle;
	seg->gpu.vendor = ops->vendor;
	seg->gpu.gpu_va = pin.gpu_va;
	seg->gpu.mapped_size = pin.mapped_size;
	seg->gpu.page_size = pin.page_size;
	seg->gpu.bus_offset = pin.bus_offset;
	seg->gpu.force_freed = false;
	seg->addr = pin.gpu_va;
	seg->length = pin.mapped_size;
	seg->is_gpu_seg = true;

	dev_info(dev->dev,
		 "[GPU-TRACE] pin OK provider=%s gpu_va=0x%llx mapped=0x%llx "
		 "page_size=0x%x bus_off=0x%llx nents=%u first_pa=0x%llx\n",
		 ops->name ? ops->name : "?",
		 pin.gpu_va, pin.mapped_size, pin.page_size, pin.bus_offset,
		 pin.sgt.nents, (uint64_t)sg_dma_address(pin.sgt.sgl));

	udma_gpu_p2p_ops_put(ops);
	return 0;
}


/* ====================================================================
 * MXGDA: GPU-resident SQ (work queue) support
 *
 * SQ buffer lives in GPU HBM. The NIC reads WQEs over GDR, so the SQ VA
 * must be translated by UMMU exactly like a GPU segment:
 *
 *   create_jetty_ex(sq_cstm=1, is_gpu_seg=1, buff=<GPU VA>)
 *     -> udma_alloc_u_sq_buf()
 *     -> udma_gpu_pin_sq_pages()        [this file]
 *          ops->pin(va, len, NULL, sq, &pin)   vendor pin -> phys SG
 *          -> udma_ioummu_map_phys_sg()        MATT writes UMMU page table
 *
 * Unlike udma_gpu_pin_seg_pages() this path has no ubcore_seg_cfg and no
 * token_id: token_id is a segment access credential and carries no
 * meaning for a queue buffer.
 *
 * Works in BOTH SVA modes:
 *   separated (sva_sep_mode_en=1): ctx->mm set at udma_ctx.c, ctx->ummu_dev
 *   shared    (sva_sep_mode_en=0): ctx->mm set, no seg_tree needed here
 * ==================================================================== */

/*
 * vendor 已回收 GPU 显存后，异步撤销 UMMU 页表映射。
 *
 * 为什么必须异步：invalidate 回调可能运行在原子上下文，
 * iommu_unmap() 会持锁并可能睡眠，直接调用会触发 BUG。
 *
 * 为什么必须撤销：显存已被回收，但页表仍指向原物理地址。
 * 若不撤销，网卡 DMA 会读到已释放的物理页 —— 这是整机挂死的直接原因。
 */
static void udma_gpu_sq_reclaim_work(struct work_struct *work)
{
	struct udma_jetty_queue *sq =
		container_of(work, struct udma_jetty_queue, gpu_reclaim_work);

	if (!sq->udma_ctx || !sq->udma_ctx->dev)
		return;

	dev_warn(sq->udma_ctx->dev->dev,
		 "GPU SQ: withdrawing page table for reclaimed buffer va=0x%llx\n",
		 sq->buf.addr);

	/* 只撤销页表，不碰 vendor pin —— 显存已由 vendor 回收，
	 * 减引用会让 vendor 二次 recycle。真正的释放留给 unpin。 */
	udma_gpu_unmap_sq_pages(sq);

	if (sq->udma_ctx->dev)
		dev_warn(sq->udma_ctx->dev->dev,
			 "GPU SQ: page table withdrawn, queue 0x%llx is now unusable\n",
			 sq->buf.addr);
}

static int udma_gpu_sq_invalidate(void *arg)
{
	struct udma_jetty_queue *sq = arg;

	if (!sq)
		return -EINVAL;

	/*
	 * 应用在队列仍存活时释放了 GPU 显存（mcFree / 进程退出）。
	 * 必须记住这一点：后续 unpin 时要告诉 vendor "已经回收过了"，
	 * 否则会重复递减引用计数，触发二次 recycle。
	 * 与 udma_gpu_invalidate()（segment 路径）保持一致。
	 */
	WRITE_ONCE(sq->gpu_force_freed, true);

	if (sq->udma_ctx && sq->udma_ctx->dev)
		dev_err(sq->udma_ctx->dev->dev,
			"UDMA: GPU SQ buffer force-freed by vendor (va=0x%llx) — "
			"queue is stale, withdrawing mapping\n", sq->buf.addr);

	/*
	 * 立刻撤页表，让网卡拿不到这块显存。
	 * 若在原子上下文则交给 workqueue，绝不阻塞 vendor 路径。
	 */
	if (sq->gpu_pin) {
		if (in_atomic() || irqs_disabled())
			schedule_work(&sq->gpu_reclaim_work);
		else
			udma_gpu_sq_reclaim_work(&sq->gpu_reclaim_work);
	}

	return 0;
}

int udma_gpu_pin_sq_pages(struct udma_dev *dev, struct udma_jetty_queue *sq)
{
	const struct udma_gpu_p2p_ops *ops;
	struct udma_gpu_p2p_pin *pin;
	int ret;

	if (!sq->udma_ctx) {
		dev_err(dev->dev, "GPU SQ: udma_ctx is NULL\n");
		return -EINVAL;
	}
	if (!sq->udma_ctx->mm) {
		dev_err(dev->dev, "GPU SQ: mm is NULL, cannot build MATT mapping\n");
		return -EINVAL;
	}

	ops = udma_gpu_p2p_ops_get_selected(dev->dev);
	if (IS_ERR(ops)) {
		dev_err(dev->dev,
			"GPU SQ: no GPU P2P provider registered (load "
			"udma_metax_p2p_bridge.ko or udma_nv_p2p_bridge.ko)\n");
		return PTR_ERR(ops);
	}

	pin = kzalloc(sizeof(*pin), GFP_KERNEL);
	if (!pin) {
		udma_gpu_p2p_ops_put(ops);
		return -ENOMEM;
	}

	ret = ops->pin(sq->buf.addr, sq->buf.len,
		       udma_gpu_sq_invalidate, sq, pin);
	if (ret) {
		dev_err(dev->dev,
			"GPU SQ pin failed va=0x%llx len=0x%llx provider=%s ret=%d\n",
			sq->buf.addr, sq->buf.len,
			ops->name ? ops->name : "?", ret);
		udma_gpu_p2p_ops_put(ops);
		kfree(pin);
		return ret;
	}

	if (!pin->mapped_size || !pin->sgt.nents || !pin->sgt.sgl) {
		dev_err(dev->dev, "GPU SQ pin returned empty range/sgt\n");
		if (pin->sgt.sgl)
			sg_free_table(&pin->sgt);
		ops->unpin(pin->handle, pin->gpu_va, false);
		udma_gpu_p2p_ops_put(ops);
		kfree(pin);
		return -EINVAL;
	}

	/*
	 * 风险5：MATT 要求 phys-SG 在 IOVA 侧连续。单段（nents==1）天然满足；
	 * 多段时必须由厂商保证各段 DMA 地址连续，否则页表会拼错。
	 * 这里做显式校验：段数 >1 且长度不等 → 视为不支持，拒绝而不是冒险。
	 */
	if (pin->sgt.nents > 1) {
		uint64_t total = 0;
		struct scatterlist *s;
		unsigned int i;

		for_each_sg(pin->sgt.sgl, s, pin->sgt.nents, i)
			total += sg_dma_len(s);

		if (total != pin->mapped_size) {
			dev_err(dev->dev,
				"GPU SQ: multi-segment SG unsupported "
				"(nents=%u sum=0x%llx mapped=0x%llx) va=0x%llx\n",
				pin->sgt.nents, total, pin->mapped_size, sq->buf.addr);
			sg_free_table(&pin->sgt);
			ops->unpin(pin->handle, pin->gpu_va, false);
			kfree(pin);
			udma_gpu_p2p_ops_put(ops);
			return -EOPNOTSUPP;
		}
		dev_warn(dev->dev,
			 "GPU SQ: %u-segment SG accepted va=0x%llx mapped=0x%llx\n",
			 pin->sgt.nents, sq->buf.addr, pin->mapped_size);
	}

	sq->gpu_pin = pin;
	sq->gpu_vendor = ops->vendor;
	sq->gpu_force_freed = false;
	sq->gpu_map_installed = 0;
	INIT_WORK(&sq->gpu_reclaim_work, udma_gpu_sq_reclaim_work);

	dev_info(dev->dev,
		 "[GPU-SQ-TRACE] pin OK provider=%s sq_va=0x%llx mapped=0x%llx "
		 "page_size=0x%x nents=%u first_pa=0x%llx\n",
		 ops->name ? ops->name : "?",
		 pin->gpu_va, pin->mapped_size, pin->page_size,
		 pin->sgt.nents, (uint64_t)sg_dma_address(pin->sgt.sgl));

	udma_gpu_p2p_ops_put(ops);
	return 0;
}

/*
 * MXGDA: 检查 SQ 缓冲 VA 是否已经在 UMMU 页表里映射过。
 *
 * 必须做这一步：iommu_map() 对已存在的 IOVA 返回 -EEXIST(-17)，
 * 而失败后若再去 unpin vendor，vendor 可能已经 recycle 了这块显存
 * （MetaX 会打 "recycle memreg"），两侧状态不一致会导致系统卡死。
 * 所以映射前先探测，已占用就干净地报错返回，绝不触碰 vendor pin。
 */
static bool udma_sq_iova_already_mapped(struct udma_jetty_queue *sq)
{
	struct udma_context *ctx = sq->udma_ctx;
	struct iommu_domain *domain;

	if (!ctx || !ctx->ummu_dev)
		return false;

	domain = iommu_get_domain_for_dev(ctx->ummu_dev);
	if (!domain)
		return false;

	return iommu_iova_to_phys(domain, sq->buf.addr) != 0;
}

int udma_gpu_map_sq_pages(struct udma_dev *dev, struct udma_jetty_queue *sq)
{
	int ret;

	if (!sq->gpu_pin || !sq->udma_ctx || !sq->udma_ctx->mm)
		return -EINVAL;

	/* 预检：该 IOVA 已被占用则直接失败，不进入 iommu_map 失败路径。
	 * 典型场景：mcMalloc 反复返回同一地址，上一个 jfs 尚未释放。 */
	if (udma_sq_iova_already_mapped(sq)) {
		dev_err(dev->dev,
			"GPU SQ: VA 0x%llx already mapped in UMMU page table "
			"(len=0x%llx). Another jetty/segment still owns it — "
			"delete it first or use a different GPU buffer.\n",
			sq->buf.addr, sq->buf.len);
		return -EADDRINUSE;
	}

	/*
	 * 只做 local 单向映射（l_tid=ctx->tid -> r_tid=INVALID）。
	 *
	 * WQ/门铃是 LOCAL 资源：只有本地网卡经 ctx->tid 访问，远端节点
	 * 从不读写本地 WQ。所以不需要 remote(r_tid) 方向。
	 *
	 * 对照：GDR segment 在 !local_only 时做 remote+local 双向，
	 * 因为它是供远端 RDMA 访问的。两者语义不同，不要照搬。
	 * 实测：多加 remote 方向后 local 会 -EEXIST（同一页表槽），
	 * 且 unmap 时 actual=0x0 导致 -EFAULT，状态错乱。
	 */
	ret = udma_ioummu_map_phys_sg(sq->udma_ctx->tid, UMMU_INVALID_TID,
				      IOMMU_READ | IOMMU_WRITE,
				      sq->buf.addr, &sq->gpu_pin->sgt,
				      sq->udma_ctx->mm);
	if (ret) {
		dev_err(dev->dev,
			"GPU SQ MATT map failed va=0x%llx ret=%d\n",
			sq->buf.addr, ret);
		return ret;
	}


	set_bit(UDMA_SQ_MAP_INSTALLED_BIT,
		(unsigned long *)&sq->gpu_map_installed);

	dev_info(dev->dev, "[GPU-SQ-TRACE] MATT map OK sq_va=0x%llx len=0x%llx tid=%u\n",
		 sq->buf.addr, sq->buf.len, sq->udma_ctx->tid);
	return 0;
}

void udma_gpu_unmap_sq_pages(struct udma_jetty_queue *sq)
{
	uint64_t unmap_len;
	int ret;

	if (!sq || !sq->udma_ctx)
		return;

	if (!sq->gpu_pin || !sq->udma_ctx->mm)
		return;

	/* 防重入：reclaim work 与 free 路径都可能调用，保证 unmap 只执行一次。 */
	if (!test_and_clear_bit(UDMA_SQ_MAP_INSTALLED_BIT,
				(unsigned long *)&sq->gpu_map_installed))
		return;

	/* 用 vendor 实际 pin 到的长度，而不是 buf.len：
	 * vendor 会向上对齐范围（MetaX: PAGE_SIZE），两者可能不等。 */
	unmap_len = sq->gpu_pin->mapped_size ? sq->gpu_pin->mapped_size
					     : sq->buf.len;

	/*
	 * 关键：ummu_sva_matt_unmap_phys_sg() 对 size==0 直接返回 -EINVAL。
	 * 若此处长度为 0，说明状态已不一致，宁可报错也不要静默跳过
	 * —— 跳过会留下指向已释放显存的页表。
	 */
	if (!unmap_len) {
		dev_err(sq->udma_ctx->dev->dev,
			"GPU SQ: unmap with zero length va=0x%llx, state inconsistent\n",
			sq->buf.addr);
		return;
	}

	ret = udma_ioummu_unmap_phys_sg(sq->udma_ctx->tid, UMMU_INVALID_TID,
					sq->buf.addr, unmap_len,
					sq->udma_ctx->mm);
	if (ret) {
		dev_err(sq->udma_ctx->dev->dev,
			"GPU SQ: MATT unmap FAILED va=0x%llx len=0x%llx ret=%d "
			"— page table may still point at freed memory\n",
			sq->buf.addr, unmap_len, ret);
		/* 失败则恢复标志，允许后续重试 */
		set_bit(UDMA_SQ_MAP_INSTALLED_BIT,
			(unsigned long *)&sq->gpu_map_installed);
		return;
	}

	dev_info(sq->udma_ctx->dev->dev,
		 "[GPU-SQ-TRACE] MATT unmap OK sq_va=0x%llx len=0x%llx\n",
		 sq->buf.addr, unmap_len);
}

void udma_gpu_unpin_sq_pages(struct udma_jetty_queue *sq)
{
	const struct udma_gpu_p2p_ops *ops;
	struct udma_gpu_p2p_pin *pin;
	struct device *dma_dev;
	bool recycled;

	if (!sq || !sq->is_gpu_seg)
		return;

	pin = sq->gpu_pin;
	if (!pin)
		return;

	sq->gpu_pin = NULL;
	dma_dev = (sq->udma_ctx && sq->udma_ctx->dev) ? sq->udma_ctx->dev->dev : NULL;

	/*
	 * 读 vendor 是否已经回收过这块显存（invalidate 回调置位）。
	 * recycled=true 时必须传给 vendor，否则重复递减引用计数会触发二次
	 * recycle —— 表现为 unmap 时 -EINVAL、页表指向已释放物理页、整机挂死。
	 */
	recycled = READ_ONCE(sq->gpu_force_freed);

	if (pin->handle) {
		ops = udma_gpu_p2p_ops_get_slot(sq->gpu_vendor);
		if (ops) {
			ops->unpin(pin->handle, pin->gpu_va, recycled);
			udma_gpu_p2p_ops_put(ops);
		} else if (!recycled) {
			pr_err("UDMA: unpin GPU SQ va=0x%llx but no provider registered — leaking pin\n",
			       pin->gpu_va);
		}
	}

	if (pin->sgt.sgl)
		sg_free_table(&pin->sgt);
	kfree(pin);

	dev_info(dma_dev, "[GPU-SQ-TRACE] unpin done\n");
}

void udma_gpu_unpin_seg_pages(struct udma_segment *seg)
{
	const struct udma_gpu_p2p_ops *ops;
	void *handle;
	bool recycled;

	if (!seg || !udma_seg_is_gpu(seg))
		return;

	recycled = READ_ONCE(seg->gpu.force_freed);
	handle = xchg(&seg->gpu.handle, NULL);

	if (handle) {
		ops = udma_gpu_p2p_ops_get_slot(seg->gpu.vendor);
		if (ops) {
			ops->unpin(handle, seg->gpu.gpu_va, recycled);
			udma_gpu_p2p_ops_put(ops);
		} else if (!recycled) {
			pr_err("UDMA: unpin GPU seg gpu_va=0x%llx but no GPU P2P "
			       "provider registered (bridge unloaded?) — leaking pin\n",
			       seg->gpu.gpu_va);
		}
	}

	if (seg->umem) {
		sg_free_table(&seg->umem->append.sgt);
		kfree(seg->umem);
		seg->umem = NULL;
	}

	seg->is_gpu_seg = false;
}
