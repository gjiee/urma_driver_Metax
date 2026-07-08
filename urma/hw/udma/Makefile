# SPDX-License-Identifier: GPL-2.0+

# GPU兼容测试补丁 — SP4
# NV_P2P_HEADER_PATH 必须指向含 nv-p2p.h 的目录: .../kernel-open/nvidia
# ccflags 在本文件生效；KBUILD_EXTRA_SYMBOLS 须在仓库根 Makefile 设置
NV_P2P_HEADER_PATH ?=

ifneq ($(NV_P2P_HEADER_PATH),)
ccflags-y += -I$(NV_P2P_HEADER_PATH)
endif

udma-$(CONFIG_UB_UDMA) := udma_main.o udma_cmd.o udma_common.o udma_ctx.o udma_db.o \
	     udma_tid.o udma_eq.o udma_jfc.o \
	     udma_ctrlq_tp.o udma_eid.o udma_ctl.o udma_segment.o udma_seg_tree.o \
	     udma_dfx.o udma_jfs.o udma_jetty.o udma_jfr.o udma_mue.o udma_jetty_group.o \
	     udma_safe_mode.o udma_gpu_p2p.o

obj-m := udma.o
