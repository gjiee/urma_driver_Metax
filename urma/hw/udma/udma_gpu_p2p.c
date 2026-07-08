// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 *
 * 环节 5: GPU Pin + SG 构建（Scheme A）
 *   NV get_pages_persistent → build_sg_table_phys(PA → sg_dma_address)
 * 输出 sgt 供环节 7 ummu_sva_matt_map 使用；不返回用户态。
 *
 * 注意: nvidia_p2p_build_sg_table_phys 不是 NVIDIA 官方导出符号
 * (开源/闭源 nvidia.ko 都没有)，如果你链接的是官方驱动，
 * 把下面这行换成本文件内的本地实现（不依赖 NV 侧新增符号），
 * 只用官方稳定 ABI nvidia_p2p_get_pages_persistent +
 * struct nvidia_p2p_page_table/nvidia_p2p_page。
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
 * GPU兼容测试补丁 — 环节 5 主函数
 *
 * 调用链:
 *   udma_register_seg / udma_pin_pages_and_ioummu_map
 *     → udma_gpu_pin_seg_pages()
 *       → nvidia_p2p_get_pages_persistent(gpu_va, mapped_size, &page_table, 0)
 *       → nvidia_p2p_build_sg_table_phys(page_table, &seg->umem->append.sgt)
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
	 * GPU兼容测试补丁: 调用 NV 导出符号（开源/闭源 nvidia.ko 均导出）
	 * 入参 gpu_va:     64KB 对齐 GPU 虚拟地址
	 * 入参 mapped_size: 映射字节数（64KB 倍数）
	 * 出参 page_table:  pinned 页表，注销时 put_pages_persistent
	 * 入参 flags:      0
	 */
	ret = nvidia_p2p_get_pages_persistent(gpu_va, mapped_size, &page_table, 0);
	if (ret) {
		dev_err(dev->dev,
			"nvidia_p2p_get_pages_persistent failed gpu_va=0x%llx len=0x%llx ret=%d\n",
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
	 * GPU兼容测试补丁: 将 NV page_table 转为 scatterlist
	 * 每个 sg: dma_address = GPU 物理地址（非 CPU PFN）
	 * 供 ummu_sva_matt_map(domain, gpu_va, sgt, prot) 消费
	 */
	ret = nvidia_p2p_build_sg_table_phys(page_table, &seg->umem->append.sgt);
	if (ret) {
		dev_err(dev->dev, "nvidia_p2p_build_sg_table_phys failed ret=%d\n", ret);
		goto err_free_umem;
	}

	seg->gpu.page_table = page_table;
	seg->gpu.gpu_va = gpu_va;
	seg->gpu.mapped_size = mapped_size;
	seg->addr = gpu_va;
	seg->length = mapped_size;
	seg->is_gpu_seg = true;

	return 0;

err_free_umem:
	kfree(seg->umem);
	seg->umem = NULL;
err_put_pages:
	nvidia_p2p_put_pages_persistent(gpu_va, page_table, 0);
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
		nvidia_p2p_put_pages_persistent(seg->gpu.gpu_va, seg->gpu.page_table, 0);
		seg->gpu.page_table = NULL;
	}

	if (seg->umem) {
		sg_free_table(&seg->umem->append.sgt);
		kfree(seg->umem);
		seg->umem = NULL;
	}

	seg->is_gpu_seg = false;
}
