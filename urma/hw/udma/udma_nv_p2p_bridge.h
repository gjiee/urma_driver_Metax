/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-2-Clause) */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4
 *
 * udma_nv_p2p_bridge: 独立的小模块，专门负责直接链接闭源 nvidia.ko 的
 * nvidia_p2p_* 符号，参照传统 RDMA 厂商（Mellanox nvidia-peermem.ko）
 * 的做法 —— 那个模块本身就是 MODULE_LICENSE("Dual BSD/GPL")，不是纯 GPL。
 *
 * 为什么要单独一个模块（而不是让 udma.ko 直接调）:
 *   udma.ko 是 MODULE_LICENSE("GPL")。GPL 模块跨边界静态链接 proprietary
 *   模块（nvidia.ko）的符号时，insmod 阶段的静态符号解析对 GPL taint /
 *   CONFIG_MODVERSIONS CRC 都有额外校验，任何一处不吻合，报错都是统一的
 *   "Unknown symbol X (err -2)"，没法从报错本身区分具体原因，
 *   也没有充分证据表明纯改一个 license 字符串就能规避——真正干净的做法
 *   是照抄业界已经验证过十几年的模式：单独切一个 Dual BSD/GPL 的小模块
 *   专门做这层"胶水"，对外只暴露自己的 wrapper 符号（本文件声明的这几个），
 *   udma.ko 只依赖这个我们自己完全掌控、跟自己在同一次 kbuild 里编译、
 *   Module.symvers/CRC 天然一致的 wrapper 模块，不直接碰 nvidia.ko。
 *
 * 加载顺序: nvidia.ko -> udma_nv_p2p_bridge.ko -> udma.ko
 * （softdep 只有 modprobe 认，insmod 需要手动保证顺序）
 */

#ifndef __UDMA_NV_P2P_BRIDGE_H__
#define __UDMA_NV_P2P_BRIDGE_H__

#include <linux/types.h>

struct nvidia_p2p_page_table;

/*
 * 包一层 nvidia_p2p_get_pages(p2p_token=0, va_space=0, ...)
 * @free_callback: 必须非 NULL（nvidia_p2p_get_pages 强制要求），
 *                 GPU 内存被 App 提前释放时异步调用
 */
int udma_nv_p2p_get_pages(uint64_t gpu_va, uint64_t length,
			  struct nvidia_p2p_page_table **page_table,
			  void (*free_callback)(void *data), void *data);

/* 包一层 nvidia_p2p_put_pages(p2p_token=0, va_space=0, ...) */
int udma_nv_p2p_put_pages(uint64_t gpu_va, struct nvidia_p2p_page_table *page_table);

/* 包一层 nvidia_p2p_free_page_table()，官方文档标注是 no-op */
int udma_nv_p2p_free_page_table(struct nvidia_p2p_page_table *page_table);

#endif /* __UDMA_NV_P2P_BRIDGE_H__ */
