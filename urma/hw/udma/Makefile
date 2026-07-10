# SPDX-License-Identifier: GPL-2.0+

# GPU兼容测试补丁 — SP4
# NV_P2P_HEADER_PATH 必须指向含 nv-p2p.h 的目录: .../kernel-open/nvidia
# ccflags 在本文件生效；KBUILD_EXTRA_SYMBOLS 须在仓库根 Makefile 设置
NV_P2P_HEADER_PATH ?=

# FIX: 没有 GPU/没装 NVIDIA 驱动源码的机器（比如集群里作为纯 client、
# 不带 GPU 的对端节点）也需要编译出跟带 GPU 那台节点完全一样的
# udma.ko/ubcore.ko，否则两边内核模块版本不一致，跨节点 MAD/TP 协议
# 交互会出问题（这不是假设，是真实踩过的坑）。
# 之前这里对 NV 头文件的存在是硬性要求 —— 只要 NV_P2P_HEADER_PATH 下
# 没有 nv-p2p.h，整个 udma.ko 都编译不出来。现在改成自动降级：探测到
# 没有 nv-p2p.h 就自动把 UDMA_GPU_P2P_ENABLE 编译期覆盖成 0（见
# udma_gpu_p2p.h/.c），GPU pin 相关的几个函数编译成 -ENOTSUPP 的
# stub，其它所有 HOST 段功能不受影响；同时不编译
# udma_nv_p2p_bridge.ko（它必须直接链 nvidia.ko，没有 GPU 的机器上
#本来就装不了、也用不上）。
HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)

ifeq ($(HAVE_NV_P2P_HEADER),1)
ccflags-y += -I$(NV_P2P_HEADER_PATH)
else
ccflags-y += -DUDMA_GPU_P2P_ENABLE=0
endif

udma-$(CONFIG_UB_UDMA) := udma_main.o udma_cmd.o udma_common.o udma_ctx.o udma_db.o \
	     udma_tid.o udma_eq.o udma_jfc.o \
	     udma_ctrlq_tp.o udma_eid.o udma_ctl.o udma_segment.o udma_seg_tree.o \
	     udma_dfx.o udma_jfs.o udma_jetty.o udma_jfr.o udma_mue.o udma_jetty_group.o \
	     udma_safe_mode.o udma_gpu_p2p.o

# GPU兼容测试补丁: 独立的 Dual BSD/GPL 桥接模块，专门链接闭源 nvidia.ko
# 的符号。udma.ko 不 import 桥接模块的任何符号——两者关系是桥接模块
# 反过来调 udma.ko 导出的 udma_register_gpu_p2p_ops() 注册函数指针
# （见 udma_nv_p2p_bridge.h），不是静态符号依赖。放在同一个 Makefile
# 里编译成两个 .ko，同一次 kbuild/modpost 互相可见，不需要额外
# KBUILD_EXTRA_SYMBOLS 拼接。单文件模块，Kbuild 自动用
# udma_nv_p2p_bridge.c 生成 udma_nv_p2p_bridge.ko
#
# FIX: udma_nv_p2p_bridge.ko 直接静态链接 nvidia.ko 的符号，没有
# nv-p2p.h（进而也不会有配套的 nvidia.ko/Module.symvers）就没法编译，
# 也没有意义（没有 GPU 的机器装不了 nvidia.ko）——探测不到就直接不把
# 它加进 obj-m，不影响 udma.o 本体的编译。
obj-m := udma.o
ifeq ($(HAVE_NV_P2P_HEADER),1)
obj-m += udma_nv_p2p_bridge.o
endif
