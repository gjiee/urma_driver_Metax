// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 *
 * 环节 5: GPU Pin + SG 构建（Scheme A，闭源 nvidia.ko 兼容版）
 *
 * 改动记录:
 *   - nvidia_p2p_get_pages_persistent / put_pages_persistent /
 *     nvidia_p2p_build_sg_table_phys 在闭源驱动上都 "Unknown symbol ...
 *     (err -2)"。换成经典 nvidia_p2p_get_pages/put_pages/free_page_table
 *     之后，同样报 Unknown symbol —— 但这次用 /proc/kallsyms 确认符号
 *     确实存在于已加载的 nvidia.ko 里（有 __ksymtab_xxx、__crc_xxx）。
 *   - udma.ko 是 MODULE_LICENSE("GPL")，nvidia.ko 是 proprietary
 *     (TAINT_PROPRIETARY_MODULE)。insmod 阶段对这类跨 GPL/proprietary
 *     边界的符号做的是"静态解析"：既要 GPL taint 判定通过，也要
 *     CONFIG_MODVERSIONS 开启时的 CRC 完全匹配（这台机器
 *     vermagic 里有 modversions）。任何一处不匹配，内核给的报错
 *     统一都是 "Unknown symbol X (err -2)"，没法从报错本身区分
 *     到底是 GPL taint 问题还是 CRC 不匹配问题。
 *   - 彻底绕过：不让 insmod 做静态符号解析，改用内核标准的
 *     symbol_get()/symbol_put() 在运行时动态从 kallsyms 里查函数地址。
 *     声明成 __attribute__((weak)) 之后，insmod udma.ko 阶段完全不要求
 *     这三个符号存在，查不到就在 register_seg 时返回错误，不会导致
 *     整个 udma.ko 加载失败。
 *
 * 当前阶段目标（TODO 标注处均是已知简化，先跑通 NV 函数交互）:
 *   1. register_seg 能成功调用到 nvidia_p2p_get_pages 并拿到 page_table；
 *   2. 本地拼出 sg_table，交给 UMMU 建表；
 *   3. free_callback 只做最小处理（打日志 + nvidia_p2p_free_page_table），
 *      不做完整的向上层通知失效；这条路径在测试场景不会被触发
 *      （只要 App 没有在 unregister_seg 之前 cudaFree），先放着。
 *   4. symbol_get/symbol_put 目前是"每次调用现取现还"，不是在整个 pin
 *      期间常驻持有引用——理论上 nvidia.ko 可能在 pin 和 unpin 之间被
 *      rmmod，属于已知简化，暂不处理。
 */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <nv-p2p.h>

#include "udma_gpu_p2p.h"

/*
 * GPU兼容测试补丁: 函数指针类型，与 nv-p2p.h 里的声明一一对应。
 * 用 typeof() 校验签名一致，避免手抄函数原型时打错参数类型。
 */
typedef typeof(nvidia_p2p_get_pages) *udma_fn_nvidia_p2p_get_pages_t;
typedef typeof(nvidia_p2p_put_pages) *udma_fn_nvidia_p2p_put_pages_t;
typedef typeof(nvidia_p2p_free_page_table) *udma_fn_nvidia_p2p_free_page_table_t;

/*
 * GPU兼容测试补丁: 运行时动态符号查找，绕开 insmod 阶段的静态符号解析
 * （GPL taint / CRC modversion 校验都在这一步，报错统一是
 * "Unknown symbol X (err -2)"，本函数直接跳过它）。
 *
 * symbol_get() 内部会给 nvidia 模块加一次引用计数（等价于弱化版的
 * try_module_get），保证查到符号期间 nvidia.ko 不会被 rmmod；
 * 用完必须对称调用 symbol_put()，否则 nvidia.ko 永远卸不掉。
 *
 * 返回 0 且三个输出指针都非 NULL 表示成功；失败时已经内部做好清理，
 * 调用方不需要再 symbol_put 任何东西。
 */
