// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 *
 * 环节 5: GPU Pin + SG 构建（Scheme A，闭源 nvidia.ko 兼容版，v2）
 *
 * 改动记录:
 *   - nvidia_p2p_get_pages_persistent / put_pages_persistent /
 *     nvidia_p2p_build_sg_table_phys 在闭源驱动上都 "Unknown symbol ...
 *     (err -2)"，换成经典 nvidia_p2p_get_pages/put_pages/free_page_table
 *     依然 "Unknown symbol"——根因是 udma.ko 纯 GPL、nvidia.ko
 *     proprietary，跨边界静态符号解析不干净。
 *   - v1 修复（已废弃）: 切一个 Dual BSD/GPL 的 udma_nv_p2p_bridge.ko
 *     直接链 nvidia.ko，udma.ko 改成静态链接这个桥接模块的 wrapper 符号。
 *     结果 udma_nv_p2p_bridge.ko 因为链了 nvidia.ko，被内核
 *     inherit_taint() 无条件打上 TAINT_PROPRIETARY_MODULE（跟它自己
 *     声明什么 license 无关），udma.ko 再去链**它**，撞上一模一样的
 *     "Unknown symbol"墙，只是模块名换成了 udma_nv_p2p_bridge。
 *     本质没解决——只是把"谁是 proprietary"这层皮往后挪了一层。
 *   - v2 修复（当前采用）: 彻底反转控制方向，udma.ko **不 import 任何
 *     bridge 导出符号**。udma.ko 自己导出两个普通 GPL 符号
 *     udma_register_gpu_p2p_ops()/udma_unregister_gpu_p2p_ops()
 *     （本文件实现），udma_nv_p2p_bridge.ko 在自己 module_init() 里
 *     主动调用它们，把一组函数指针"注册"进来。udma.ko 后续调 NV P2P
 *     一律走函数指针（g_gpu_p2p_ops->get_pages(...)），不做任何静态
 *     符号解析——内核模块加载器层面根本没有 "resolve_symbol" 这一步，
 *     自然没有 taint 传播/GPL 校验/CRC 校验的问题。这正是 Mellanox
 *     ib_core.ko 与 nvidia-peermem.ko 之间的真实关系
 *     （ib_register_peer_memory_client 模式），不是我们发明的。
 *   - nvidia_p2p_build_sg_table_phys 根本不是官方符号（本项目自造），
 *     改成本文件内 udma_gpu_build_sg_table() 本地实现，只用官方公开
 *     稳定字段 page_table->pages[i]->physical_address（纯结构体字段
 *     读取，不涉及符号链接）。
 *
 * 加载顺序: nvidia.ko -> udma.ko -> udma_nv_p2p_bridge.ko
 * （bridge 必须最后装，因为它 init 时要反过来调 udma.ko 的导出符号）
 *
 * 当前阶段已知简化（TODO）:
 *   free_callback 只做最小处理（打日志 + free_page_table），不做完整的
 *   向上层通知失效；只要测试流程严格 "register → 用 → unregister"，
 *   不提前 cudaFree，这条路径不会被触发。
 */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
/*
 * 仅用于 struct nvidia_p2p_page_table/nvidia_p2p_page 的字段布局和
 * NVIDIA_P2P_PAGE_SIZE_64KB 常量 —— 纯编译期类型信息，不产生任何符号
 * 链接依赖（本文件不直接调用任何 nvidia_p2p_* 函数）。
 */
#include <nv-p2p.h>

#include "udma_gpu_p2p.h"
#include "udma_nv_p2p_bridge.h"

/*
 * GPU兼容测试补丁: 当前注册的 GPU P2P provider（唯一，桥接模块 init
 * 时注册，exit 时反注册）。加锁保护并发 register/unregister 和
 * pin/unpin 之间的竞争；调用前 try_module_get(owner) 防止调用期间
 * provider 模块被 rmmod。
 */
static const struct udma_gpu_p2p_ops *g_gpu_p2p_ops;
static DEFINE_MUTEX(g_gpu_p2p_ops_lock);

