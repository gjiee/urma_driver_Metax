// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 *
 * 环节 5: GPU Pin + SG 构建（Scheme A，闭源 nvidia.ko 兼容版）
 *
 * 改动记录:
 *   - nvidia_p2p_get_pages_persistent / put_pages_persistent
 *     在闭源驱动上 "Unknown symbol ... (err -2)"，改用经典
 *     nvidia_p2p_get_pages / nvidia_p2p_put_pages（需要 free_callback）。
 *   - nvidia_p2p_build_sg_table_phys 根本不是官方符号（本项目自造），
 *     闭源驱动同样没有，改成本文件内 udma_gpu_build_sg_table() 本地实现，
 *     只用官方公开稳定字段 page_table->pages[i]->physical_address。
 *
 * 当前阶段目标（TODO 标注处均是已知简化，先跑通 NV 函数交互）:
 *   1. register_seg 能成功调用到 nvidia_p2p_get_pages 并拿到 page_table；
 *   2. 本地拼出 sg_table，交给 UMMU 建表；
 *   3. free_callback 只做最小处理（打日志 + nvidia_p2p_free_page_table），
 *      不做完整的向上层通知失效；这条路径在测试场景不会被触发
 *      （只要 App 没有在 unregister_seg 之前 cudaFree），先放着。
 */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <nv-p2p.h>

#include "udma_gpu_p2p.h"

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
	nvidia_p2p_free_page_table(seg->gpu.page_table);
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

	/*
	 * GPU兼容测试补丁: 调 NV 经典 P2P API（开源/闭源驱动均导出，
	 * nvidia-peermem.c 同款调用方式）
	 * p2p_token/va_space: 固定 0（跟 nvidia-peermem.c nv_mem_acquire 一致，
	 *                     没有真正的 p2p_token 场景就传 0）
	 * free_callback:      udma_gpu_p2p_free_callback，data 传 seg 本身
	 *                     方便回调里定位 gpu.page_table/gpu_va
	 */
	ret = nvidia_p2p_get_pages(0, 0, gpu_va, mapped_size, &page_table,
				   udma_gpu_p2p_free_callback, seg);
	if (ret) {
		dev_err(dev->dev,
			"nvidia_p2p_get_pages failed gpu_va=0x%llx len=0x%llx ret=%d\n",
			gpu_va, mapped_size, ret);
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

	seg->gpu.page_table = page_table;
	seg->gpu.gpu_va = gpu_va;
	seg->gpu.mapped_size = mapped_size;
	seg->gpu.force_freed = false;
	seg->addr = gpu_va;
	seg->length = mapped_size;
	seg->is_gpu_seg = true;

	return 0;

err_free_umem:
	kfree(seg->umem);
	seg->umem = NULL;
err_put_pages:
	nvidia_p2p_put_pages(0, 0, gpu_va, page_table);
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
		if (!seg->gpu.force_freed)
			nvidia_p2p_put_pages(0, 0, seg->gpu.gpu_va, seg->gpu.page_table);
		seg->gpu.page_table = NULL;
	}

	if (seg->umem) {
		sg_free_table(&seg->umem->append.sgt);
		kfree(seg->umem);
		seg->umem = NULL;
	}

	seg->is_gpu_seg = false;
}
