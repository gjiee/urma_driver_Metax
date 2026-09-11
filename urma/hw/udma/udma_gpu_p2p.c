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
#include <linux/string.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/mm.h>

#include "udma_gpu_p2p.h"
#include "udma_gpu_p2p_ops.h"

#define UDMA_GPU_SEL_AUTO	(-1)
#define UDMA_GPU_SEL_NONE	(-2)

static const struct udma_gpu_p2p_ops *g_gpu_p2p_ops[UDMA_GPU_VENDOR_MAX];
static DEFINE_MUTEX(g_gpu_p2p_ops_lock);

/*
 * Runtime switch: which GPU card URMA GPU-seg registration uses.
 *
 *   auto   — exactly one registered provider (default)
 *   metax  — 沐曦
 *   nvidia — NVIDIA (alias: nv)
 *   none   — disable GPU pin
 *
 * Both bridges may be loaded. pin() follows this param; unpin() uses
 * the vendor recorded on the segment.
 *
 *   insmod udma.ko gpu_vendor=metax
 *   echo nvidia > /sys/module/udma/parameters/gpu_vendor
 */
static char gpu_vendor[16] = "auto";

static int udma_gpu_parse_sel(const char *s)
{
	if (!s || !s[0] || !strcasecmp(s, "auto"))
		return UDMA_GPU_SEL_AUTO;
	if (!strcasecmp(s, "none") || !strcasecmp(s, "off"))
		return UDMA_GPU_SEL_NONE;
	if (!strcasecmp(s, "metax"))
		return UDMA_GPU_VENDOR_METAX;
	if (!strcasecmp(s, "nvidia") || !strcasecmp(s, "nv"))
		return UDMA_GPU_VENDOR_NVIDIA;
	return -EINVAL;
}

static const char *udma_gpu_vendor_name(int vendor)
{
	switch (vendor) {
	case UDMA_GPU_VENDOR_METAX:
		return "metax";
	case UDMA_GPU_VENDOR_NVIDIA:
		return "nvidia";
	case UDMA_GPU_SEL_NONE:
		return "none";
	case UDMA_GPU_SEL_AUTO:
		return "auto";
	default:
		return "?";
	}
}

static int gpu_vendor_set(const char *val, const struct kernel_param *kp)
{
	char buf[16];
	int sel;

	if (strscpy(buf, val, sizeof(buf)) < 0)
		return -EINVAL;
	strim(buf);
	sel = udma_gpu_parse_sel(buf);
	if (sel == -EINVAL) {
		pr_err("UDMA: invalid gpu_vendor='%s' (use auto|metax|nvidia|none)\n",
		       buf);
		return -EINVAL;
	}
	strscpy(gpu_vendor, udma_gpu_vendor_name(sel), sizeof(gpu_vendor));
	pr_info("UDMA: gpu_vendor=%s\n", gpu_vendor);
	return 0;
}

static int gpu_vendor_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", gpu_vendor);
}

static const struct kernel_param_ops gpu_vendor_ops = {
	.set = gpu_vendor_set,
	.get = gpu_vendor_get,
};

module_param_cb(gpu_vendor, &gpu_vendor_ops, NULL, 0644);
MODULE_PARM_DESC(gpu_vendor,
		 "GPU card for URMA GPU segments: auto, metax, nvidia, none");

int udma_register_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	int ret = 0;

	if (!ops || !ops->pin || !ops->unpin ||
	    ops->vendor >= UDMA_GPU_VENDOR_MAX)
		return -EINVAL;

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops[ops->vendor]) {
		pr_err("UDMA: GPU P2P %s already registered, reject %s\n",
		       udma_gpu_vendor_name(ops->vendor),
		       ops->name ? ops->name : "?");
		ret = -EBUSY;
		goto out;
	}
	g_gpu_p2p_ops[ops->vendor] = ops;
	pr_info("UDMA: GPU P2P ops registered by %s (gpu_vendor=%s)\n",
		ops->name ? ops->name : udma_gpu_vendor_name(ops->vendor),
		gpu_vendor);
out:
	mutex_unlock(&g_gpu_p2p_ops_lock);
	return ret;
}
EXPORT_SYMBOL(udma_register_gpu_p2p_ops);

