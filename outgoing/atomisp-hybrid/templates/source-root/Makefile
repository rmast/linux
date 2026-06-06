# SPDX-License-Identifier: GPL-2.0

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD)/drivers/staging/media/atomisp CONFIG_VIDEO_ATOMISP=m modules
	$(MAKE) -C $(KDIR) M=$(PWD)/external/ipu-bridge modules
	$(MAKE) -C $(KDIR) M=$(PWD)/external/mt9m114 modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/drivers/staging/media/atomisp clean
	$(MAKE) -C $(KDIR) M=$(PWD)/external/ipu-bridge clean
	$(MAKE) -C $(KDIR) M=$(PWD)/external/mt9m114 clean
