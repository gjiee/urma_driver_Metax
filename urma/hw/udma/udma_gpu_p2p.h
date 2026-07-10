/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright(c) 2025 HiSilicon Technologies CO., Ltd. All rights reserved. */
/*
 * GPU兼容测试补丁 — SP4 (openEuler-24.03-LTS-SP4 同步版)
 *
 * 环节: 用户 GPU 标记识别 + GPU pin 接口声明
 *
 * ABI 设计: GPU 标记走 ubcore_udata->udrv_data 私有通道（HW driver 自定义
 * 命令数据，ubcore 全程当成 void* 不解析），不占用 cfg->flag.value 的任何
 * 位。用户态在 urma_seg_cfg_t 里新增一个普通字段 is_gpu_seg（不是
 * bitfield），udma_u_register_seg() 把它打进 struct udma_register_seg_ucmd
 * 通过 urma_cmd_udrv_priv_t.in_addr 传给内核。
 *
 * 为什么不用 flag.value 的 bit:
 *   参见 include/ub/urma/ubcore_types.h 中 union ubcore_reg_seg_flag：
 *     token_policy:3 cacheable:1 dsva:1 access:6 non_pin:1 user_iova:1
 *     token_id_valid:1 pa:1 reserved:17
 *   bit14 是内核私有的 flag.bs.pa，用户态 ioctl 传 bit14 会被
 *   ubcore_check_register_seg_para() 当成 pa==1 直接拒绝(-EINVAL)。
 *   即便换成 bit15(reserved 区间起点)理论上安全，用户态改起来也要动
 *   flag 语义；改用 udrv_data 侧信道后彻底不用管 flag 里哪个位空闲，
 *   ubcore 校验路径完全不受影响。
 *
 * UBCORE_REG_SEG_GPU_VA 保留作为 udma.ko 内部私有标记（只在
 * udma_register_seg() 内部对本地 cfg 拷贝打标记，从不经过 ubcore 校验，
 * 也不会传回用户态），位号选哪个都不影响正确性。
 */

#ifndef __UDMA_GPU_P2P_H__
#define __UDMA_GPU_P2P_H__

#include <linux/types.h>
#include <linux/uaccess.h>
#include <ub/urma/ubcore_types.h>
#include "udma_dev.h"
#include "udma_segment.h"

/*
 * GPU兼容测试补丁: 1=启用本文件 API（需要 nv-p2p.h，即 NV_P2P_HEADER_PATH
 * 指向的目录存在）；0=编译为空 stub 返回 -ENOTSUPP，完全不需要任何
 * NVIDIA 头文件，用于没有 GPU/没装 NVIDIA 驱动源码的机器（比如只做
 * client 端、没有 GPU 的对端节点）也能编译出同一份 udma.ko。
 *
 * 默认 1（保持原行为）；OOT Makefile 在检测不到 nv-p2p.h 时，会通过
 * ccflags-y += -DUDMA_GPU_P2P_ENABLE=0 覆盖成 0，见 urma/hw/udma/Makefile。
 */
#ifndef UDMA_GPU_P2P_ENABLE
#define UDMA_GPU_P2P_ENABLE	1
#endif

/*
 * GPU兼容测试补丁: udma.ko 内部私有标记位，仅用于 udma_register_seg()
 * 内部本地 cfg 拷贝（ubcore_register_seg 传下来的 tmp_cfg），从不经过
 * ubcore 校验、也不回传用户态，位号选哪个都无所谓。
 */
#define UBCORE_REG_SEG_GPU_VA		BIT(15)

/*
 * GPU兼容测试补丁: 用户态 → 内核 GPU 标记的 wire 格式
 * 必须与 UMDK 侧 struct udma_register_seg_ucmd 布局一致
 * (src/urma/hw/udma/udma_u_segment.c)
 */
struct udma_register_seg_ucmd {
	uint32_t is_gpu_seg;
	uint32_t reserved;
};

/*
 * GPU兼容测试补丁: 从 udata->udrv_data 私有通道读取 GPU 标记
 * @udata: udma_register_seg() 的第三个入参，udata->udrv_data->in_addr
 *         指向用户态 struct udma_register_seg_ucmd
 * 任何解析失败都返回 false（走 HOST 段路径），不影响非 GPU 场景
 */
