// SPDX-License-Identifier: GPL-2.0+
/*
 * UDMA <-> MetaX GPU P2P bridge.
 *
 * MetaX already exposes an acquire/get_mem sg_table API (GPL). This
 * module adapts it to udma.ko's vendor-neutral pin/unpin ops:
 *
 *   acquire_mem(va, len, invalidate, seg)
 *     → get_mem() → vendor sg_table
 *     → clone to UMMU phys SG (sg_page=NULL, dma_addr=PA)
 *
 * UB is not a PCI device, so we never dma_map with
 * metax_p2p_get_pci_dev(). UMMU programs BAR/aperture PAs the same
 * way as the NVIDIA path.
 *
 * bus_offset (aper_base - pci_bus_base) is stored and optionally
 * added to each PA. Default is pass-through; set apply_bus_offset=1
 * if the vendor sgt holds PCI bus addresses instead of BAR PAs.
 *
 * Load order: metax/mxcd.ko -> udma.ko -> udma_metax_p2p_bridge.ko
 */

#define pr_fmt(fmt) "udma_metax_p2p_bridge: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>

#include "udma_gpu_p2p_ops.h"
#ifdef HAVE_METAX_P2P_HEADER
#include <metax_p2p.h>
#else
#include "udma_metax_p2p.h"
#endif

static bool apply_bus_offset;
module_param(apply_bus_offset, bool, 0644);
MODULE_PARM_DESC(apply_bus_offset,
		 "Add MetaX bus_offset (aper_base - pci_bus_base) to SG PAs (default: 0)");

struct udma_metax_handle {
	void *mx_handle;
	struct sg_table *mx_sgt;
};

static dma_addr_t udma_metax_sg_pa(struct scatterlist *sg, uint64_t bus_offset)
{
	dma_addr_t pa = 0;

	if (sg_dma_address(sg))
		pa = sg_dma_address(sg);
	else if (sg_page(sg))
		pa = page_to_phys(sg_page(sg)) + sg->offset;

	if (apply_bus_offset)
		pa += bus_offset;

	return pa;
}

static unsigned int udma_metax_sg_len(struct scatterlist *sg)
{
	if (sg_dma_len(sg))
		return sg_dma_len(sg);
	return sg->length;
}

static int udma_metax_build_phys_sgt(struct sg_table *src, struct sg_table *dst,
				     uint64_t bus_offset)
{
	struct scatterlist *s, *d;
	unsigned int i;
	int ret;

	if (!src || !src->nents || !src->sgl)
		return -EINVAL;

	ret = sg_alloc_table(dst, src->nents, GFP_KERNEL);
	if (ret)
		return ret;

	d = dst->sgl;
	for_each_sg(src->sgl, s, src->nents, i) {
		dma_addr_t pa = udma_metax_sg_pa(s, bus_offset);
		unsigned int len = udma_metax_sg_len(s);

		if (!pa || !len) {
			sg_free_table(dst);
			return -EINVAL;
		}

		sg_set_page(d, NULL, len, 0);
		sg_dma_address(d) = pa;
		sg_dma_len(d) = len;
		d = sg_next(d);
	}

	return 0;
}

static int udma_metax_pin(uint64_t va, uint64_t len,
			  int (*invalidate)(void *arg), void *arg,
			  struct udma_gpu_p2p_pin *out)
{
	struct udma_metax_handle *h;
	struct sg_table *mx_sgt = NULL;
	uint64_t gpu_va, mapped_size;
	uint32_t page_size;
	uint64_t bus_offset;
	int ret;

	if (!va || !len)
		return -EINVAL;

	/*
	 * Same PAGE_SIZE align as metax_p2p_acquire_mem(). UMMU IOVA
	 * must cover the vendor window, not only the user [va, va+len).
	 */
	gpu_va = ALIGN_DOWN(va, PAGE_SIZE);
	mapped_size = ALIGN(va + len, PAGE_SIZE) - gpu_va;

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	ret = metax_p2p_acquire_mem(va, (size_t)len, &h->mx_handle,
				    invalidate, arg);
	if (ret) {
		pr_err("metax_p2p_acquire_mem va=0x%llx len=0x%llx ret=%d\n",
		       va, len, ret);
		kfree(h);
		return ret;
	}