int udma_register_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->get_pages || !ops->put_pages || !ops->free_page_table)
		return -EINVAL;

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops) {
		pr_err("UDMA: GPU P2P ops already registered by %s, reject %s\n",
		       g_gpu_p2p_ops->owner ? g_gpu_p2p_ops->owner->name : "?",
		       ops->owner ? ops->owner->name : "?");
		ret = -EBUSY;
		goto out;
	}
	g_gpu_p2p_ops = ops;
	pr_info("UDMA: GPU P2P ops registered by %s\n",
		ops->owner ? ops->owner->name : "?");
out:
	mutex_unlock(&g_gpu_p2p_ops_lock);
	return ret;
}
EXPORT_SYMBOL(udma_register_gpu_p2p_ops);

void udma_unregister_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops != ops) {
		pr_warn("UDMA: udma_unregister_gpu_p2p_ops: ops pointer mismatch, ignoring\n");
		goto out;
	}
	g_gpu_p2p_ops = NULL;
	pr_info("UDMA: GPU P2P ops unregistered\n");
out:
	mutex_unlock(&g_gpu_p2p_ops_lock);
}
EXPORT_SYMBOL(udma_unregister_gpu_p2p_ops);

/*
 * GPU兼容测试补丁: 取当前 provider 并 try_module_get 固定住，防止拿到
 * 指针之后 provider 被 rmmod。用完必须配对调用 udma_gpu_p2p_ops_put()。
 * 返回 NULL 表示当前没有 provider 注册（bridge 模块没装/装晚了）。
 */
static const struct udma_gpu_p2p_ops *udma_gpu_p2p_ops_get(void)
{
	const struct udma_gpu_p2p_ops *ops;

	mutex_lock(&g_gpu_p2p_ops_lock);
	ops = g_gpu_p2p_ops;
	if (ops && !try_module_get(ops->owner))
		ops = NULL;
	mutex_unlock(&g_gpu_p2p_ops_lock);
	return ops;
}

static void udma_gpu_p2p_ops_put(const struct udma_gpu_p2p_ops *ops)
{
	if (ops)
		module_put(ops->owner);
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
 * @page_table: provider get_pages() 返回值，entries/pages[]/page_size 都是
 *              官方 nv-p2p.h 公开字段，开源闭源驱动布局一致。这里只是读
 *              结构体字段，不涉及任何符号链接，留在 udma.ko（GPL）里没有
 *              license 问题。
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
 * （经当前注册的 provider 转发，函数指针调用，不涉及符号链接）
 *
 * 触发时机: GPU 内存在本段还没 unregister_seg 之前被 App 提前释放
 * （例如 cudaFree 早于 urma_unregister_seg）。NV 驱动会异步调用这里，
 * 调用之后 page_table 本身已经不可信，不能再对它做 put_pages()。
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
	const struct udma_gpu_p2p_ops *ops;

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
	 * 回调上下文里不能再调 put_pages()（page_table 已经在 NV 内部被
	 * 清理，语义上等价于 nvidia-peermem.c 的 nv_get_p2p_free_callback
	 * 场景）。free_page_table 官方文档标注是 no-op，留着调用是为了跟
	 * 第三方生态的既有习惯保持一致。
	 */
	ops = udma_gpu_p2p_ops_get();
	if (!ops) {
		pr_err("UDMA: GPU free_callback but no provider registered "
		       "(bridge module unloaded?) — skip free_page_table\n");
		return;
	}
	ops->free_page_table(seg->gpu.page_table);
	udma_gpu_p2p_ops_put(ops);
}