static int udma_gpu_p2p_sym_get(udma_fn_nvidia_p2p_get_pages_t *get_pages,
				udma_fn_nvidia_p2p_put_pages_t *put_pages,
				udma_fn_nvidia_p2p_free_page_table_t *free_page_table)
{
	*get_pages = (udma_fn_nvidia_p2p_get_pages_t)symbol_get(nvidia_p2p_get_pages);
	if (!*get_pages)
		goto err_no_get_pages;

	*put_pages = (udma_fn_nvidia_p2p_put_pages_t)symbol_get(nvidia_p2p_put_pages);
	if (!*put_pages)
		goto err_no_put_pages;

	*free_page_table = (udma_fn_nvidia_p2p_free_page_table_t)symbol_get(nvidia_p2p_free_page_table);
	if (!*free_page_table)
		goto err_no_free_page_table;

	return 0;

err_no_free_page_table:
	symbol_put(nvidia_p2p_put_pages);
	*put_pages = NULL;
err_no_put_pages:
	symbol_put(nvidia_p2p_get_pages);
	*get_pages = NULL;
err_no_get_pages:
	pr_err("UDMA: nvidia_p2p_* symbols not found — is nvidia.ko loaded "
	       "and does it export the classic GPUDirect P2P API? "
	       "check: cat /proc/kallsyms | grep nvidia_p2p_get_pages\n");
	return -ENOENT;
}

static void udma_gpu_p2p_sym_put(void)
{
	symbol_put(nvidia_p2p_free_page_table);
	symbol_put(nvidia_p2p_put_pages);
	symbol_put(nvidia_p2p_get_pages);
}

/*
 * GPU兼容测试补丁: 将用户 [va, va+len) 映射长度对齐到 64KB 边界
 * @addr: cfg->va（可非对齐，函数内部会 mask）
 * @len:  cfg->len
 * 返回: 从 (addr & MASK) 到向上对齐 end 的字节数
 */
static uint64_t udma_gpu_align_len(uint64_t addr, uint64_t len)
{
	uint64_t end;

	end = (addr + len + UDMA_GPU_P2P_PAGE_SIZE - 1) & UDMA_GPU_P2P_PAGE_MASK;
	return end - (addr & UDMA_GPU_P2P_PAGE_MASK);
}

/*
 * GPU兼容测试补丁: 本地拼 sg_table，替代不存在的 nvidia_p2p_build_sg_table_phys
 * @page_table: nvidia_p2p_get_pages() 返回值，entries/pages[]/page_size 都是
 *              官方 nv-p2p.h 公开字段，开源闭源驱动布局一致
 * @sgt:        输出，sg_page=NULL（无 struct page，纯 PA），
 *              sg_dma_address=GPU 物理地址，sg_dma_len=64KB
 */
static int udma_gpu_build_sg_table(struct nvidia_p2p_page_table *page_table,
				   struct sg_table *sgt)
{
	struct scatterlist *sg;
	uint64_t page_size = UDMA_GPU_P2P_PAGE_SIZE;
	uint32_t i;
	int ret;

	if (page_table->page_size != NVIDIA_P2P_PAGE_SIZE_64KB)
		return -EINVAL;

	ret = sg_alloc_table(sgt, page_table->entries, GFP_KERNEL);
	if (ret)
		return ret;

	for_each_sg(sgt->sgl, sg, page_table->entries, i) {
		sg_set_page(sg, NULL, page_size, 0);
		sg_dma_address(sg) = page_table->pages[i]->physical_address;
		sg_dma_len(sg) = page_size;
	}

	return 0;
}

/*
 * GPU兼容测试补丁: nvidia_p2p_get_pages() 的 free_callback
 *
 * 触发时机: GPU 内存在本段还没 unregister_seg 之前被 App 提前释放
 * （例如 cudaFree 早于 urma_unregister_seg）。NV 驱动会异步调用这里，
 * 调用之后 page_table 本身已经不可信，不能再对它做 nvidia_p2p_put_pages()。
 *
 * TODO（当前测试补丁的已知简化）:
 *   正确做法应该顺着 seg → ubcore_target_seg 往上通知 ubcore/上层这段已经
 *   失效（类似 nvidia-peermem 的 invalidate_peer_memory 回调），让后续
 *   WRITE/READ 直接报错而不是访问已释放的物理页。这里先只打印错误日志，
 *   把 force_freed 置位防止 unpin 时重复 put_pages，其余先不处理——
 *   只要测试流程严格 "register → 用 → unregister"，不提前 cudaFree，
 *   这个回调就不会被触发，不影响先跑通 pin/unpin 主链路。
 */
