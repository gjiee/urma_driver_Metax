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
/*
static int udma_nv_p2p_bridge_get_pages(uint64_t gpu_va, uint64_t length,
					struct nvidia_p2p_page_table **page_table,
					void (*free_callback)(void *data), void *data)
{
	return nvidia_p2p_get_pages(0, 0, gpu_va, length, page_table,
				    free_callback, data);
}
*/
#define UDMA_NV_GPU_PAGE_SIZE SZ_64K

static int
udma_nv_p2p_bridge_get_pages(uint64_t gpu_va, uint64_t length,
                             struct nvidia_p2p_page_table **page_table,
                             void (*free_callback)(void *data),
                             void *data)
{
        struct nvidia_p2p_page_table *pt;
        uint64_t end;
        int ret;

        pr_info("UDMA NV P2P get_pages enter: "
                "gpu_va=0x%016llx len=0x%llx "
                "va_offset=0x%llx len_offset=0x%llx "
                "page_table_arg=%px old_pt=%px "
                "callback=%ps data=%px "
                "pid=%d tgid=%d comm=%s mm=%px "
                "interrupt=%d atomic=%d irqs_disabled=%d\n",
                (unsigned long long)gpu_va,
                (unsigned long long)length,
                (unsigned long long)(gpu_va & (UDMA_NV_GPU_PAGE_SIZE - 1)),
                (unsigned long long)(length & (UDMA_NV_GPU_PAGE_SIZE - 1)),
                page_table,
                page_table ? *page_table : NULL,
                (void *)free_callback,
                data,
                current->pid,
                current->tgid,
                current->comm,
                current->mm,
                in_interrupt(),
                in_atomic(),
                irqs_disabled());

        if (!page_table) {
                pr_err("UDMA NV P2P: page_table argument is NULL\n");
                return -EINVAL;
        }

        if (!gpu_va || !length) {
                pr_err("UDMA NV P2P: invalid range: va=0x%llx len=0x%llx\n",
                       (unsigned long long)gpu_va,
                       (unsigned long long)length);
                return -EINVAL;
        }

        end = gpu_va + length;
        if (end < gpu_va) {
                pr_err("UDMA NV P2P: address overflow: va=0x%llx len=0x%llx\n",
                       (unsigned long long)gpu_va,
                       (unsigned long long)length);
                return -EINVAL;
        }

        if (!IS_ALIGNED(gpu_va, UDMA_NV_GPU_PAGE_SIZE)) {
                pr_err("UDMA NV P2P: gpu_va is not 64KB aligned: "
                       "va=0x%llx offset=0x%llx\n",
                       (unsigned long long)gpu_va,
                       (unsigned long long)
                       (gpu_va & (UDMA_NV_GPU_PAGE_SIZE - 1)));
                return -EINVAL;
        }

        if (!IS_ALIGNED(length, UDMA_NV_GPU_PAGE_SIZE)) {
                pr_err("UDMA NV P2P: length is not 64KB aligned: "
                       "len=0x%llx offset=0x%llx\n",
                       (unsigned long long)length,
                       (unsigned long long)
                       (length & (UDMA_NV_GPU_PAGE_SIZE - 1)));
                return -EINVAL;
        }

        /*
         * 新驱动的普通 get_pages 不允许 NULL callback；
         * nv-p2p.h 也要求 data 为 non-NULL。
         */
        if (!free_callback) {
                pr_err("UDMA NV P2P: free_callback is NULL\n");
                return -EINVAL;
        }

        if (!data) {
                pr_err("UDMA NV P2P: callback data is NULL\n");
                return -EINVAL;
        }

        if (!current->mm) {
                pr_warn("UDMA NV P2P: current->mm is NULL; "
                        "get_pages may be running from a kernel thread/workqueue\n");
        }

        if (in_interrupt() || in_atomic()) {
                pr_warn("UDMA NV P2P: called from non-sleepable context; "
                        "nvidia_p2p_get_pages may sleep\n");
        }

        if (*page_table) {
                pr_warn("UDMA NV P2P: *page_table is already non-NULL: %px\n",
                        *page_table);
        }

        pr_info("UDMA NV P2P: calling NVIDIA API range=[0x%llx, 0x%llx)\n",
                (unsigned long long)gpu_va,
                (unsigned long long)end);

        ret = nvidia_p2p_get_pages(0, 0, gpu_va, length, page_table,
                                   free_callback, data);

        if (ret) {
                pr_err("UDMA NV P2P: nvidia_p2p_get_pages failed: "
                       "va=0x%llx len=0x%llx ret=%d%s\n",
                       (unsigned long long)gpu_va,
                       (unsigned long long)length,
                       ret,
                       ret == -EINVAL ? " (EINVAL)" :
                       ret == -ENOMEM ? " (ENOMEM)" :
                       ret == -EIO ? " (EIO)" : "");
                return ret;
        }

        pt = *page_table;
        if (!pt) {
                pr_err("UDMA NV P2P: API returned success but page_table is NULL\n");
                return -EIO;
        }

        pr_info("UDMA NV P2P get_pages success: "
                "pt=%px version=0x%08x page_size_id=%u "
                "entries=%u pages=%px gpu_uuid=%px\n",
                pt,
                pt->version,
                pt->page_size,
                pt->entries,
                pt->pages,
                pt->gpu_uuid);

        if (!NVIDIA_P2P_PAGE_TABLE_VERSION_COMPATIBLE(pt))
                pr_warn("UDMA NV P2P: incompatible page-table version 0x%08x\n",
                        pt->version);

        if (pt->entries && pt->pages) {
                struct nvidia_p2p_page *first = pt->pages[0];
                struct nvidia_p2p_page *last = pt->pages[pt->entries - 1];

                pr_info("UDMA NV P2P: first_page=%px pa=0x%llx "
                        "last_page=%px pa=0x%llx\n",
                        first,
                        first ?
                        (unsigned long long)first->physical_address : 0ULL,
                        last,
                        last ?
                        (unsigned long long)last->physical_address : 0ULL);
        }

        return 0;
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
