/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */

#ifndef __UDMA_GPU_P2P_DEBUG_H__
#define __UDMA_GPU_P2P_DEBUG_H__

#include "udma_dev.h"
#include "udma_segment.h"

struct ubcore_jfs_wr;
struct udma_sqe_ctl;

#ifdef CONFIG_UB_UDMA_GPU_P2P_DEBUG
void udma_gpu_p2p_registration_test(struct udma_dev *dev,
				    struct udma_segment *seg);
uint64_t udma_gpu_p2p_debug_resolve_sge(struct udma_dev *dev,
					struct udma_segment *seg,
					uint64_t user_va, uint32_t len);
void udma_gpu_p2p_debug_post_wr(struct udma_dev *dev, struct ubcore_jfs_wr *wr,
				struct udma_sqe_ctl *sqe);
#else
static inline void udma_gpu_p2p_registration_test(struct udma_dev *dev,
						  struct udma_segment *seg) {}
static inline uint64_t udma_gpu_p2p_debug_resolve_sge(struct udma_dev *dev,
						      struct udma_segment *seg,
						      uint64_t user_va,
						      uint32_t len)
{
	return user_va;
}
static inline void udma_gpu_p2p_debug_post_wr(struct udma_dev *dev,
					      struct ubcore_jfs_wr *wr,
					      struct udma_sqe_ctl *sqe) {}
#endif

#endif /* __UDMA_GPU_P2P_DEBUG_H__ */
