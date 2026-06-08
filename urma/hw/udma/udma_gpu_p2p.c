// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <nv-p2p.h>

#include "udma_gpu_p2p.h"
#include "udma_gpu_p2p_debug.h"

static uint64_t udma_gpu_align_len(uint64_t addr, uint64_t len)
{
	uint64_t end;

	end = (addr + len + UDMA_GPU_P2P_PAGE_SIZE - 1) & UDMA_GPU_P2P_PAGE_MASK;
	return end - (addr & UDMA_GPU_P2P_PAGE_MASK);
}

int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg)
{
	struct nvidia_p2p_page_table *page_table = NULL;
	uint64_t gpu_va;
	uint64_t mapped_size;
	int ret;

	if (!udma_seg_cfg_is_gpu(cfg))
		return -EINVAL;

	gpu_va = cfg->va & UDMA_GPU_P2P_PAGE_MASK;
	mapped_size = udma_gpu_align_len(cfg->va, cfg->len);
	if (mapped_size == 0)
		return -EINVAL;

	ret = nvidia_p2p_get_pages_persistent(gpu_va, mapped_size, &page_table, 0);
	if (ret) {
		dev_err(dev->dev,
			"nvidia_p2p_get_pages_persistent failed gpu_va=0x%llx len=0x%llx ret=%d\n",
			gpu_va, mapped_size, ret);
		return ret;
	}

	nvidia_p2p_debug_dump_addresses(gpu_va, page_table, "udma_register_seg");

	seg->umem = kzalloc(sizeof(*seg->umem), GFP_KERNEL);
	if (!seg->umem) {
		ret = -ENOMEM;
		goto err_put_pages;
	}

	seg->umem->ub_dev = &dev->ub_dev;
	seg->umem->va = cfg->va;
	seg->umem->length = cfg->len;
	seg->umem->owning_mm = NULL;

	ret = nvidia_p2p_build_sg_table_phys(page_table, &seg->umem->append.sgt);
	if (ret) {
		dev_err(dev->dev, "nvidia_p2p_build_sg_table_phys failed ret=%d\n", ret);
		goto err_free_umem;
	}

	seg->gpu.page_table = page_table;
	seg->gpu.gpu_va = gpu_va;
	seg->gpu.mapped_size = mapped_size;
	seg->gpu.page_size = UDMA_GPU_P2P_PAGE_SIZE;
	atomic_set(&seg->gpu.reg_test_done, 0);
	atomic_set(&seg->gpu.wr_test_done, 0);

	seg->addr = gpu_va;
	seg->length = mapped_size;
	seg->is_gpu_seg = true;

	dev_info(dev->dev,
		 "GPU P2P seg registered va=0x%llx len=0x%llx entries=%u (IOMMU/UMMU map skipped)\n",
		 cfg->va, cfg->len, page_table->entries);

	udma_gpu_p2p_registration_test(dev, seg);

	return 0;

err_free_umem:
	kfree(seg->umem);
	seg->umem = NULL;
err_put_pages:
	nvidia_p2p_put_pages_persistent(gpu_va, page_table, 0);
	return ret;
}

void udma_gpu_unpin_seg_pages(struct udma_segment *seg)
{
	if (!seg || !udma_seg_is_gpu(seg) || !seg->gpu.page_table)
		return;

	if (seg->umem) {
		sg_free_table(&seg->umem->append.sgt);
		kfree(seg->umem);
		seg->umem = NULL;
	}

	nvidia_p2p_put_pages_persistent(seg->gpu.gpu_va, seg->gpu.page_table, 0);
	seg->gpu.page_table = NULL;
	seg->is_gpu_seg = false;
}

uint64_t udma_gpu_resolve_dma_addr(const struct udma_segment *seg, uint64_t user_va)
{
	struct scatterlist *sg;
	uint32_t page_size;
	uint32_t index;
	uint64_t offset;

	if (!seg || !udma_seg_is_gpu(seg) || !seg->umem)
		return user_va;

	page_size = seg->gpu.page_size ? seg->gpu.page_size : UDMA_GPU_P2P_PAGE_SIZE;
	if (user_va < seg->gpu.gpu_va)
		return user_va;

	offset = user_va - seg->gpu.gpu_va;
	index = offset / page_size;
	if (index >= seg->umem->append.sgt.nents)
		return user_va;

	sg = &seg->umem->append.sgt.sgl[index];
	return sg_dma_address(sg) + (offset % page_size);
}
