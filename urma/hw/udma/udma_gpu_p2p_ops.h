/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * Vendor-neutral GPU P2P contract between udma.ko and a vendor bridge.
 *
 * udma.ko never imports vendor symbols (nvidia / metax / ...). A bridge
 * module calls udma_register_gpu_p2p_ops() from its init and feeds
 * function pointers. Pin/unpin then go through those pointers, so GPL
 * taint / CRC checks never run against a proprietary GPU driver.
 *
 * The ops are shaped after MetaX (acquire handle + sg_table), not the
 * NVIDIA page-table API. NVIDIA is adapted in udma_nv_p2p_bridge.c.
 *
 * Load order: <gpu vendor>.ko -> udma.ko -> udma_*_p2p_bridge.ko
 */

#ifndef __UDMA_GPU_P2P_OPS_H__
#define __UDMA_GPU_P2P_OPS_H__

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/module.h>

/* Slots in udma.ko. Selected at runtime by module param gpu_vendor. */
enum udma_gpu_vendor {
	UDMA_GPU_VENDOR_METAX = 0,
	UDMA_GPU_VENDOR_NVIDIA,
	UDMA_GPU_VENDOR_MAX
};

/*
 * One successful vendor pin.
 *
 * sgt is UMMU-ready and owned by the caller after pin() returns:
 *   sg_page        = NULL
 *   sg_dma_address = GPU BAR / aperture PA
 *   sg_dma_len     = vendor page size
 *
 * handle is owned by the vendor until unpin(), or until the vendor
 * invalidate callback runs (MetaX recycle frees the handle itself).
 */
struct udma_gpu_p2p_pin {
	void *handle;
	struct sg_table sgt;
	uint64_t gpu_va;
	uint64_t mapped_size;
	uint32_t page_size;
	uint64_t bus_offset;
};

struct udma_gpu_p2p_ops {
	enum udma_gpu_vendor vendor;
	const char *name;
	struct module *owner;
	/* MetaX recycle kfree()s the handle after invalidate; NVIDIA does not. */
	bool recycles_handle;

	/*
	 * Pin [va, va+len) of current->mm. Vendor aligns the range
	 * (MetaX: PAGE_SIZE, NVIDIA: 64KB) and returns the aligned
	 * window in @out.
	 *
	 * @invalidate is called if the app frees GPU memory before
	 * unregister. Signature matches MetaX:
	 *   int (*callback)(void *arg)
	 * NVIDIA wraps it as void (*)(void *).
	 */
	int (*pin)(uint64_t va, uint64_t len,
		   int (*invalidate)(void *arg), void *arg,
		   struct udma_gpu_p2p_pin *out);

	/*
	 * Drop vendor resources. Must not free @sgt (caller owns it).
	 * @recycled is true when the vendor already invalidated the
	 * handle (do not put/release again; NVIDIA still frees its
	 * local wrapper).
	 */
	void (*unpin)(void *handle, uint64_t gpu_va, bool recycled);
};

int udma_register_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops);
void udma_unregister_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops);

#endif /* __UDMA_GPU_P2P_OPS_H__ */
