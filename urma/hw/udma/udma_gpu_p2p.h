/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 *
 * 环节: 用户 flag 识别 + GPU pin 接口声明
 * 用户态在 register_seg 时设置 flag.value 的 bit15，内核据此走 GPU 路径。
 *
 * 为什么是 bit15 而不是 bit14:
 *   参见 include/ub/urma/ubcore_types.h 中 union ubcore_reg_seg_flag：
 *     token_policy:3 cacheable:1 dsva:1 access:6 non_pin:1 user_iova:1
 *     token_id_valid:1 pa:1 reserved:17
 *   bit14 已经是内核私有的 flag.bs.pa，用户态 ioctl 传 bit14 会被
 *   ubcore_check_register_seg_para() 当成 pa==1 直接拒绝(-EINVAL)。
 *   bit15 是 reserved 区间(bit15~31)的第一位，未被任何已知字段占用。
 */

#ifndef __UDMA_GPU_P2P_H__
#define __UDMA_GPU_P2P_H__

#include <linux/types.h>
#include <ub/urma/ubcore_types.h>
#include "udma_dev.h"
#include "udma_segment.h"

//GPU兼容测试补丁: 1=启用本文件 API；0=编译为空 stub 返回 -ENOTSUPP
#define UDMA_GPU_P2P_ENABLE	1

/*
 * GPU兼容测试补丁: 用户 ABI — register_seg 的 flag.value 第 15 位
 * 调用方: uburma_cmd_register_seg → cfg.flag.value（来自 ioctl in.flag）
 * 用户态需: seg_cfg.flag.value |= UBCORE_REG_SEG_GPU_VA
 * 并跳过 ummu_grant()，因 GPU VA 不是 CPU 进程虚拟地址
 */
#define UBCORE_REG_SEG_GPU_VA		BIT(15)

//GPU兼容测试补丁: NVIDIA P2P 仅支持 64KB 页，与 NVIDIA_P2P_PAGE_SIZE_64KB 一致
#define UDMA_GPU_P2P_PAGE_SHIFT		16
#define UDMA_GPU_P2P_PAGE_SIZE		((uint64_t)1 << UDMA_GPU_P2P_PAGE_SHIFT)
#define UDMA_GPU_P2P_PAGE_MASK		(~(UDMA_GPU_P2P_PAGE_SIZE - 1))

/*
 * GPU兼容测试补丁: 判断 ubcore_seg_cfg 是否为 GPU VA 注册
 * @cfg: register_seg 入参，含 va/len/flag/token_id
 * 返回 true 当 cfg->flag.value & UBCORE_REG_SEG_GPU_VA
 */
static inline bool udma_seg_cfg_is_gpu(const struct ubcore_seg_cfg *cfg)
{
	return !!(cfg->flag.value & UBCORE_REG_SEG_GPU_VA);
}

/*
 * GPU兼容测试补丁: 判断已创建的 udma_segment 是否为 GPU 段
 * @seg: 内核段对象，is_gpu_seg 在 pin 成功或 init_seg_cfg 时设置
 */
static inline bool udma_seg_is_gpu(const struct udma_segment *seg)
{
	return seg->is_gpu_seg;
}

#if UDMA_GPU_P2P_ENABLE

/*
 * GPU兼容测试补丁: Pin GPU VA 并构建 SG 表
 * @dev:   udma 设备
 * @seg:   输出 umem->append.sgt、seg->gpu.*、seg->addr/length
 * @cfg:   用户 va/len/flag；va 会向下对齐到 64KB
 * 成功返回 0；失败自动 put_pages_persistent
 */
int udma_gpu_pin_seg_pages(struct udma_dev *dev, struct udma_segment *seg,
			   struct ubcore_seg_cfg *cfg);

/*
 * GPU兼容测试补丁: 释放 GPU pin 与 SG
 * @seg: 必须 is_gpu_seg；调用 nvidia_p2p_put_pages_persistent + sg_free_table
 */
void udma_gpu_unpin_seg_pages(struct udma_segment *seg);

#else

static inline int udma_gpu_pin_seg_pages(struct udma_dev *dev,
					 struct udma_segment *seg,
					 struct ubcore_seg_cfg *cfg)
{
	return -ENOTSUPP;
}

static inline void udma_gpu_unpin_seg_pages(struct udma_segment *seg) {}

#endif

#endif /* __UDMA_GPU_P2P_H__ */
