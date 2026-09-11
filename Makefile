ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build

# NVIDIA (optional). Default matches the lab /usr/src tree.
NVIDIA_SRC ?= /usr/src/nvidia-575.57.08
NV_P2P_HEADER_PATH ?= $(NVIDIA_SRC)/nvidia
NV_MODULE_SYMVERS ?= $(firstword \
        $(wildcard /var/lib/dkms/nvidia/575.57.08/$(shell uname -r)*/$(shell uname -m)/module/Module.symvers) \
        $(wildcard /var/lib/dkms/nvidia/575.57.08/$(shell uname -r)*/Module.symvers) \
        $(wildcard $(NVIDIA_SRC)/Module.symvers))

# MetaX: installed KMD in extra/ (metax.ko.xz + Module.symvers)
METAX_KMOD_DIR ?= /lib/modules/$(shell uname -r)/extra
METAX_KMOD ?= $(METAX_KMOD_DIR)/metax.ko.xz
METAX_MODULE_SYMVERS ?= $(METAX_KMOD_DIR)/Module.symvers

HAVE_NV_P2P_HEADER := $(if $(wildcard $(NV_P2P_HEADER_PATH)/nv-p2p.h),1,0)
HAVE_NV_SYMVERS := $(if $(and $(filter 1,$(HAVE_NV_P2P_HEADER)),$(wildcard $(NV_MODULE_SYMVERS))),1,0)
HAVE_METAX_SYMVERS := $(if $(wildcard $(METAX_MODULE_SYMVERS)),1,0)

EXTRA_KBUILD_SYMVERS := $(if $(filter 1,$(HAVE_NV_SYMVERS)),$(abspath $(NV_MODULE_SYMVERS)),) \
	$(if $(filter 1,$(HAVE_METAX_SYMVERS)),$(abspath $(METAX_MODULE_SYMVERS)),)

.PHONY: all clean show-vars

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) \
                CONFIG_UB=m CONFIG_UB_UBASE=m CONFIG_UB_URMA=m CONFIG_UB_UDMA=m \
                NV_P2P_HEADER_PATH=$(if $(filter 1,$(HAVE_NV_P2P_HEADER)),$(NV_P2P_HEADER_PATH),) \
                METAX_MODULE_SYMVERS=$(if $(filter 1,$(HAVE_METAX_SYMVERS)),$(abspath $(METAX_MODULE_SYMVERS)),) \
                KBUILD_EXTRA_SYMBOLS="$(EXTRA_KBUILD_SYMVERS)" \
                modules

show-vars:
	@echo KDIR=$(KDIR)
	@echo NVIDIA_SRC=$(NVIDIA_SRC)
	@echo NV_P2P_HEADER_PATH=$(NV_P2P_HEADER_PATH)
	@echo NV_MODULE_SYMVERS=$(NV_MODULE_SYMVERS)
	@echo HAVE_NV_P2P_HEADER=$(HAVE_NV_P2P_HEADER)
	@echo METAX_KMOD_DIR=$(METAX_KMOD_DIR)
	@echo METAX_KMOD=$(METAX_KMOD)
	@echo METAX_MODULE_SYMVERS=$(METAX_MODULE_SYMVERS)
	@echo HAVE_METAX_SYMVERS=$(HAVE_METAX_SYMVERS)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

else

ifneq ($(NV_P2P_HEADER_PATH),)
ccflags-y += -I$(NV_P2P_HEADER_PATH)
endif

ifneq ($(KBUILD_EXTRA_SYMBOLS),)
else
ifneq ($(NV_MODULE_SYMVERS),)
KBUILD_EXTRA_SYMBOLS += $(NV_MODULE_SYMVERS)
endif
endif
ifneq ($(METAX_MODULE_SYMVERS),)
KBUILD_EXTRA_SYMBOLS += $(METAX_MODULE_SYMVERS)
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
