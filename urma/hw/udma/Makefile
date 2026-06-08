# SPDX-License-Identifier: GPL-2.0+

NV_P2P_HEADER_PATH ?=
NV_MODULE_SYMVERS ?=

ccflags-$(CONFIG_UB_UDMA_NV_P2P) += -I$(NV_P2P_HEADER_PATH)
ifneq ($(NV_MODULE_SYMVERS),)
KBUILD_EXTRA_SYMBOLS += $(NV_MODULE_SYMVERS)
endif

udma-$(CONFIG_UB_UDMA) := udma_main.o udma_cmd.o udma_common.o udma_ctx.o udma_db.o \
	     udma_tid.o udma_eq.o udma_jfc.o \
	     udma_ctrlq_tp.o udma_eid.o udma_ctl.o udma_segment.o udma_seg_tree.o \
	     udma_dfx.o udma_jfs.o udma_jetty.o udma_jfr.o udma_mue.o udma_jetty_group.o \
	     udma_safe_mode.o
udma-$(CONFIG_UB_UDMA_NV_P2P) += udma_gpu_p2p.o
udma-$(CONFIG_UB_UDMA_GPU_P2P_DEBUG) += udma_gpu_p2p_debug.o

obj-m := udma.o