static void udma_gpu_p2p_free_callback(void *data)
{
	struct udma_segment *seg = (struct udma_segment *)data;
	udma_fn_nvidia_p2p_get_pages_t get_pages;
	udma_fn_nvidia_p2p_put_pages_t put_pages;
	udma_fn_nvidia_p2p_free_page_table_t free_page_table;

	if (!seg) {
		pr_err("UDMA: GPU free_callback with NULL seg\n");
		return;
	}

	pr_err("UDMA: GPU seg gpu_va=0x%llx force-freed by NVIDIA driver "
	       "(App freed GPU memory before unregister_seg) — segment now stale, "
	       "TODO: propagate invalidation to ubcore\n",
	       seg->gpu.gpu_va);

	seg->gpu.force_freed = true;

	/*
	 * 回调上下文里不能再调 nvidia_p2p_put_pages()（page_table 已经在
	 * NV 内部被清理，语义上等价于 nvidia-peermem.c 的
	 * nv_get_p2p_free_callback 场景）。nvidia_p2p_free_page_table()
	 * 官方文档标注是 no-op，留着调用是为了跟第三方生态的既有习惯保持一致。
	 */
	if (udma_gpu_p2p_sym_get(&get_pages, &put_pages, &free_page_table))
		return;
	free_page_table(seg->gpu.page_table);
	udma_gpu_p2p_sym_put();
}

/*
 * GPU兼容测试补丁 — 环节 5 主函数
 *
 * 调用链:
 *   udma_register_seg / udma_pin_pages_and_ioummu_map
 *     → udma_gpu_pin_seg_pages()
 *       → nvidia_p2p_get_pages(0, 0, gpu_va, mapped_size, &page_table,
 *                              udma_gpu_p2p_free_callback, seg)
 *       → udma_gpu_build_sg_table(page_table, &seg->umem->append.sgt)
 *
 * 参数说明:
 *   cfg->va:          用户传入的 CUDA GPU 虚拟地址
 *   cfg->len:         用户段长度
 *   cfg->token_id:    必填，否则 -EINVAL
 *   page_table:       NV 填充，每 entry 含 physical_address（GPU 显存 PA）
 *   seg->umem->append.sgt:
 *                     sg_page=NULL, sg_dma_address=PA, sg_dma_len=64KB
 */
