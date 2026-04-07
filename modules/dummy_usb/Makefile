# SPDX-License-Identifier: GPL-2.0
#
# Makefile for USB over Ethernet kernel modules
#

obj-m := usb_over_net.o

obj-m += usb_over_ethernet_hcd.o
usb_over_ethernet_hcd-objs := hcd.o usb_over_net.o

obj-m += usb_over_ethernet_udc.o
usb_over_ethernet_udc-objs := udc.o usb_over_net.o

ccflags-y := -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

help:
	@echo "USB over Ethernet Kernel Modules"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build all modules"
	@echo "  clean   - Clean build artifacts"
	@echo "  install - Install modules to system"
	@echo ""
	@echo "Modules:"
	@echo "  usb_over_ethernet_hcd - Host Controller Driver (runs on host PC)"
	@echo "  usb_over_ethernet_udc - USB Device Controller (runs on device PC)"
	@echo ""
	@echo "Usage:"
	@echo "  Host PC:  insmod usb_over_ethernet_hcd.ko remote_ip=<device_ip>"
	@echo "  Device PC: insmod usb_over_ethernet_udc.ko listen_port=5555"