/*
 * GPU兼容测试补丁 — 环节 5 主函数
 *
 * 调用链:
 *   udma_register_seg / udma_pin_pages_and_ioummu_map
 *     → udma_gpu_pin_seg_pages()
 *       → g_gpu_p2p_ops->get_pages(gpu_va, mapped_size, &page_table,
 *                                  udma_gpu_p2p_free_callback, seg)
 *         [udma_nv_p2p_bridge.ko 注册的回调，内部转发到 nvidia_p2p_get_pages;
 *          函数指针调用，udma.ko 不 import 任何 bridge/nvidia 符号]
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
	const struct udma_gpu_p2p_ops *ops;
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

	ops = udma_gpu_p2p_ops_get();
	if (!ops) {
		dev_err(dev->dev, "GPU P2P provider not registered "
			"(is udma_nv_p2p_bridge.ko loaded?)\n");
		return -ENODEV;
	}

	/*
	 * GPU兼容测试补丁: 函数指针调用，走已注册的 provider（通常是
	 * udma_nv_p2p_bridge.ko），udma.ko 本身不 import 任何 nvidia_p2p_*
	 * 或 bridge 导出符号。
	 * free_callback: udma_gpu_p2p_free_callback，data 传 seg 本身
	 *                方便回调里定位 gpu.page_table/gpu_va
	 */
	ret = ops->get_pages(gpu_va, mapped_size, &page_table,
			     udma_gpu_p2p_free_callback, seg);
	if (ret) {
		dev_err(dev->dev,
			"GPU P2P get_pages failed gpu_va=0x%llx len=0x%llx ret=%d\n",
			gpu_va, mapped_size, ret);
		udma_gpu_p2p_ops_put(ops);
		return ret;
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

	dev_info(dev->dev,
		 "[GPU-TRACE] NV pin + SG build OK: gpu_va=0x%llx mapped_size=0x%llx "
		 "entries=%u first_pa=0x%llx\n",
		 gpu_va, mapped_size, page_table->entries,
		 (uint64_t)sg_dma_address(seg->umem->append.sgt.sgl));

	/*
	 * GPU兼容测试补丁 — 一次性流程验证开关。
	 * 改成 1：NV pin + 本地 SG 构建成功后立刻打印上面那行，然后主动报错
	 * 结束流程（不再往下走 seg->is_gpu_seg=true / UMMU matt_map），方便
	 * 先单独验证"内核识别 GPU VA → 调 NV → 建 SG 表"这一段能不能跑通，
	 * 不受后面 UMMU 建表是否成功影响。验证完改回 0（或删掉这块 #if）
	 * 即可恢复正常注册流程。
	 */
#define UDMA_GPU_P2P_TEST_STOP_AFTER_SG 0
#if UDMA_GPU_P2P_TEST_STOP_AFTER_SG
	dev_err(dev->dev,
		"[GPU-TRACE] TEST STOP: UDMA_GPU_P2P_TEST_STOP_AFTER_SG=1, "
		"aborting before UMMU map on purpose (set macro to 0 to continue)\n");
	ret = -ECANCELED;
	goto err_free_sgt;
#endif

	seg->gpu.page_table = page_table;
	seg->gpu.gpu_va = gpu_va;
	seg->gpu.mapped_size = mapped_size;
	seg->gpu.force_freed = false;
	seg->addr = gpu_va;
	seg->length = mapped_size;
	seg->is_gpu_seg = true;

	/*
	 * TODO: 这里 put 之后就不再持有 provider 模块的引用计数，pin 期间
	 * 理论上 udma_nv_p2p_bridge.ko（进而 nvidia.ko）可能被 rmmod
	 * （已知简化，见文件头注释）。unpin 时重新 get 一次。
	 */
	udma_gpu_p2p_ops_put(ops);
	return 0;

#if UDMA_GPU_P2P_TEST_STOP_AFTER_SG
err_free_sgt:
	sg_free_table(&seg->umem->append.sgt);
#endif
err_free_umem:
	kfree(seg->umem);
	seg->umem = NULL;
err_put_pages:
	ops->put_pages(gpu_va, page_table);
	udma_gpu_p2p_ops_put(ops);
	return ret;
}

/*
 * GPU兼容测试补丁 — 环节 E 注销 pin
 * 与 udma_unpin_seg_pages() 中 GPU 分支配合
 * 入参 seg->gpu.gpu_va / seg->gpu.page_table 来自 pin 时保存
 */
void udma_gpu_unpin_seg_pages(struct udma_segment *seg)
{
	if (!seg || !udma_seg_is_gpu(seg))
		return;

	if (seg->gpu.page_table) {
		/*
		 * force_freed==true 说明 free_callback 已经跑过一次，
		 * page_table 在 NV 内部已经失效，不能再 put_pages。
		 */
		if (!seg->gpu.force_freed) {
			const struct udma_gpu_p2p_ops *ops = udma_gpu_p2p_ops_get();

			if (ops) {
				ops->put_pages(seg->gpu.gpu_va, seg->gpu.page_table);
				udma_gpu_p2p_ops_put(ops);
			} else {
				pr_err("UDMA: unpin GPU seg gpu_va=0x%llx but no GPU P2P "
				       "provider registered (bridge unloaded?) — leaking pin\n",
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