int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg)
{
	udma_fn_nvidia_p2p_get_pages_t get_pages;
	udma_fn_nvidia_p2p_put_pages_t put_pages;
	udma_fn_nvidia_p2p_free_page_table_t free_page_table;
	struct nvidia_p2p_page_table *page_table = NULL;
	uint64_t gpu_va;
	uint64_t mapped_size;
	int ret;

	if (!udma_seg_cfg_is_gpu(cfg))
		return -EINVAL;

	if (!cfg->token_id) {
		dev_err(dev->dev, "GPU seg requires valid token_id\n");
		return -EINVAL;
	}

	/* GPU兼容测试补丁: gpu_va 向下对齐 64KB，作为 NV pin 起始地址 */
	gpu_va = cfg->va & UDMA_GPU_P2P_PAGE_MASK;
	mapped_size = udma_gpu_align_len(cfg->va, cfg->len);
	if (mapped_size == 0) {
		dev_err(dev->dev, "invalid GPU seg len, va=0x%llx len=0x%llx\n",
			cfg->va, cfg->len);
		return -EINVAL;
	}

	ret = udma_gpu_p2p_sym_get(&get_pages, &put_pages, &free_page_table);
	if (ret) {
		dev_err(dev->dev, "GPU P2P symbols unavailable, ret=%d\n", ret);
		return ret;
	}

	/*
	 * GPU兼容测试补丁: 调 NV 经典 P2P API（开源/闭源驱动均导出，
	 * nvidia-peermem.c 同款调用方式），运行时符号，静态 insmod
	 * 解析阶段不要求它存在。
	 * p2p_token/va_space: 固定 0（跟 nvidia-peermem.c nv_mem_acquire 一致，
	 *                     没有真正的 p2p_token 场景就传 0）
	 * free_callback:      udma_gpu_p2p_free_callback，data 传 seg 本身
	 *                     方便回调里定位 gpu.page_table/gpu_va
	 */
	ret = get_pages(0, 0, gpu_va, mapped_size, &page_table,
			udma_gpu_p2p_free_callback, seg);
	if (ret) {
		dev_err(dev->dev,
			"nvidia_p2p_get_pages failed gpu_va=0x%llx len=0x%llx ret=%d\n",
			gpu_va, mapped_size, ret);
		goto err_sym_put;
	}

	if (!page_table->entries || page_table->page_size != NVIDIA_P2P_PAGE_SIZE_64KB) {
		dev_err(dev->dev, "unsupported GPU page_table entries=%u page_size=%u\n",
			page_table->entries, page_table->page_size);
		ret = -EINVAL;
		goto err_put_pages;
	}

	seg->umem = kzalloc(sizeof(*seg->umem), GFP_KERNEL);
	if (!seg->umem) {
		ret = -ENOMEM;
		goto err_put_pages;
	}

	seg->umem->ub_dev = &dev->ub_dev;
	seg->umem->va = cfg->va;
	seg->umem->length = cfg->len;

	/*
	 * GPU兼容测试补丁: 本地拼 sg_table（不再依赖 NV 侧任何自造符号）
	 * 每个 sg: dma_address = GPU 物理地址（非 CPU PFN）
	 * 供 ummu_sva_matt_map(domain, gpu_va, sgt, prot) 消费
	 */
	ret = udma_gpu_build_sg_table(page_table, &seg->umem->append.sgt);
	if (ret) {
		dev_err(dev->dev, "udma_gpu_build_sg_table failed ret=%d\n", ret);
		goto err_free_umem;
	}

	seg->gpu.page_table = page_table;
	seg->gpu.gpu_va = gpu_va;
	seg->gpu.mapped_size = mapped_size;
	seg->gpu.force_freed = false;
	seg->addr = gpu_va;
	seg->length = mapped_size;
	seg->is_gpu_seg = true;

	/*
	 * TODO: 这里 symbol_put 之后就不再持有 nvidia.ko 的引用计数，
	 * pin 期间理论上 nvidia.ko 可能被 rmmod（已知简化，见文件头注释）。
	 * unpin 时 udma_gpu_unpin_seg_pages() 会重新 symbol_get 一次。
	 */
	udma_gpu_p2p_sym_put();
	return 0;

err_free_umem:
	kfree(seg->umem);
	seg->umem = NULL;
err_put_pages:
	put_pages(0, 0, gpu_va, page_table);
err_sym_put:
	udma_gpu_p2p_sym_put();
	return ret;
}

/*
 * GPU兼容测试补丁 — 环节 E 注销 pin
 * 与 udma_unpin_seg_pages() 中 GPU 分支配合
 * 入参 seg->gpu.gpu_va / seg->gpu.page_table 来自 pin 时保存
 */
void udma_gpu_unpin_seg_pages(struct udma_segment *seg)
{
	udma_fn_nvidia_p2p_get_pages_t get_pages;
	udma_fn_nvidia_p2p_put_pages_t put_pages;
	udma_fn_nvidia_p2p_free_page_table_t free_page_table;

	if (!seg || !udma_seg_is_gpu(seg))
		return;

	if (seg->gpu.page_table) {
		/*
		 * force_freed==true 说明 free_callback 已经跑过一次，
		 * page_table 在 NV 内部已经失效，不能再 put_pages。
		 */
		if (!seg->gpu.force_freed) {
			if (udma_gpu_p2p_sym_get(&get_pages, &put_pages, &free_page_table) == 0) {
				put_pages(0, 0, seg->gpu.gpu_va, seg->gpu.page_table);
				udma_gpu_p2p_sym_put();
			} else {
				pr_err("UDMA: unpin GPU seg gpu_va=0x%llx but nvidia_p2p_put_pages "
				       "symbol unavailable (nvidia.ko unloaded?) — leaking pin\n",
				       seg->gpu.gpu_va);
			}
		}
		seg->gpu.page_table = NULL;
	}

	if (seg->umem) {
		sg_free_table(&seg->umem->append.sgt);
		kfree(seg->umem);
		seg->umem = NULL;
	}

	seg->is_gpu_seg = false;
}
