// SPDX-License-Identifier: (GPL-2.0+ OR BSD-2-Clause)
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4
 *
 * 独立小模块，直接静态链接闭源 nvidia.ko 的 nvidia_p2p_get_pages /
 * nvidia_p2p_put_pages / nvidia_p2p_free_page_table。License 用
 * "Dual BSD/GPL"，跟 nvidia-peermem.ko 完全一致。
 *
 * 注意: 即便声明 Dual BSD/GPL，本模块因为链了 nvidia.ko（proprietary）的
 * 符号，仍然会被内核 inherit_taint() 打上 TAINT_PROPRIETARY_MODULE
 * ——这是内核层面按依赖边传播的，跟自己声明什么 license 无关，改不了。
 *
 * 所以本模块**不再对外 EXPORT_SYMBOL** 任何东西给 udma.ko 静态链接
 * （那样 udma.ko 会重蹈 nvidia 那次的覆辙）。而是反过来，在
 * module_init() 时主动调用 udma.ko 导出的 udma_register_gpu_p2p_ops()，
 * 把自己的函数指针"注册"过去——udma.ko 后续全部通过函数指针调用，
 * 不做任何静态符号解析，taint 传播链在这里被切断。
 *
 * 加载顺序: nvidia.ko -> udma.ko -> udma_nv_p2p_bridge.ko
 * （必须最后加载，因为 init 时要反过来调 udma.ko 的导出符号）
 */

#define pr_fmt(fmt) "udma_nv_p2p_bridge: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <nv-p2p.h>

#include "udma_nv_p2p_bridge.h"

static int udma_nv_p2p_bridge_get_pages(uint64_t gpu_va, uint64_t length,
					struct nvidia_p2p_page_table **page_table,
					void (*free_callback)(void *data), void *data)
{
	return nvidia_p2p_get_pages(0, 0, gpu_va, length, page_table,
				    free_callback, data);
}

static int udma_nv_p2p_bridge_put_pages(uint64_t gpu_va,
					struct nvidia_p2p_page_table *page_table)
{
	return nvidia_p2p_put_pages(0, 0, gpu_va, page_table);
}

static int udma_nv_p2p_bridge_free_page_table(struct nvidia_p2p_page_table *page_table)
{
	return nvidia_p2p_free_page_table(page_table);
}

static const struct udma_gpu_p2p_ops udma_nv_p2p_bridge_ops = {
	.get_pages = udma_nv_p2p_bridge_get_pages,
	.put_pages = udma_nv_p2p_bridge_put_pages,
	.free_page_table = udma_nv_p2p_bridge_free_page_table,
	.owner = THIS_MODULE,
};

static int __init udma_nv_p2p_bridge_init(void)
{
	int ret;

	/*
	 * GPU兼容测试补丁: 调 udma.ko 导出的注册接口（普通 GPL 符号，非
	 * proprietary），把上面这组函数指针交给它。udma.ko 要求已经加载，
	 * 否则这里 -ENOENT（符号都找不到）或者其他错误。
	 */
	ret = udma_register_gpu_p2p_ops(&udma_nv_p2p_bridge_ops);
	if (ret) {
		pr_err("udma_register_gpu_p2p_ops failed ret=%d "
		       "(is udma.ko loaded first?)\n", ret);
		return ret;
	}

	pr_info("loaded, registered GPU P2P ops with udma.ko\n");
	return 0;
}

static void __exit udma_nv_p2p_bridge_exit(void)
{
	udma_unregister_gpu_p2p_ops(&udma_nv_p2p_bridge_ops);
	pr_info("unloaded\n");
}

module_init(udma_nv_p2p_bridge_init);
module_exit(udma_nv_p2p_bridge_exit);

/*
 * GPU兼容测试补丁: 加载顺序依赖，只有 modprobe 会认；insmod 需要手动
 * 按顺序: nvidia.ko -> udma.ko -> udma_nv_p2p_bridge.ko
 * （注意跟 v1 相反，本模块现在必须最后装）
 */
MODULE_SOFTDEP("pre: nvidia udma");
MODULE_DESCRIPTION("UDMA <-> NVIDIA GPUDirect P2P bridge (registers callback ops with udma.ko, mirrors ib_core/nvidia-peermem pattern)");
MODULE_AUTHOR("HiSilicon Tech. Co., Ltd.");
MODULE_LICENSE("Dual BSD/GPL");
