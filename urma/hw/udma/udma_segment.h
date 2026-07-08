/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 * 扩展 udma_segment 以保存 GPU pin 状态（page_table / gpu_va）
 */

#ifndef __UDMA_SEGMENT_H__
#define __UDMA_SEGMENT_H__

#include "udma_dev.h"

//GPU兼容测试补丁: NV P2P 页表类型，完整定义在 nv-p2p.h
struct nvidia_p2p_page_table;

/*
 * GPU兼容测试补丁: GPU 段 pin 后的持久化状态
 * page_table:  nvidia_p2p_get_pages() 返回值，unpin 时 nvidia_p2p_put_pages() 归还
 * gpu_va:      64KB 对齐后的 pin 起始 VA（= seg->addr），nvidia_p2p_put_pages()
 *              的 virtual_address 参数也用它
 * mapped_size: 实际 pin 的字节长度（= seg->length）
 * force_freed: NV 侧异步 free_callback 被调用后置位（GPU 内存被 App 提前释放，
 *              而 URMA 段还没 unregister）。置位后 unpin 路径不再调用
 *              nvidia_p2p_put_pages()（page_table 已经被 NV 内部释放，
 *              重复调用未定义），只做本地清理。
 *              这是 TODO：正确做法应该在此时通知 ubcore/上层使该段失效，
 *              当前测试补丁只打印错误日志，不做进一步传播。
 */
struct udma_gpu_p2p_mem {
	struct nvidia_p2p_page_table *page_table;
	uint64_t gpu_va;
	uint64_t mapped_size;
	bool force_freed;
};

struct udma_segment {
	struct ubcore_target_seg core_tseg;
	struct udma_umem *umem;
	struct page *first_page;
	uint32_t token_value;
	bool token_value_valid;
	bool kernel_mode;
	//GPU兼容测试补丁: true 表示本段通过 UBCORE_REG_SEG_GPU_VA 注册
	bool is_gpu_seg;
	uint32_t tid;
	uint64_t addr;
	uint64_t length;
	uint64_t vm_start;
	uint64_t vm_end;
	//GPU兼容测试补丁: 仅 is_gpu_seg 时有效
	struct udma_gpu_p2p_mem gpu;
};

static inline struct udma_segment *to_udma_seg(struct ubcore_target_seg *seg)
{
	return container_of(seg, struct udma_segment, core_tseg);
}

struct ubcore_target_seg *udma_register_seg(struct ubcore_device *ub_dev,
					    struct ubcore_seg_cfg *cfg,
					    struct ubcore_udata *udata);
int udma_unregister_seg(struct ubcore_target_seg *seg);
struct ubcore_target_seg *udma_import_seg(struct ubcore_device *dev,
					  struct ubcore_target_seg_cfg *cfg,
					  struct ubcore_udata *udata);
int udma_unimport_seg(struct ubcore_target_seg *tseg);
void udma_destroy_seg_tree_table(void);

#endif /* __UDMA_SEGMENT_H__ */