	ret = metax_p2p_get_mem(h->mx_handle, &mx_sgt);
	if (ret || !mx_sgt) {
		pr_err("metax_p2p_get_mem failed ret=%d\n", ret);
		if (!ret)
			ret = -EFAULT;
		goto err_release;
	}

	h->mx_sgt = mx_sgt;
	page_size = metax_p2p_get_page_size(h->mx_handle);
	if (!page_size)
		page_size = PAGE_SIZE;
	bus_offset = metax_p2p_get_bus_offset(h->mx_handle);

	ret = udma_metax_build_phys_sgt(mx_sgt, &out->sgt, bus_offset);
	if (ret) {
		pr_err("build phys sgt failed ret=%d nents=%u\n",
		       ret, mx_sgt->nents);
		goto err_put;
	}

	out->handle = h;
	out->gpu_va = gpu_va;
	out->mapped_size = mapped_size;
	out->page_size = page_size;
	out->bus_offset = bus_offset;

	pr_info("pin OK va=0x%llx->0x%llx len=0x%llx->0x%llx page=0x%x "
		"bus_off=0x%llx apply_off=%d nents=%u first_pa=0x%llx\n",
		va, gpu_va, len, mapped_size, page_size, bus_offset,
		apply_bus_offset, out->sgt.nents,
		(uint64_t)sg_dma_address(out->sgt.sgl));
	return 0;

err_put:
	metax_p2p_put_mem(h->mx_handle, mx_sgt);
err_release:
	metax_p2p_release_mem(h->mx_handle);
	kfree(h);
	return ret;
}

static void udma_metax_unpin(void *handle, uint64_t gpu_va, bool recycled)
{
	struct udma_metax_handle *h = handle;

	(void)gpu_va;
	if (!h)
		return;

	/*
	 * recycle_mem_reg already put the sgt, dropped the GEM ref,
	 * and kfree'd the MetaX handle after invalidate().
	 */
	if (recycled) {
		kfree(h);
		return;
	}

	if (h->mx_handle) {
		if (h->mx_sgt)
			metax_p2p_put_mem(h->mx_handle, h->mx_sgt);
		metax_p2p_release_mem(h->mx_handle);
	}
	kfree(h);
}

static const struct udma_gpu_p2p_ops udma_metax_p2p_bridge_ops = {
	.vendor = UDMA_GPU_VENDOR_METAX,
	.name = "metax",
	.owner = THIS_MODULE,
	.recycles_handle = true,
	.pin = udma_metax_pin,
	.unpin = udma_metax_unpin,
};

static int __init udma_metax_p2p_bridge_init(void)
{
	int ret;

	ret = udma_register_gpu_p2p_ops(&udma_metax_p2p_bridge_ops);
	if (ret) {
		pr_err("udma_register_gpu_p2p_ops failed ret=%d "
		       "(is udma.ko loaded first?)\n", ret);
		return ret;
	}

	pr_info("loaded, registered MetaX GPU P2P ops with udma.ko\n");
	return 0;
}

static void __exit udma_metax_p2p_bridge_exit(void)
{
	udma_unregister_gpu_p2p_ops(&udma_metax_p2p_bridge_ops);
	pr_info("unloaded\n");
}

module_init(udma_metax_p2p_bridge_init);
module_exit(udma_metax_p2p_bridge_exit);

MODULE_SOFTDEP("pre: udma");
MODULE_DESCRIPTION("UDMA <-> MetaX GPU P2P bridge (acquire/get_mem → phys SG)");
MODULE_AUTHOR("HiSilicon Tech. Co., Ltd.");
MODULE_LICENSE("GPL");
