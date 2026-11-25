/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU segment flag parsing + pin/unpin entry for udma.ko.
 *
 * Userspace marks a GPU VA with udrv_data (struct udma_register_seg_ucmd),
 * not cfg->flag. ubcore never parses udrv_data. After ubcore validation
 * passes, udma_register_seg() sets the local-only UBCORE_REG_SEG_GPU_VA
 * bit on its tmp_cfg copy.
 *
 * The actual GPU pages come from a registered vendor provider
 * (see udma_gpu_p2p_ops.h). Which card is used is udma.ko module
 * param gpu_vendor=auto|metax|nvidia|none.
 */

#ifndef __UDMA_GPU_P2P_H__
#define __UDMA_GPU_P2P_H__

#include <linux/types.h>
#include <linux/uaccess.h>
#include <ub/urma/ubcore_types.h>
#include "udma_dev.h"
#include "udma_segment.h"

/* Local-only mark on tmp_cfg. Never validated by ubcore, never returned. */
#define UBCORE_REG_SEG_GPU_VA		BIT(15)

/*
 * Must match UMDK struct udma_register_seg_ucmd
 * (src/urma/hw/udma/udma_u_segment.c).
 *
 * reserved stays 0. Do not put a GPU vendor id here: pin dispatch is
 * udma.ko module param gpu_vendor=auto|metax|nvidia|none, not udrv_data.
 */
struct udma_register_seg_ucmd {
	uint32_t is_gpu_seg;
	uint32_t reserved;
};

static inline bool udma_seg_ucmd_is_gpu(struct ubcore_udata *udata)
{
	struct udma_register_seg_ucmd ucmd = { 0 };
	unsigned long byte;

	if (!udata || !udata->udrv_data || !udata->udrv_data->in_addr)
		return false;

	byte = copy_from_user(&ucmd, (void *)(uintptr_t)udata->udrv_data->in_addr,
			      min_t(uint32_t, udata->udrv_data->in_len, sizeof(ucmd)));
	if (byte)
		return false;

	return ucmd.is_gpu_seg != 0;
}

static inline bool udma_seg_cfg_is_gpu(const struct ubcore_seg_cfg *cfg)
{
	return !!(cfg->flag.value & UBCORE_REG_SEG_GPU_VA);
}

static inline bool udma_seg_is_gpu(const struct udma_segment *seg)
{
	return seg->is_gpu_seg;
}

int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg);
void udma_gpu_unpin_seg_pages(struct udma_segment *seg);

#endif /* __UDMA_GPU_P2P_H__ */
