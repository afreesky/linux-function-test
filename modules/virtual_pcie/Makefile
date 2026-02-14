# SPDX-License-Identifier: GPL-2.0
#
# Virtual PCIe over Ethernet driver Makefile
#

# If Kbuild is not used (i.e., we're building outside kernel tree),
# define the necessary variables
obj-m := virtual_pcie.o

# Source files
virtual_pcie-objs := virtual_pcie_core.o virtual_pcie_net.o virtual_pcie_pkt.o virtual_pcie_shmem.o

# Kernel source directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Current directory
PWD := $(shell pwd)

# Default target
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean target
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install target
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# Help
help:
	$(MAKE) -C $(KDIR) M=$(PWD) help
