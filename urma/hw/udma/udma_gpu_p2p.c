// SPDX-License-Identifier: GPL-2.0+
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU pin/unpin inside udma.ko — vendor-neutral.
 *
 * Host segments keep using pin_user_pages. GPU segments:
 *
 *   ioctl REGISTER_SEG (is_gpu_seg via udrv_data)
 *     → udma_register_seg()
 *     → udma_gpu_pin_seg_pages()
 *          ops->pin(va, len, invalidate, seg, &pin)
 *          take pin.sgt (phys SG, sg_page=NULL)
 *     → ummu_sva_matt_map_phys_sg(gpu_va, sgt)
 *
 * Vendor bridges (loaded last) register ops:
 *   MetaX  : acquire_mem → get_mem → clone to phys SG
 *   NVIDIA : get_pages   → build phys SG from BAR PAs
 *
 * udma.ko does not include nv-p2p.h or metax headers.
 */

#define dev_fmt(fmt) "UDMA: " fmt

#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/scatterlist.h>

#include "udma_gpu_p2p.h"
#include "udma_gpu_p2p_ops.h"

static const struct udma_gpu_p2p_ops *g_gpu_p2p_ops;
static DEFINE_MUTEX(g_gpu_p2p_ops_lock);

int udma_register_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->pin || !ops->unpin)
		return -EINVAL;

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops) {
		pr_err("UDMA: GPU P2P ops already registered by %s, reject %s\n",
		       g_gpu_p2p_ops->name ? g_gpu_p2p_ops->name : "?",
		       ops->name ? ops->name : "?");
		ret = -EBUSY;
		goto out;
	}
	g_gpu_p2p_ops = ops;
	pr_info("UDMA: GPU P2P ops registered by %s\n",
		ops->name ? ops->name : "?");
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
 * Vendor is dropping the GPU buffer (cudaFree / MetaX recycle) while
 * the URMA segment is still registered.
 *
 * MetaX recycle frees the vendor handle after this returns, so the
 * handle pointer is cleared here. NVIDIA keeps a local wrapper that
 * unpin() still frees.
 *
 * TODO: propagate invalidation to ubcore so later WRITE/READ fail
 * instead of touching stale PAs. Test path is register → use →
 * unregister without early free.
 */
static int udma_gpu_invalidate(void *arg)
{
	struct udma_segment *seg = arg;

	if (!seg) {
		pr_err("UDMA: GPU invalidate with NULL seg\n");
		return -EINVAL;
	}

	pr_err("UDMA: GPU seg gpu_va=0x%llx force-freed by vendor "
	       "(App freed GPU memory before unregister_seg) — segment now stale, "
	       "TODO: propagate invalidation to ubcore\n",
	       seg->gpu.gpu_va);

	WRITE_ONCE(seg->gpu.force_freed, true);
	return 0;
}

int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg)
{
	const struct udma_gpu_p2p_ops *ops;
	struct udma_gpu_p2p_pin pin = { 0 };
	int ret;

	if (!udma_seg_cfg_is_gpu(cfg))
		return -EINVAL;

	if (!cfg->token_id) {
		dev_err(dev->dev, "GPU seg requires valid token_id\n");
		return -EINVAL;
	}

	ops = udma_gpu_p2p_ops_get();
	if (!ops) {
		dev_err(dev->dev, "GPU P2P provider not registered "
			"(load udma_metax_p2p_bridge.ko or udma_nv_p2p_bridge.ko)\n");
		return -ENODEV;
	}

	ret = ops->pin(cfg->va, cfg->len, udma_gpu_invalidate, seg, &pin);
	if (ret) {
		dev_err(dev->dev,
			"GPU P2P pin failed va=0x%llx len=0x%llx provider=%s ret=%d\n",
			cfg->va, cfg->len, ops->name ? ops->name : "?", ret);
		udma_gpu_p2p_ops_put(ops);
		return ret;
	}

	if (!pin.mapped_size || !pin.sgt.nents || !pin.sgt.sgl) {
		dev_err(dev->dev, "GPU P2P pin returned empty range/sgt\n");
		if (pin.sgt.sgl)
			sg_free_table(&pin.sgt);
		ops->unpin(pin.handle, pin.gpu_va, false);
		udma_gpu_p2p_ops_put(ops);
		return -EINVAL;
	}

	seg->umem = kzalloc(sizeof(*seg->umem), GFP_KERNEL);
	if (!seg->umem) {
		sg_free_table(&pin.sgt);
		ops->unpin(pin.handle, pin.gpu_va, false);
		udma_gpu_p2p_ops_put(ops);
		return -ENOMEM;
	}

	seg->umem->ub_dev = &dev->ub_dev;
	seg->umem->va = cfg->va;
	seg->umem->length = cfg->len;
	seg->umem->append.sgt = pin.sgt;

	seg->gpu.handle = pin.handle;
	seg->gpu.gpu_va = pin.gpu_va;
	seg->gpu.mapped_size = pin.mapped_size;
	seg->gpu.page_size = pin.page_size;
	seg->gpu.bus_offset = pin.bus_offset;
	seg->gpu.force_freed = false;
	seg->addr = pin.gpu_va;
	seg->length = pin.mapped_size;
	seg->is_gpu_seg = true;

	dev_info(dev->dev,
		 "[GPU-TRACE] pin OK provider=%s gpu_va=0x%llx mapped=0x%llx "
		 "page_size=0x%x bus_off=0x%llx nents=%u first_pa=0x%llx\n",
		 ops->name ? ops->name : "?",
		 pin.gpu_va, pin.mapped_size, pin.page_size, pin.bus_offset,
		 pin.sgt.nents, (uint64_t)sg_dma_address(pin.sgt.sgl));

	udma_gpu_p2p_ops_put(ops);
	return 0;
}

void udma_gpu_unpin_seg_pages(struct udma_segment *seg)
{
	const struct udma_gpu_p2p_ops *ops;
	void *handle;
	bool recycled;

	if (!seg || !udma_seg_is_gpu(seg))
		return;

	recycled = READ_ONCE(seg->gpu.force_freed);
	handle = xchg(&seg->gpu.handle, NULL);

	if (handle) {
		ops = udma_gpu_p2p_ops_get();
		if (ops) {
			ops->unpin(handle, seg->gpu.gpu_va, recycled);
			udma_gpu_p2p_ops_put(ops);
		} else if (!recycled) {
			pr_err("UDMA: unpin GPU seg gpu_va=0x%llx but no GPU P2P "
			       "provider registered (bridge unloaded?) — leaking pin\n",
			       seg->gpu.gpu_va);
		}
	}

	if (seg->umem) {
		sg_free_table(&seg->umem->append.sgt);
		kfree(seg->umem);
		seg->umem = NULL;
	}

	seg->is_gpu_seg = false;
}
