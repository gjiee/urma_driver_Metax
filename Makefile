# SPDX-License-Identifier: GPL-2.0+

ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build
BUILD_DIR ?= $(CURDIR)/build

# 必须带 /nvidia：#include <nv-p2p.h> 在该目录下
NV_P2P_HEADER_PATH ?= $(abspath ../nv_module_netlab/kernel-open/nvidia)
NV_MODULE_SYMVERS  ?= $(NV_P2P_HEADER_PATH)/Module.symvers

export NV_P2P_HEADER_PATH
export NV_MODULE_SYMVERS

.PHONY: all modules clean show-vars

all: modules

show-vars:
	@echo "NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)"
	@test -f "$(NV_P2P_HEADER_PATH)/nv-p2p.h" && echo "OK: nv-p2p.h exists" || \
		echo "ERROR: nv-p2p.h not at $(NV_P2P_HEADER_PATH)/nv-p2p.h"
	@echo "NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)"

modules:
	@test -f "$(NV_P2P_HEADER_PATH)/nv-p2p.h" || { \
		echo "ERROR: nv-p2p.h not found under $(NV_P2P_HEADER_PATH)"; \
		echo "  Use: NV_P2P_HEADER_PATH=/path/to/nv-p2p-header-dir"; \
		exit 1; }
	@test -f "$(NV_MODULE_SYMVERS)" && \
		echo "NV_MODULE_SYMVERS found, will pass via KBUILD_EXTRA_SYMBOLS (optional: udma_gpu_p2p.c now uses symbol_get() at runtime, not static linkage)" || \
		echo "NV_MODULE_SYMVERS not found ($(NV_MODULE_SYMVERS)) — OK, not required anymore since nvidia_p2p_* are resolved via symbol_get() at runtime"
	mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_UB=m CONFIG_UB_UBASE=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
		NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH) \
		NV_MODULE_SYMVERS=$(abspath $(NV_MODULE_SYMVERS)) \
		KBUILD_EXTRA_SYMBOLS="$(wildcard $(NV_MODULE_SYMVERS))" \
		modules
	@find $(CURDIR) -name '*.ko' ! -path '$(BUILD_DIR)/*' -print0 | \
		xargs -0 -r cp -t $(BUILD_DIR)/ 2>/dev/null || true
	@ls -1 $(BUILD_DIR) 2>/dev/null || true

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf $(BUILD_DIR)

else

# ===== kbuild 读这一段（KERNELRELEASE 已设置）=====
# 不要把 obj-y / ccflags-y 写在上面的 ifeq 分支里

NV_P2P_HEADER_PATH ?=

ifneq ($(KBUILD_EXTRA_SYMBOLS),)
else
NV_MODULE_SYMVERS ?= $(abspath $(NV_P2P_HEADER_PATH)/Module.symvers)
KBUILD_EXTRA_SYMBOLS := $(NV_MODULE_SYMVERS)
endif

obj-y += ubus/
obj-y += ubfi/
obj-y += obmm/
obj-$(CONFIG_UB_UBASE) += ubase/
obj-$(CONFIG_UB_CDMA) += cdma/
obj-$(CONFIG_UB_URMA) += urma/
obj-$(CONFIG_UB_UDMA) += urma/hw/udma/
obj-$(CONFIG_UB_SENTRY) += sentry/
obj-$(CONFIG_UB_UBDEVSHM) += ubdevshm/
obj-$(CONFIG_UB_UBMEMPFD) += ubmempfd/

endif
