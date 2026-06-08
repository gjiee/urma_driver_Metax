/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */

#ifndef __UDMA_GPU_P2P_H__
#define __UDMA_GPU_P2P_H__

#include <linux/types.h>
#include <ub/urma/ubcore_types.h>
#include "udma_dev.h"
#include "udma_segment.h"

/*
 * User-space ABI: set this bit in urma_reg_seg_flag / ubcore_reg_seg_flag
 * .value (reserved field bit 0, overall flag bit 14) when registering a
 * GPU device pointer via UBURMA_CMD_REGISTER_SEG / URMA_CMD_REGISTER_SEG.
 *
 * UMDK example:
 *   seg_cfg.flag.value |= UBCORE_REG_SEG_GPU_VA;
 *   seg_cfg.va = (uint64_t)cu_device_ptr;  // 64KB aligned
 *   seg_cfg.len = aligned_size;
 *   seg_cfg.flag.bs.access = UBCORE_ACCESS_LOCAL_ONLY;  // Phase-0 POC
 * Also skip ummu_grant() in udma_u_register_seg() for this flag.
 */
#define UBCORE_REG_SEG_GPU_VA		BIT(14)

#define UDMA_GPU_P2P_PAGE_SHIFT		16
#define UDMA_GPU_P2P_PAGE_SIZE		((uint64_t)1 << UDMA_GPU_P2P_PAGE_SHIFT)
#define UDMA_GPU_P2P_PAGE_MASK		(~(UDMA_GPU_P2P_PAGE_SIZE - 1))

#define UDMA_GPU_P2P_DEBUG_MAGIC	0xDEADBEEFU

static inline bool udma_seg_cfg_is_gpu(const struct ubcore_seg_cfg *cfg)
{
	return !!(cfg->flag.value & UBCORE_REG_SEG_GPU_VA);
}

static inline bool udma_seg_is_gpu(const struct udma_segment *seg)
{
	return seg->is_gpu_seg;
}

#ifdef CONFIG_UB_UDMA_NV_P2P
int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg);
void udma_gpu_unpin_seg_pages(struct udma_segment *seg);
uint64_t udma_gpu_resolve_dma_addr(const struct udma_segment *seg,
				   uint64_t user_va);
#else
static inline int udma_gpu_pin_seg_pages(struct udma_dev *dev,
					 struct udma_segment *seg,
					 struct ubcore_seg_cfg *cfg)
{
	return -ENOTSUPP;
}

static inline void udma_gpu_unpin_seg_pages(struct udma_segment *seg) {}

static inline uint64_t udma_gpu_resolve_dma_addr(const struct udma_segment *seg,
						 uint64_t user_va)
{
	return user_va;
}
#endif /* CONFIG_UB_UDMA_NV_P2P */

#endif /* __UDMA_GPU_P2P_H__ */
