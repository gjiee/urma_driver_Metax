# SPDX-License-Identifier: GPL-2.0+
#
# In-kernel: drivers/ub/Makefile 挂 urma/ 与 urma/hw/udma/
# OOT 仅 urma:  cd urma && make  -> build/*.ko

ifeq ($(KERNELRELEASE),)

BUILD_DIR ?= $(CURDIR)/build
KDIR      ?= /lib/modules/$(shell uname -r)/build
REPO_ROOT ?= $(abspath $(CURDIR)/..)

NV_P2P_HEADER_PATH ?= $(abspath $(REPO_ROOT)/../nv_module_netlab/kernel-open/nvidia)
NV_MODULE_SYMVERS  ?= $(NV_P2P_HEADER_PATH)/Module.symvers
METAX_MODULE_SYMVERS ?=
UBASE_MODULE_SYMVERS ?= $(REPO_ROOT)/ubase/Module.symvers

export NV_P2P_HEADER_PATH
export NV_MODULE_SYMVERS
export METAX_MODULE_SYMVERS
export UBASE_MODULE_SYMVERS

.PHONY: all modules clean check-nv-symvers
all: modules

# NVIDIA headers are optional. udma.ko always has GPU pin (needs a
# loaded vendor bridge at runtime). Missing nv-p2p.h only skips
# udma_nv_p2p_bridge.ko; udma_metax_p2p_bridge.ko still builds.
HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)
HAVE_NV_SYMVERS := $(if $(and $(filter 1,$(HAVE_NV_P2P_HEADER)),$(wildcard $(NV_MODULE_SYMVERS))),1,0)

check-nv-symvers:
ifeq ($(HAVE_NV_P2P_HEADER),0)
	@echo "WARNING: nv-p2p.h not found under $(NV_P2P_HEADER_PATH)."
	@echo "  udma.ko still builds with GPU pin (vendor-neutral)."
	@echo "  udma_nv_p2p_bridge.ko will NOT be built."
	@echo "  udma_metax_p2p_bridge.ko will still be built."
else
	@test -f "$(NV_MODULE_SYMVERS)" || { \
		echo "WARNING: $(NV_MODULE_SYMVERS) not found."; \
		echo "  udma_nv_p2p_bridge.ko (Dual BSD/GPL bridge that directly"; \
		echo "  links nvidia.ko) will NOT be built; udma.ko itself does"; \
		echo "  not need it and will still build fine."; \
	}
	@test ! -f "$(NV_MODULE_SYMVERS)" || grep -q nvidia_p2p_get_pages "$(NV_MODULE_SYMVERS)" || { \
		echo "WARNING: nvidia_p2p_get_pages not found in $(NV_MODULE_SYMVERS), same as above."; }
endif

EXTRA_KBUILD_SYMVERS := $(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) \
	$(if $(wildcard $(METAX_MODULE_SYMVERS)),$(abspath $(METAX_MODULE_SYMVERS)),) \
	$(if $(wildcard $(UBASE_MODULE_SYMVERS)),$(abspath $(UBASE_MODULE_SYMVERS)),)

modules: check-nv-symvers
	mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_UB=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
		UB_URMA_STANDALONE=1 \
		NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH) \
		NV_MODULE_SYMVERS=$(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) \
		UBASE_MODULE_SYMVERS=$(abspath $(UBASE_MODULE_SYMVERS)) \
		KBUILD_EXTRA_SYMBOLS="$(EXTRA_KBUILD_SYMVERS)" \
		modules
	@find $(CURDIR) -name '*.ko' ! -path '$(BUILD_DIR)/*' -print0 | \
		xargs -0 -r cp -t $(BUILD_DIR)/
	@echo "==> KO in $(BUILD_DIR):" && ls -1 $(BUILD_DIR)

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