static inline bool udma_seg_ucmd_is_gpu(struct ubcore_udata *udata)
{
	struct udma_register_seg_ucmd ucmd = { 0 };
	unsigned long byte;

	if (!udata || !udata->udrv_data || !udata->udrv_data->in_addr)
		return false;

	byte = copy_from_user(&ucmd, (void *)(uintptr_t)udata->udrv_data->in_addr,
			      min_t(uint32_t, udata->udrv_data->in_len, sizeof(ucmd)));
	if (byte)
		return false;

	return ucmd.is_gpu_seg != 0;
}

/*
 * GPU兼容测试补丁: 闭源 nvidia.ko 符号依赖说明
 *
 * nvidia_p2p_get_pages_persistent / nvidia_p2p_put_pages_persistent /
 * nvidia_p2p_build_sg_table_phys 在闭源驱动上 insmod 会报
 * "Unknown symbol ... (err -2)"（build_sg_table_phys 本来就是本项目
 * 在开源 fork 里自己加的，*_persistent 系列在闭源驱动上同样未导出）。
 *
 * 换成经典 get_pages/put_pages 之后依然 "Unknown symbol"——udma.ko
 * 纯 GPL、nvidia.ko proprietary，跨这个边界的静态符号解析（GPL taint +
 * CONFIG_MODVERSIONS CRC 校验）不干净。
 *
 * 单独切一个 Dual BSD/GPL 的 udma_nv_p2p_bridge.ko 直接链 nvidia.ko
 * 符号（v1）也不够——inherit_taint() 会把 TAINT_PROPRIETARY_MODULE
 * 无条件传给这个桥接模块（跟它自己声明什么 license 无关），udma.ko
 * 再去静态链接**它**，撞上一模一样的墙。
 *
 * 最终修复（v2，见 udma_nv_p2p_bridge.h）: 反转控制方向。udma.ko 自己
 * 导出两个普通 GPL 符号 udma_register_gpu_p2p_ops()/
 * udma_unregister_gpu_p2p_ops()（本文件 .c 实现），udma_nv_p2p_bridge.ko
 * 在自己 module_init() 时主动调用它们，把一组函数指针注册进来。
 * udma.ko 后续调 NV P2P 全部走函数指针，不 import 任何 bridge/nvidia
 * 符号——这正是 Mellanox ib_core.ko 与 nvidia-peermem.ko 之间的真实
 * 关系（ib_register_peer_memory_client 模式）。
 *
 * SG 表不依赖 NV 侧任何符号，直接在 udma_gpu_p2p.c 里用
 * page_table->pages[i]->physical_address（公开稳定 ABI 字段）自己拼——
 * 这是纯结构体字段读取，不涉及符号链接。
 */

//GPU兼容测试补丁: NVIDIA P2P 仅支持 64KB 页，与 NVIDIA_P2P_PAGE_SIZE_64KB 一致
#define UDMA_GPU_P2P_PAGE_SHIFT		16
#define UDMA_GPU_P2P_PAGE_SIZE		((uint64_t)1 << UDMA_GPU_P2P_PAGE_SHIFT)
#define UDMA_GPU_P2P_PAGE_MASK		(~(UDMA_GPU_P2P_PAGE_SIZE - 1))

/*
 * GPU兼容测试补丁: 判断 ubcore_seg_cfg 是否为 GPU VA 注册
 * @cfg: register_seg 入参，含 va/len/flag/token_id
 *
 * 注意: cfg->flag.value 里的 UBCORE_REG_SEG_GPU_VA 位是 udma_register_seg()
 * 在拿到 udata 后，通过 udma_seg_ucmd_is_gpu() 解析用户态 udrv_data 内容，
 * 本地打上去的（见 udma_segment.c）——不是用户态直接从 ioctl 传上来的
 * flag 位，也从未经过 ubcore_check_register_seg_para() 校验。
 * 这里只是内部各分支（udma_init_seg_cfg / udma_pin_pages_and_ioummu_map /
 * legacy 路径）统一判定入口，语义上等价于"这是不是 GPU 段"。
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
 * @seg: 必须 is_gpu_seg；通过已注册 provider 的 put_pages 函数指针
 *       转发到 nvidia_p2p_put_pages，再 sg_free_table
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
