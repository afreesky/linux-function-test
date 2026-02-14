// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual PCIe over Ethernet - Shared Memory Management
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/errno.h>
#include "virtual_pcie.h"

int vpci_bar_map(struct vpci_device *dev, int bar_num,
                resource_size_t addr, resource_size_t size)
{
    if (bar_num < 0 || bar_num >= VPCI_MAX_BARS) {
        vpci_err("Invalid BAR number: %d\n", bar_num);
        return -EINVAL;
    }

    vpci_info("Mapping BAR%d: addr=0x%llx size=0x%llx\n",
              bar_num, (unsigned long long)addr, (unsigned long long)size);

    if (dev->bars[bar_num].addr) {
        vpci_warn("BAR%d already mapped, unmapping first\n", bar_num);
        vpci_bar_unmap(dev, bar_num);
    }

    dev->bars[bar_num].phys_addr = addr;
    dev->bars[bar_num].size = size;

    if (size > 0) {
        dev->bars[bar_num].addr = ioremap(addr, size);
        if (!dev->bars[bar_num].addr) {
            vpci_err("Failed to ioremap BAR%d\n", bar_num);
            return -ENOMEM;
        }
    }

    dev->bar_count++;

    vpci_info("BAR%d mapped successfully\n", bar_num);
    return 0;
}

void vpci_bar_unmap(struct vpci_device *dev, int bar_num)
{
    if (bar_num < 0 || bar_num >= VPCI_MAX_BARS) {
        return;
    }

    if (dev->bars[bar_num].addr) {
        vpci_info("Unmapping BAR%d\n", bar_num);
        iounmap(dev->bars[bar_num].addr);
        dev->bars[bar_num].addr = NULL;
        dev->bars[bar_num].phys_addr = 0;
        dev->bars[bar_num].size = 0;
        dev->bar_count--;
    }
}

int vpci_config_read(struct vpci_device *dev, u64 addr, void *data, u32 len)
{
    if (addr + len > dev->config_size) {
        vpci_warn("Config read out of bounds: addr=0x%llx len=%u\n",
                  (unsigned long long)addr, len);
        return -ERANGE;
    }

    spin_lock(&dev->lock);
    memcpy(data, dev->config_space + addr, len);
    spin_unlock(&dev->lock);

    vpci_debug("Config read: addr=0x%llx len=%u\n",
               (unsigned long long)addr, len);

    return 0;
}

int vpci_config_write(struct vpci_device *dev, u64 addr, void *data, u32 len)
{
    if (addr + len > dev->config_size) {
        vpci_warn("Config write out of bounds: addr=0x%llx len=%u\n",
                  (unsigned long long)addr, len);
        return -ERANGE;
    }

    spin_lock(&dev->lock);
    memcpy(dev->config_space + addr, data, len);
    spin_unlock(&dev->lock);

    vpci_debug("Config write: addr=0x%llx len=%u\n",
               (unsigned long long)addr, len);

    return 0;
}

int vpci_mem_read(struct vpci_device *dev, u64 addr, void *data, u32 len)
{
    int i;

    for (i = 0; i < VPCI_MAX_BARS; i++) {
        if (dev->bars[i].addr &&
            addr >= dev->bars[i].phys_addr &&
            addr + len <= dev->bars[i].phys_addr + dev->bars[i].size) {

            u64 offset = addr - dev->bars[i].phys_addr;

            if (dev->bars[i].flags & PCI_BASE_ADDRESS_SPACE_IO) {
                u8 __iomem *ioaddr = (u8 __iomem *)dev->bars[i].addr;
                memcpy_fromio(data, ioaddr + offset, len);
            } else {
                void __iomem *memaddr = dev->bars[i].addr;
                memcpy_fromio(data, memaddr + offset, len);
            }

            vpci_debug("Mem read BAR%d: offset=0x%llx len=%u\n",
                       i, (unsigned long long)offset, len);
            return 0;
        }
    }

    vpci_warn("Mem read failed: addr=0x%llx len=%u\n",
              (unsigned long long)addr, len);

    return -ENXIO;
}

int vpci_mem_write(struct vpci_device *dev, u64 addr, void *data, u32 len)
{
    int i;

    for (i = 0; i < VPCI_MAX_BARS; i++) {
        if (dev->bars[i].addr &&
            addr >= dev->bars[i].phys_addr &&
            addr + len <= dev->bars[i].phys_addr + dev->bars[i].size) {

            u64 offset = addr - dev->bars[i].phys_addr;

            if (dev->bars[i].flags & PCI_BASE_ADDRESS_SPACE_IO) {
                u8 __iomem *ioaddr = (u8 __iomem *)dev->bars[i].addr;
                memcpy_toio(ioaddr + offset, data, len);
            } else {
                void __iomem *memaddr = dev->bars[i].addr;
                memcpy_toio(memaddr + offset, data, len);
            }

            vpci_debug("Mem write BAR%d: offset=0x%llx len=%u\n",
                       i, (unsigned long long)offset, len);
            return 0;
        }
    }

    vpci_warn("Mem write failed: addr=0x%llx len=%u\n",
              (unsigned long long)addr, len);

    return -ENXIO;
}