void udma_unregister_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops)
{
	if (!ops || ops->vendor >= UDMA_GPU_VENDOR_MAX)
		return;

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (g_gpu_p2p_ops[ops->vendor] != ops) {
		pr_warn("UDMA: udma_unregister_gpu_p2p_ops: %s mismatch, ignoring\n",
			ops->name ? ops->name : "?");
		goto out;
	}
	g_gpu_p2p_ops[ops->vendor] = NULL;
	pr_info("UDMA: GPU P2P ops unregistered (%s)\n",
		udma_gpu_vendor_name(ops->vendor));
out:
	mutex_unlock(&g_gpu_p2p_ops_lock);
}
EXPORT_SYMBOL(udma_unregister_gpu_p2p_ops);

static const struct udma_gpu_p2p_ops *
udma_gpu_p2p_try_get_slot(enum udma_gpu_vendor vendor)
{
	const struct udma_gpu_p2p_ops *ops;

	if (vendor >= UDMA_GPU_VENDOR_MAX)
		return NULL;
	ops = g_gpu_p2p_ops[vendor];
	if (ops && !try_module_get(ops->owner))
		return NULL;
	return ops;
}

static const struct udma_gpu_p2p_ops *
udma_gpu_p2p_ops_get_slot(enum udma_gpu_vendor vendor)
{
	const struct udma_gpu_p2p_ops *ops;

	mutex_lock(&g_gpu_p2p_ops_lock);
	ops = udma_gpu_p2p_try_get_slot(vendor);
	mutex_unlock(&g_gpu_p2p_ops_lock);
	return ops;
}

static const struct udma_gpu_p2p_ops *
udma_gpu_p2p_ops_get_selected(struct device *dev)
{
	const struct udma_gpu_p2p_ops *ops = NULL;
	int sel, i, found = -1, n = 0;

	sel = udma_gpu_parse_sel(gpu_vendor);
	if (sel == -EINVAL) {
		dev_err(dev, "invalid gpu_vendor=%s (use auto|metax|nvidia|none)\n",
			gpu_vendor);
		return ERR_PTR(-EINVAL);
	}
	if (sel == UDMA_GPU_SEL_NONE) {
		dev_err(dev, "GPU pin disabled (gpu_vendor=none)\n");
		return ERR_PTR(-ENODEV);
	}

	mutex_lock(&g_gpu_p2p_ops_lock);
	if (sel >= 0) {
		ops = udma_gpu_p2p_try_get_slot(sel);
		mutex_unlock(&g_gpu_p2p_ops_lock);
		if (!ops) {
			dev_err(dev,
				"gpu_vendor=%s but %s provider is not registered "
				"(load the matching udma_*_p2p_bridge.ko)\n",
				gpu_vendor, udma_gpu_vendor_name(sel));
			return ERR_PTR(-ENODEV);
		}
		return ops;
	}

	for (i = 0; i < UDMA_GPU_VENDOR_MAX; i++) {
		if (g_gpu_p2p_ops[i]) {
			n++;
			found = i;
		}
	}
	if (n == 1)
		ops = udma_gpu_p2p_try_get_slot(found);
	mutex_unlock(&g_gpu_p2p_ops_lock);

	if (n == 0) {
		dev_err(dev, "GPU P2P provider not registered "
			"(load udma_metax_p2p_bridge.ko or udma_nv_p2p_bridge.ko, "
			"then set gpu_vendor=metax|nvidia if both are loaded)\n");
		return ERR_PTR(-ENODEV);
	}
	if (n > 1) {
		dev_err(dev,
			"both metax and nvidia GPU providers are registered; "
			"set gpu_vendor=metax or gpu_vendor=nvidia\n");
		return ERR_PTR(-EINVAL);
	}
	if (!ops)
		return ERR_PTR(-ENODEV);
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

	ops = udma_gpu_p2p_ops_get_selected(dev->dev);
	if (IS_ERR(ops))
		return PTR_ERR(ops);

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
	seg->gpu.vendor = ops->vendor;
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
		ops = udma_gpu_p2p_ops_get_slot(seg->gpu.vendor);
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
