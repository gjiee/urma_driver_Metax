# SPDX-License-Identifier: GPL-2.0+

# GPU P2P: udma.ko is vendor-neutral (no nv-p2p.h / no metax_p2p.h).
# Missing vendor headers must NOT disable GPU pin — the other card
# still needs the same udma.ko. Bridges are optional extra .ko files.
NV_P2P_HEADER_PATH ?=
METAX_P2P_HEADER_PATH ?=
HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)
HAVE_METAX_P2P_HEADER := $(if $(wildcard $(METAX_P2P_HEADER_PATH)/metax_p2p.h),1,0)

ifeq ($(HAVE_NV_P2P_HEADER),1)
ccflags-y += -I$(NV_P2P_HEADER_PATH)
endif
ifeq ($(HAVE_METAX_P2P_HEADER),1)
ccflags-y += -I$(METAX_P2P_HEADER_PATH) -DHAVE_METAX_P2P_HEADER=1
endif

udma-$(CONFIG_UB_UDMA) := udma_main.o udma_cmd.o udma_common.o udma_ctx.o udma_db.o \
	     udma_tid.o udma_eq.o udma_jfc.o \
	     udma_ctrlq_tp.o udma_eid.o udma_ctl.o udma_segment.o udma_seg_tree.o \
	     udma_dfx.o udma_jfs.o udma_jetty.o udma_jfr.o udma_mue.o udma_jetty_group.o \
	     udma_safe_mode.o udma_gpu_p2p.o

obj-m := udma.o
ifeq ($(HAVE_METAX_P2P_HEADER),1)
obj-m += udma_metax_p2p_bridge.o
endif
ifeq ($(HAVE_NV_P2P_HEADER),1)
obj-m += udma_nv_p2p_bridge.o
endif
