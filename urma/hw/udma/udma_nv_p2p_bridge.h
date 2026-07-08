/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-2-Clause) */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4
 *
 * v2 设计（真正解决 taint 传播问题，参照 ib_core / nvidia-peermem 的
 * "注册回调" 模式，不是简单切一个 Dual BSD/GPL 模块就完事）：
 *
 * v1 的问题：
 *   udma_nv_p2p_bridge.ko（Dual BSD/GPL）直接静态链接 nvidia.ko
 *   （proprietary）的符号后，内核 inherit_taint() 会**无条件**把
 *   TAINT_PROPRIETARY_MODULE 打到 udma_nv_p2p_bridge.ko 身上——
 *   不管它自己声明什么 license，taint 是顺着"谁链了谁的符号"这条边
 *   传播的，跟声明的 license 字符串无关。
 *   于是 udma.ko（纯 GPL）再去静态链接 udma_nv_p2p_bridge.ko 导出的
 *   符号时，撞上一模一样的墙：
 *     "module using GPL-only symbols uses symbols X from proprietary
 *      module udma_nv_p2p_bridge."
 *     "Unknown symbol X (err -2)"
 *
 * v2 修复：反转控制方向，彻底不让 udma.ko 静态解析任何来自
 * udma_nv_p2p_bridge.ko 的符号：
 *   - udma.ko 只导出两个"注册接口"（本头文件声明），本身普通 GPL 符号，
 *     不涉及任何 proprietary 符号，不会被打 taint；
 *   - udma_nv_p2p_bridge.ko 在自己的 module_init() 里调用
 *     udma_register_gpu_p2p_ops()，把一组指向自己内部函数的函数指针
 *     "喂"给 udma.ko；
 *   - udma.ko 后续调 NV P2P API 时，走的是**函数指针调用**，不是符号名
 *     链接——函数指针调用在内核模块加载器层面没有"resolve_symbol"这一步，
 *     自然不存在 taint 传播、GPL 校验、CRC 校验的问题。
 *
 * 这正是 Mellanox ib_core.ko（GPL）与 nvidia-peermem.ko（Dual BSD/GPL，
 * 且同样因链 nvidia.ko 被打 proprietary taint）之间的真实关系：
 * ib_core.ko 导出 ib_register_peer_memory_client()，nvidia-peermem.ko
 * 调用它注册一组回调，ib_core.ko 全程不静态链接 nvidia-peermem.ko
 * 的任何符号。
 *
 * 加载顺序因此变成: nvidia.ko -> udma.ko -> udma_nv_p2p_bridge.ko
 * （bridge 最后加载，因为它 init 时要反过来调 udma.ko 导出的注册函数）
 */

#ifndef __UDMA_NV_P2P_BRIDGE_H__
#define __UDMA_NV_P2P_BRIDGE_H__

#include <linux/types.h>
#include <linux/module.h>

struct nvidia_p2p_page_table;

/*
 * GPU兼容测试补丁: udma_nv_p2p_bridge.ko 实现这组回调，udma.ko 只通过
 * 函数指针调用，不做任何静态符号解析。
 * @owner: 桥接模块自己 (THIS_MODULE)，udma.ko 调用前 try_module_get(owner)，
 *         调用后 module_put(owner)，防止调用期间桥接模块被 rmmod。
 */
struct udma_gpu_p2p_ops {
	int (*get_pages)(uint64_t gpu_va, uint64_t length,
			 struct nvidia_p2p_page_table **page_table,
			 void (*free_callback)(void *data), void *data);
	int (*put_pages)(uint64_t gpu_va, struct nvidia_p2p_page_table *page_table);
	int (*free_page_table)(struct nvidia_p2p_page_table *page_table);
	struct module *owner;
};

/*
 * GPU兼容测试补丁: 由 udma.ko 导出实现 (udma_gpu_p2p.c)，普通 GPL 符号，
 * 不涉及任何 proprietary 符号，桥接模块在自己 module_init() 里调用。
 * 返回 0 成功；-EBUSY 表示已经有人注册过（同一时间只支持一个 provider）。
 */
int udma_register_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops);

/*
 * GPU兼容测试补丁: 桥接模块 module_exit() 里调用，@ops 必须跟注册时
 * 传的是同一个指针，防止误清除别的 provider（当前只支持单 provider，
 * 校验仍然有意义，便于未来扩展）。
 */
void udma_unregister_gpu_p2p_ops(const struct udma_gpu_p2p_ops *ops);

#endif /* __UDMA_NV_P2P_BRIDGE_H__ */
