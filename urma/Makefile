# SPDX-License-Identifier: GPL-2.0+
#
# In-kernel: drivers/ub/Makefile 挂 urma/ 与 urma/hw/udma/
# OOT 仅 urma:  cd urma && make  -> build/*.ko
#
# GPU vendor trees are optional. Override any of:
#   make NV_P2P_HEADER_PATH=/path/to/dir-with-nv-p2p.h
#   make NV_MODULE_SYMVERS=/path/to/nvidia/Module.symvers
#   make METAX_SRC=/path/to/metax-driver
#   make METAX_MODULE_SYMVERS=/path/to/metax/Module.symvers
#   make UBASE_MODULE_SYMVERS=/path/to/ubase/Module.symvers

ifeq ($(KERNELRELEASE),)

BUILD_DIR ?= $(CURDIR)/build
KDIR      ?= /lib/modules/$(shell uname -r)/build
REPO_ROOT ?= $(abspath $(CURDIR)/..)
UNAME_R   := $(shell uname -r)
UNAME_M   := $(shell uname -m)

# NVIDIA: first existing nv-p2p.h. Empty if none — do not hard-require
# a lab checkout such as ../nv_module_netlab.
NV_P2P_HEADER_PATH ?= $(patsubst %/nv-p2p.h,%,$(firstword \
	$(wildcard $(REPO_ROOT)/../nv_module_netlab/kernel-open/nvidia/nv-p2p.h) \
	$(wildcard /usr/src/nvidia-*/nvidia/nv-p2p.h)))

NV_MODULE_SYMVERS ?= $(firstword \
	$(if $(NV_P2P_HEADER_PATH),$(wildcard $(NV_P2P_HEADER_PATH)/Module.symvers)) \
	$(wildcard /var/lib/dkms/nvidia/*/$(UNAME_R)*/$(UNAME_M)/module/Module.symvers) \
	$(wildcard /var/lib/dkms/nvidia/*/$(UNAME_R)*/Module.symvers) \
	$(wildcard /usr/src/nvidia-*/Module.symvers))

# MetaX: first existing Module.symvers. Empty if the GPU driver was
# not built on this machine — override with METAX_MODULE_SYMVERS=.
METAX_SRC ?=
METAX_MODULE_SYMVERS ?= $(firstword \
	$(if $(METAX_SRC),$(wildcard $(METAX_SRC)/Module.symvers)) \
	$(wildcard /usr/src/metax-*/Module.symvers) \
	$(wildcard /usr/src/mxcd*/Module.symvers) \
	$(wildcard /var/lib/dkms/metax/*/$(UNAME_R)*/$(UNAME_M)/module/Module.symvers) \
	$(wildcard /var/lib/dkms/metax/*/$(UNAME_R)*/Module.symvers) \
	$(wildcard /var/lib/dkms/mxcd/*/$(UNAME_R)*/$(UNAME_M)/module/Module.symvers) \
	$(wildcard /var/lib/dkms/mxcd/*/$(UNAME_R)*/Module.symvers) \
	$(wildcard /lib/modules/$(UNAME_R)/extra/metax/Module.symvers))

UBASE_MODULE_SYMVERS ?= $(firstword \
	$(wildcard $(REPO_ROOT)/ubase/Module.symvers))

# Only pass files that exist. A missing path in KBUILD_EXTRA_SYMBOLS
# makes kbuild warn/fail ("Symbol version dump is missing").
NV_P2P_HEADER_PATH := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),$(abspath $(NV_P2P_HEADER_PATH)),)
NV_MODULE_SYMVERS := $(if $(wildcard $(NV_MODULE_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),)
METAX_MODULE_SYMVERS := $(if $(wildcard $(METAX_MODULE_SYMVERS)),$(abspath $(METAX_MODULE_SYMVERS)),)
UBASE_MODULE_SYMVERS := $(if $(wildcard $(UBASE_MODULE_SYMVERS)),$(abspath $(UBASE_MODULE_SYMVERS)),)

export NV_P2P_HEADER_PATH
export NV_MODULE_SYMVERS
export METAX_MODULE_SYMVERS
export UBASE_MODULE_SYMVERS

.PHONY: all modules clean check-symvers show-vars
all: modules

HAVE_NV_P2P_HEADER := $(if $(NV_P2P_HEADER_PATH),1,0)
HAVE_NV_SYMVERS := $(if $(and $(filter 1,$(HAVE_NV_P2P_HEADER)),$(NV_MODULE_SYMVERS)),1,0)

check-symvers:
	@echo "NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)"
	@echo "NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)"
	@echo "METAX_SRC=$(METAX_SRC)"
	@echo "METAX_MODULE_SYMVERS=$(METAX_MODULE_SYMVERS)"
	@echo "UBASE_MODULE_SYMVERS=$(UBASE_MODULE_SYMVERS)"
ifeq ($(HAVE_NV_P2P_HEADER),0)
	@echo "NOTE: no nv-p2p.h — skip udma_nv_p2p_bridge.ko; udma.ko still builds."
endif
ifeq ($(METAX_MODULE_SYMVERS),)
	@echo "NOTE: no MetaX Module.symvers — udma_metax_p2p_bridge.ko still builds,"
	@echo "      but insmod may fail with Unknown symbol if CONFIG_MODVERSIONS=y."
	@echo "      Fix: make METAX_MODULE_SYMVERS=/path/to/metax/Module.symvers"
endif

EXTRA_KBUILD_SYMVERS := $(NV_MODULE_SYMVERS) $(METAX_MODULE_SYMVERS) $(UBASE_MODULE_SYMVERS)

modules: check-symvers
	mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_UB=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
		UB_URMA_STANDALONE=1 \
		NV_P2P_HEADER_PATH="$(NV_P2P_HEADER_PATH)" \
		NV_MODULE_SYMVERS="$(NV_MODULE_SYMVERS)" \
		METAX_MODULE_SYMVERS="$(METAX_MODULE_SYMVERS)" \
		UBASE_MODULE_SYMVERS="$(UBASE_MODULE_SYMVERS)" \
		KBUILD_EXTRA_SYMBOLS="$(strip $(EXTRA_KBUILD_SYMVERS))" \
		modules
	@find $(CURDIR) -name '*.ko' ! -path '$(BUILD_DIR)/*' -print0 | \
		xargs -0 -r cp -t $(BUILD_DIR)/
	@echo "==> KO in $(BUILD_DIR):" && ls -1 $(BUILD_DIR)

show-vars: check-symvers

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf $(BUILD_DIR)

else

ifneq ($(wildcard $(NV_MODULE_SYMVERS)),)
KBUILD_EXTRA_SYMBOLS += $(NV_MODULE_SYMVERS)
endif
ifneq ($(wildcard $(METAX_MODULE_SYMVERS)),)
KBUILD_EXTRA_SYMBOLS += $(METAX_MODULE_SYMVERS)
endif
ifneq ($(wildcard $(UBASE_MODULE_SYMVERS)),)
KBUILD_EXTRA_SYMBOLS += $(UBASE_MODULE_SYMVERS)
endif

obj-$(CONFIG_UB_URMA)	+= ubcore/
obj-$(CONFIG_UB_URMA)	+= ubagg/

obj-$(CONFIG_UB_URMA)	+= uburma/
obj-$(CONFIG_UB_URMA)	+= ulp/

ifeq ($(UB_URMA_STANDALONE),1)
obj-$(CONFIG_UB_UDMA) += hw/udma/
endif

endif
