ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build

# Closed-source NVIDIA driver source
NVIDIA_SRC ?= /usr/src/nvidia-575.57.08
NV_P2P_HEADER_PATH ?= $(NVIDIA_SRC)/nvidia

# 优先用 DKMS 当前内核生成的 Module.symvers；没有再 fallback 到 /usr/src
NV_MODULE_SYMVERS ?= $(firstword \
	$(wildcard /var/lib/dkms/nvidia/580.173.02/$(shell uname -r)*/$(shell uname -m)/module/Module.symvers) \
	$(wildcard /var/lib/dkms/nvidia/580.173.02/$(shell uname -r)*/Module.symvers) \
	$(wildcard $(NVIDIA_SRC)/Module.symvers))

.PHONY: all clean show-vars

<<<<<<< HEAD
all:
	@test -f "$(NV_P2P_HEADER_PATH)/nv-p2p.h" || { echo "missing nv-p2p.h: $(NV_P2P_HEADER_PATH)/nv-p2p.h"; exit 1; }
	@test -f "$(NV_MODULE_SYMVERS)" || { echo "missing Module.symvers"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_UB=m CONFIG_UB_UBASE=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
		NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH) \
		KBUILD_EXTRA_SYMBOLS=$(abspath $(NV_MODULE_SYMVERS)) \
=======
all: modules

show-vars:
	@echo "NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)"
	@test -f "$(NV_P2P_HEADER_PATH)/nv-p2p.h" && echo "OK: nv-p2p.h exists" || \
		echo "ERROR: nv-p2p.h not at $(NV_P2P_HEADER_PATH)/nv-p2p.h"
	@echo "NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)"

# FIX: 同 urma/Makefile —— 没有 GPU 的机器（比如跨节点测试里不带 GPU
# 的对端）现在也要能编译出跟带 GPU 那台节点版本一致的整套模块，NV 相关
# 检查从"缺了就 exit 1"改成"缺了就 WARNING + 自动禁用 GPU P2P"，不再
# 阻塞整个编译。
HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)
HAVE_NV_SYMVERS := $(if $(and $(filter 1,$(HAVE_NV_P2P_HEADER)),$(wildcard $(NV_MODULE_SYMVERS))),1,0)

modules:
ifeq ($(HAVE_NV_P2P_HEADER),0)
	@echo "WARNING: nv-p2p.h not found under $(NV_P2P_HEADER_PATH)."
	@echo "  Building WITHOUT GPU P2P support (UDMA_GPU_P2P_ENABLE=0);"
	@echo "  udma_nv_p2p_bridge.ko will NOT be built. See urma/hw/udma/Makefile."
else
	@test -f "$(NV_MODULE_SYMVERS)" || \
		echo "WARNING: Module.symvers not found: $(NV_MODULE_SYMVERS); udma_nv_p2p_bridge.ko will NOT be built."
endif
	mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
		CONFIG_UB=m CONFIG_UB_UBASE=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
		NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH) \
		NV_MODULE_SYMVERS=$(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) \
		KBUILD_EXTRA_SYMBOLS="$(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),)" \
>>>>>>> 313269447ed1dc8d142449c9c354929397e36853
		modules

show-vars:
	@echo KDIR=$(KDIR)
	@echo NVIDIA_SRC=$(NVIDIA_SRC)
	@echo NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)
	@echo NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

else

NV_P2P_HEADER_PATH ?= /usr/src/nvidia-580.173.02/nvidia
ccflags-y += -I$(NV_P2P_HEADER_PATH)

ifneq ($(KBUILD_EXTRA_SYMBOLS),)
else
NVIDIA_SRC ?= /usr/src/nvidia-580.173.02
NV_MODULE_SYMVERS ?= $(NVIDIA_SRC)/Module.symvers
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
