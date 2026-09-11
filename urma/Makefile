# SPDX-License-Identifier: GPL-2.0+
#
# In-kernel: drivers/ub/Makefile 挂 urma/ 与 urma/hw/udma/
# OOT 仅 urma:  cd urma && make  -> build/*.ko

ifeq ($(KERNELRELEASE),)

BUILD_DIR ?= $(CURDIR)/build
KDIR      ?= /lib/modules/$(shell uname -r)/build
REPO_ROOT ?= $(abspath $(CURDIR)/..)

# NVIDIA: 实验室旁路树，与 urma_driver 同级
#   ../nv_module_netlab/kernel-open/nvidia/nv-p2p.h
NV_P2P_HEADER_PATH ?= $(abspath $(REPO_ROOT)/../nv_module_netlab/kernel-open/nvidia)
NV_MODULE_SYMVERS  ?= $(NV_P2P_HEADER_PATH)/Module.symvers

# MetaX: 已安装的 KMD，不走 /usr/src 头文件。
# 服务器 extra/ 里同时有 metax.ko.xz 和 Module.symvers：
#   /lib/modules/<kver>/extra/metax.ko.xz
#   /lib/modules/<kver>/extra/Module.symvers
# 模块名 metax，version 3.8.23，GPL。
# P2P 原型用本仓库 urma/hw/udma/udma_metax_p2p.h。
METAX_KMOD_DIR ?= /lib/modules/$(shell uname -r)/extra
METAX_KMOD ?= $(METAX_KMOD_DIR)/metax.ko.xz
METAX_MODULE_SYMVERS ?= $(METAX_KMOD_DIR)/Module.symvers

UBASE_MODULE_SYMVERS ?= $(REPO_ROOT)/ubase/Module.symvers

export NV_P2P_HEADER_PATH
export NV_MODULE_SYMVERS
export METAX_KMOD_DIR
export METAX_KMOD
export METAX_MODULE_SYMVERS
export UBASE_MODULE_SYMVERS

.PHONY: all modules clean check-gpu-deps show-vars
all: modules

# udma.ko always has GPU pin. Vendor bridges are optional:
#   NVIDIA: need nv-p2p.h (+ its Module.symvers to link nvidia.ko)
#   MetaX:  need extra/Module.symvers (and extra/metax.ko.xz)
HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)
HAVE_NV_SYMVERS := $(if $(and $(filter 1,$(HAVE_NV_P2P_HEADER)),$(wildcard $(NV_MODULE_SYMVERS))),1,0)
HAVE_METAX_KMOD := $(if $(wildcard $(METAX_KMOD)),1,0)
HAVE_METAX_SYMVERS := $(if $(wildcard $(METAX_MODULE_SYMVERS)),1,0)

check-gpu-deps:
ifeq ($(HAVE_NV_P2P_HEADER),0)
	@echo "WARNING: nv-p2p.h not found under $(NV_P2P_HEADER_PATH)."
	@echo "  udma.ko still builds with GPU pin (vendor-neutral)."
	@echo "  udma_nv_p2p_bridge.ko will NOT be built."
else
	@test -f "$(NV_MODULE_SYMVERS)" || { \
		echo "WARNING: $(NV_MODULE_SYMVERS) not found."; \
		echo "  udma_nv_p2p_bridge.ko will NOT be built."; \
	}
	@test ! -f "$(NV_MODULE_SYMVERS)" || grep -q nvidia_p2p_get_pages "$(NV_MODULE_SYMVERS)" || { \
		echo "WARNING: nvidia_p2p_get_pages not found in $(NV_MODULE_SYMVERS)."; }
endif
ifeq ($(HAVE_METAX_SYMVERS),0)
	@echo "WARNING: MetaX Module.symvers not found: $(METAX_MODULE_SYMVERS)"
	@echo "  Expected installed KMD tree:"
	@echo "    $(METAX_KMOD_DIR)/metax.ko.xz"
	@echo "    $(METAX_KMOD_DIR)/Module.symvers"
	@echo "  Override: make METAX_KMOD_DIR=/lib/modules/\$$(uname -r)/extra"
	@echo "  udma_metax_p2p_bridge.ko will NOT be built."
else
	@echo "MetaX KMD dir: $(METAX_KMOD_DIR)"
	@echo "  ko:      $(METAX_KMOD)"
	@echo "  symvers: $(METAX_MODULE_SYMVERS)"
	@test -f "$(METAX_KMOD)" || { \
		echo "WARNING: $(METAX_KMOD) not found; Module.symvers is present."; \
	}
	@grep -q metax_p2p_acquire_mem "$(METAX_MODULE_SYMVERS)" || { \
		echo "WARNING: metax_p2p_acquire_mem not in $(METAX_MODULE_SYMVERS)."; \
		echo "  Bridge still builds against local udma_metax_p2p.h;"; \
		echo "  insmod may fail if this Module.symvers is not from metax.ko."; \
	}
endif

EXTRA_KBUILD_SYMVERS := $(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) \
	$(if $(filter 1,$(HAVE_METAX_SYMVERS)),$(abspath $(METAX_MODULE_SYMVERS)),) \
	$(if $(wildcard $(UBASE_MODULE_SYMVERS)),$(abspath $(UBASE_MODULE_SYMVERS)),)

modules: check-gpu-deps
	mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_UB=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
		UB_URMA_STANDALONE=1 \
		NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH) \
		NV_MODULE_SYMVERS=$(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) \
		METAX_MODULE_SYMVERS=$(if $(filter 1,$(HAVE_METAX_SYMVERS)),$(abspath $(METAX_MODULE_SYMVERS)),) \
		UBASE_MODULE_SYMVERS=$(abspath $(UBASE_MODULE_SYMVERS)) \
		KBUILD_EXTRA_SYMBOLS="$(EXTRA_KBUILD_SYMVERS)" \
		modules
	@find $(CURDIR) -name '*.ko' ! -path '$(BUILD_DIR)/*' -print0 | \
		xargs -0 -r cp -t $(BUILD_DIR)/
	@echo "==> KO in $(BUILD_DIR):" && ls -1 $(BUILD_DIR)

show-vars:
	@echo KDIR=$(KDIR)
	@echo NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)
	@echo NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)
	@echo HAVE_NV_P2P_HEADER=$(HAVE_NV_P2P_HEADER)
	@echo METAX_KMOD_DIR=$(METAX_KMOD_DIR)
	@echo METAX_KMOD=$(METAX_KMOD)
	@echo METAX_MODULE_SYMVERS=$(METAX_MODULE_SYMVERS)
	@echo HAVE_METAX_KMOD=$(HAVE_METAX_KMOD)
	@echo HAVE_METAX_SYMVERS=$(HAVE_METAX_SYMVERS)
	@echo UBASE_MODULE_SYMVERS=$(UBASE_MODULE_SYMVERS)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf $(BUILD_DIR)

else

ifneq ($(NV_MODULE_SYMVERS),)
KBUILD_EXTRA_SYMBOLS += $(NV_MODULE_SYMVERS)
endif
ifneq ($(METAX_MODULE_SYMVERS),)
KBUILD_EXTRA_SYMBOLS += $(METAX_MODULE_SYMVERS)
endif
ifneq ($(UBASE_MODULE_SYMVERS),)
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
