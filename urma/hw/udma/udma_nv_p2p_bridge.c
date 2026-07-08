// SPDX-License-Identifier: (GPL-2.0+ OR BSD-2-Clause)
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4
 *
 * 独立小模块，直接静态链接闭源 nvidia.ko 的 nvidia_p2p_get_pages /
 * nvidia_p2p_put_pages / nvidia_p2p_free_page_table，对外只暴露自己的
 * wrapper 符号（udma_nv_p2p_*）。License 用 "Dual BSD/GPL"，跟
 * nvidia-peermem.ko 完全一致 —— 这是业界验证过十几年、GPL RDMA 网卡驱动
 * 间接使用闭源 nvidia_p2p_* API 的标准模式。
 *
 * 详见 udma_nv_p2p_bridge.h 顶部注释：为什么不让 udma.ko（纯 GPL）
 * 直接调 nvidia.ko（proprietary）。
 */

#define pr_fmt(fmt) "udma_nv_p2p_bridge: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <nv-p2p.h>

#include "udma_nv_p2p_bridge.h"

int udma_nv_p2p_get_pages(uint64_t gpu_va, uint64_t length,
			  struct nvidia_p2p_page_table **page_table,
			  void (*free_callback)(void *data), void *data)
{
	return nvidia_p2p_get_pages(0, 0, gpu_va, length, page_table,
				    free_callback, data);
}
EXPORT_SYMBOL(udma_nv_p2p_get_pages);

int udma_nv_p2p_put_pages(uint64_t gpu_va, struct nvidia_p2p_page_table *page_table)
{
	return nvidia_p2p_put_pages(0, 0, gpu_va, page_table);
}
EXPORT_SYMBOL(udma_nv_p2p_put_pages);

int udma_nv_p2p_free_page_table(struct nvidia_p2p_page_table *page_table)
{
	return nvidia_p2p_free_page_table(page_table);
}
EXPORT_SYMBOL(udma_nv_p2p_free_page_table);

static int __init udma_nv_p2p_bridge_init(void)
{
	pr_info("loaded, bridging udma.ko <-> nvidia.ko GPUDirect P2P API\n");
	return 0;
}

static void __exit udma_nv_p2p_bridge_exit(void)
{
	pr_info("unloaded\n");
}

module_init(udma_nv_p2p_bridge_init);
module_exit(udma_nv_p2p_bridge_exit);

/*
 * GPU兼容测试补丁: 加载顺序依赖，只有 modprobe 会认；insmod 需要手动
 * 先 insmod nvidia.ko，再 insmod udma_nv_p2p_bridge.ko，最后 insmod udma.ko
 */
MODULE_SOFTDEP("pre: nvidia");
MODULE_DESCRIPTION("UDMA <-> NVIDIA GPUDirect P2P bridge (mirrors nvidia-peermem.ko licensing pattern)");
MODULE_AUTHOR("HiSilicon Tech. Co., Ltd.");
MODULE_LICENSE("Dual BSD/GPL");
