ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build

# Closed-source NVIDIA driver source
NVIDIA_SRC ?= /usr/src/nvidia-575.57.08
NV_P2P_HEADER_PATH ?= $(NVIDIA_SRC)/nvidia

# 优先用 DKMS 当前内核生成的 Module.symvers；没有再 fallback 到 /usr/src
NV_MODULE_SYMVERS ?= $(firstword \
        $(wildcard /var/lib/dkms/nvidia/575.57.08/$(shell uname -r)*/$(shell uname -m)/module/Module.symvers) \
        $(wildcard /var/lib/dkms/nvidia/575.57.08/$(shell uname -r)*/Module.symvers) \
        $(wildcard $(NVIDIA_SRC)/Module.symvers))

.PHONY: all clean show-vars

all:
	@test -f "$(NV_P2P_HEADER_PATH)/nv-p2p.h" || { echo "missing nv-p2p.h: $(NV_P2P_HEADER_PATH)/nv-p2p.h"; exit 1; }
	@test -f "$(NV_MODULE_SYMVERS)" || { echo "missing Module.symvers"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
                CONFIG_UB=m CONFIG_UB_UBASE=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
                NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH) \
                KBUILD_EXTRA_SYMBOLS=$(abspath $(NV_MODULE_SYMVERS)) \
                modules

show-vars:
	@echo KDIR=$(KDIR)
	@echo NVIDIA_SRC=$(NVIDIA_SRC)
	@echo NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)
	@echo NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

else

NV_P2P_HEADER_PATH ?= /usr/src/nvidia-575.57.08/nvidia
ccflags-y += -I$(NV_P2P_HEADER_PATH)

ifneq ($(KBUILD_EXTRA_SYMBOLS),)
else
NVIDIA_SRC ?= /usr/src/nvidia-575.57.08
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
