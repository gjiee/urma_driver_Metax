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
UBASE_MODULE_SYMVERS ?= $(REPO_ROOT)/ubase/Module.symvers

export NV_P2P_HEADER_PATH
export NV_MODULE_SYMVERS
export UBASE_MODULE_SYMVERS

.PHONY: all modules clean check-nv-symvers
all: modules

# FIX: NV 相关检查从"硬性要求、缺了就 exit 1 整个编译失败"改成
# "探测到就校验、探测不到就打个 WARNING 自动跳过 GPU P2P 那部分"。
# 没有 GPU/没装 NVIDIA 驱动源码的机器（比如跨节点测试里作为纯 client
# 的对端）现在也能编译出功能完整（除了 GPU pin 走 -ENOTSUPP stub）的
# udma.ko/ubcore.ko 等一整套模块——这对保证两端内核模块版本一致、避免
# 跨节点协议不兼容非常重要，参见 udma_gpu_p2p.h/.c 的 UDMA_GPU_P2P_ENABLE。
HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)
HAVE_NV_SYMVERS := $(if $(and $(filter 1,$(HAVE_NV_P2P_HEADER)),$(wildcard $(NV_MODULE_SYMVERS))),1,0)

check-nv-symvers:
ifeq ($(HAVE_NV_P2P_HEADER),0)
	@echo "WARNING: nv-p2p.h not found under $(NV_P2P_HEADER_PATH)."
	@echo "  Building WITHOUT GPU P2P support: udma.ko will compile with"
	@echo "  UDMA_GPU_P2P_ENABLE=0 (GPU pin path returns -ENOTSUPP, all"
	@echo "  other HOST-segment functionality is unaffected), and"
	@echo "  udma_nv_p2p_bridge.ko will NOT be built."
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

EXTRA_KBUILD_SYMVERS := $(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) $(if $(wildcard $(UBASE_MODULE_SYMVERS)),$(abspath $(UBASE_MODULE_SYMVERS)),)

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
