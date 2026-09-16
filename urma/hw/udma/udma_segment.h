/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 * GPU pin state lives in struct udma_gpu_p2p_mem (vendor handle + VA).
 */

#ifndef __UDMA_SEGMENT_H__
#define __UDMA_SEGMENT_H__

#include "udma_dev.h"

/*
 * GPU pin state after a vendor provider returns.
 * handle:      vendor-opaque (MetaX mem_reg / NVIDIA wrapper)
 * gpu_va:      aligned pin start, also the UMMU IOVA key
 * mapped_size: aligned pin length
 * page_size:   vendor GPU page (MetaX variable, NVIDIA 64KB)
 * bus_offset:  MetaX aper_base - pci_bus_base; 0 for NVIDIA
 * vendor:      which provider pinned this seg (unpin must use the same)
 * force_freed: vendor invalidate ran (early GPU free)
 * matt_mapped: isolated SVA installed a phys-SG overlay
 */
struct udma_gpu_p2p_mem {
	void *handle;
	u8 vendor;
	uint64_t gpu_va;
	uint64_t mapped_size;
	uint32_t page_size;
	uint64_t bus_offset;
	bool force_freed;
	bool matt_mapped;
};

struct udma_segment {
	struct ubcore_target_seg core_tseg;
	struct udma_umem *umem;
	struct page *first_page;
	uint32_t token_value;
	bool token_value_valid;
	bool kernel_mode;
	bool is_gpu_seg;
	uint32_t tid;
	uint64_t addr;
	uint64_t length;
	uint64_t vm_start;
	uint64_t vm_end;
	//GPU兼容测试补丁: 仅 is_gpu_seg 时有效
	struct udma_gpu_p2p_mem gpu;
};

static inline struct udma_segment *to_udma_seg(struct ubcore_target_seg *seg)
{
	return container_of(seg, struct udma_segment, core_tseg);
}

struct ubcore_target_seg *udma_register_seg(struct ubcore_device *ub_dev,
					    struct ubcore_seg_cfg *cfg,
					    struct ubcore_udata *udata);
int udma_unregister_seg(struct ubcore_target_seg *seg);
struct ubcore_target_seg *udma_import_seg(struct ubcore_device *dev,
					  struct ubcore_target_seg_cfg *cfg,
					  struct ubcore_udata *udata);
int udma_unimport_seg(struct ubcore_target_seg *tseg);
void udma_destroy_seg_tree_table(void);

#endif /* __UDMA_SEGMENT_H__ */
