/*
 * Loongson LS7A north bridge support
 *
 * Copyright (c) 2019 Loongson Technology
 * Copyright (c) 2020 Lemote Inc.
 *
 * Authors:
 *  Chen Zhu <zhuchen@loongson.cn>
 *  Huacai Chen <chenhc@lemote.com>
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"

#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/mips/mips.h"
#include "hw/pci/pci_host.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "qemu/log.h"
#include "hw/loader.h"
#include "elf.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_bus.h"
#include "linux/kvm.h"
#include "sysemu/kvm.h"
#include "sysemu/reset.h"
#include "migration/vmstate.h"

#define LS7A_REG_BASE         0x1FE00000
#define LS7A_PCICONFIGBASE    0x00
#define LS7A_IREGBASE         0x100

#define LS7A_PCICONFIG_BASE   (LS7A_REG_BASE + LS7A_PCICONFIGBASE)
#define LS7A_PCICONFIG_SIZE   (0x100)

#define LS7A_INTERNAL_REG_BASE  (LS7A_REG_BASE + LS7A_IREGBASE)
#define LS7A_INTERNAL_REG_SIZE  (0xE0)

#define NR_REGS             (0xE0 >> 2)

#ifdef DEBUG_LS7A
#define DPRINTF(fmt, ...) fprintf(stderr, "%s: " fmt, __FUNCTION__, ##__VA_ARGS__)
#else
#define DPRINTF(fmt, ...)
#endif

typedef struct Ls7aState Ls7aState;

typedef struct PCILs7aState
{
    PCIDevice dev;

    Ls7aState *pcihost;
    uint32_t regs[NR_REGS];

    /* LS7A registers */
    MemoryRegion iomem;

} PCILs7aState;

struct Ls7aState {
    PCIHostState parent_obj;
    qemu_irq *pic;
    PCILs7aState *pci_dev;
};

#define TYPE_LS7A_PCI_HOST_BRIDGE "ls7a-pcihost"
#define LS7A_PCI_HOST_BRIDGE(obj) \
    OBJECT_CHECK(Ls7aState, (obj), TYPE_LS7A_PCI_HOST_BRIDGE)

#define TYPE_PCI_LS7A "ls7a"
#define PCI_LS7A(obj) \
    OBJECT_CHECK(PCILs7aState, (obj), TYPE_PCI_LS7A)

static uint64_t ls7a_pciconf_readl(void *opaque, hwaddr addr,
                                     unsigned size)
{
    uint64_t val;
    PCILs7aState *s = opaque;
    PCIDevice *d = PCI_DEVICE(s);

    val = d->config_read(d, addr, 4);
    DPRINTF(TARGET_FMT_plx" val %x\n", addr,val);
    return val;
}

static void ls7a_pciconf_writel(void *opaque, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    PCILs7aState *s = opaque;
    PCIDevice *d = PCI_DEVICE(s);

    DPRINTF(TARGET_FMT_plx" val %lx\n", addr, val);
    d->config_write(d, addr, val, 4);
}

/* north bridge PCI configure space. 0x1fe0 0000 - 0x1fe0 00ff */

static const MemoryRegionOps ls7a_pciconf_ops = {
    .read = ls7a_pciconf_readl,
    .write = ls7a_pciconf_writel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};


static void ls7a_reset(void *opaque)
{
    uint64_t wmask;
    wmask = ~(-1);

    PCIDevice *dev = opaque;
    pci_set_word(dev->config + 0x0, 0x0014);
    pci_set_word(dev->wmask + 0x0, wmask & 0xffff);
    pci_set_word(dev->cmask + 0x0, 0xffff);
    pci_set_word(dev->config + 0x2, 0x7a00);
    pci_set_word(dev->wmask + 0x2, wmask & 0xffff);
    pci_set_word(dev->cmask + 0x2, 0xffff);
    pci_set_word(dev->config + 0x4, 0x0000);
    pci_set_word(dev->config + 0x6, 0x0010);
    pci_set_word(dev->wmask + 0x6, wmask & 0xffff);
    pci_set_word(dev->cmask + 0x6, 0xffff);
    pci_set_byte(dev->config + 0x8, 0x00);
    pci_set_byte(dev->wmask + 0x8, wmask & 0xff);
    pci_set_byte(dev->cmask + 0x8, 0xff);
    pci_set_byte(dev->config + 0x9, 0x00);
    pci_set_byte(dev->wmask + 0x9, wmask & 0xff);
    pci_set_byte(dev->cmask + 0x9, 0xff);
    pci_set_byte(dev->config + 0xa, 0x00);
    pci_set_byte(dev->wmask + 0xa, wmask & 0xff);
    pci_set_byte(dev->cmask + 0xa, 0xff);
    pci_set_byte(dev->config + 0xb, 0x06);
    pci_set_byte(dev->wmask + 0xb, wmask & 0xff);
    pci_set_byte(dev->cmask + 0xb, 0xff);
    pci_set_byte(dev->config + 0xc, 0x00);
    pci_set_byte(dev->wmask + 0xc, wmask & 0xff);
    pci_set_byte(dev->cmask + 0xc, 0xff);
    pci_set_byte(dev->config + 0xe, 0x80);
    pci_set_byte(dev->wmask + 0xe, wmask & 0xff);
    pci_set_byte(dev->cmask + 0xe, 0xff);
    pci_set_long(dev->config + PCI_BASE_ADDRESS_0, 0x0);
    pci_set_long(dev->wmask + PCI_BASE_ADDRESS_0, wmask & 0xffffffff);
    pci_set_long(dev->cmask + PCI_BASE_ADDRESS_0, 0xffffffff);
    pci_set_long(dev->config + PCI_BASE_ADDRESS_1, 0x0);
    pci_set_long(dev->wmask + PCI_BASE_ADDRESS_1, wmask & 0xffffffff);
    pci_set_long(dev->cmask + PCI_BASE_ADDRESS_1, 0xffffffff);
    pci_set_long(dev->config + PCI_BASE_ADDRESS_2, 0x0);
    pci_set_long(dev->wmask + PCI_BASE_ADDRESS_2, wmask & 0xffffffff);
    pci_set_long(dev->cmask + PCI_BASE_ADDRESS_2, 0xffffffff);
    pci_set_long(dev->config + PCI_BASE_ADDRESS_3, 0x00000004);
    pci_set_long(dev->wmask + PCI_BASE_ADDRESS_3, wmask & 0xffffffff);
    pci_set_long(dev->cmask + PCI_BASE_ADDRESS_3, 0xffffffff);
    pci_set_long(dev->config + PCI_BASE_ADDRESS_4, 0x0);
    pci_set_long(dev->wmask + PCI_BASE_ADDRESS_4, wmask & 0xffffffff);
    pci_set_long(dev->cmask + PCI_BASE_ADDRESS_4, 0xffffffff);
    pci_set_long(dev->config + PCI_BASE_ADDRESS_5, 0x0);
    pci_set_long(dev->wmask + PCI_BASE_ADDRESS_5, wmask & 0xffffffff);
    pci_set_long(dev->cmask + PCI_BASE_ADDRESS_5, 0xffffffff);
    pci_set_word(dev->config + PCI_CARDBUS_CIS, 0x0000);
    pci_set_word(dev->config + 0x2c, 0x0014);
    pci_set_word(dev->wmask + 0x2c, wmask & 0xffff);
    pci_set_word(dev->cmask + 0x2c, 0xffff);
    pci_set_word(dev->config + 0x2e, 0x7a00);
    pci_set_word(dev->wmask + 0x2e, wmask & 0xffff);
    pci_set_word(dev->cmask + 0x2e, 0xffff);
    pci_set_byte(dev->config + 0x34, 0x40);
    pci_set_byte(dev->wmask + 0x34, wmask & 0xff);
    pci_set_byte(dev->cmask + 0x34, 0xff);
    pci_set_byte(dev->config + 0x3c, 0x00);
    pci_set_byte(dev->config + 0x3d, 0x00);
    pci_set_byte(dev->wmask + 0x3d, wmask & 0xff);
    pci_set_word(dev->config + 0x3e, 0x0000);
    pci_set_byte(dev->config + 0x4c, 0x60);
    pci_set_byte(dev->wmask + 0x4c, wmask & 0xff);
}

static uint64_t ls7a_readl(void *opaque, hwaddr addr,
                             unsigned size)
{
    PCILs7aState *s = opaque;
    uint32_t saddr;

    saddr = addr >> 2;
    DPRINTF(TARGET_FMT_plx"\n", addr);

    return s->regs[saddr];
}

static void ls7a_writel(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
{
}

static const MemoryRegionOps ls7a_ops = {
    .read = ls7a_readl,
    .write = ls7a_writel,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static const VMStateDescription vmstate_ls7a = {
    .name = "ls7a",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PCILs7aState),
        VMSTATE_END_OF_LIST()
    }
};

static void pci_ls7a_set_irq(void *opaque, int irq_num, int level)
{
    Ls7aState *s = opaque;
    qemu_irq *pic = s->pic;
    qemu_set_irq(pic[irq_num], level);
}

static int pci_ls7a_map_irq(PCIDevice *d, int irq_num)
{
    static int virq[] = {3, 4, 5, 6, 7, 9, 10, 11};

    int slot = (d->devfn >> 3);
    int pin = *(d->config + PCI_INTERRUPT_PIN);

    return virq[(pin + slot) % 8];
}

static void ls7a_realize(PCIDevice *dev, Error **errp)
{
    PCILs7aState *s = PCI_LS7A(dev);
    SysBusDevice *sysbus = SYS_BUS_DEVICE(s->pcihost);
    PCIHostState *phb = PCI_HOST_BRIDGE(s->pcihost);

    /* Ls7a North Bridge */
    pci_config_set_prog_interface(dev->config, 0x00);

    /* set the north bridge register mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &ls7a_ops, s,
                          "north-bridge-register", LS7A_INTERNAL_REG_SIZE);
    sysbus_init_mmio(sysbus, &s->iomem);

    sysbus_mmio_map(sysbus, 0, LS7A_INTERNAL_REG_BASE);

    /* set the north bridge pci configure mapping */
    memory_region_init_io(&phb->conf_mem, OBJECT(s), &ls7a_pciconf_ops, s,
                          "north-bridge-pci-config", LS7A_PCICONFIG_SIZE);
    sysbus_init_mmio(sysbus, &phb->conf_mem);

    sysbus_mmio_map(sysbus, 1, LS7A_PCICONFIG_BASE);

    /* set the default value of north bridge pci config */
    qemu_register_reset(ls7a_reset, s);
}

static uint64_t ls7a_pci_config_read(void *opaque,
                                           hwaddr addr, unsigned size)
{
    uint64_t val;
    hwaddr tmp_addr;

    if(addr & 0x1000000)
        tmp_addr = addr & 0xffff;
    else
        tmp_addr = addr & 0xffffff;

    val = pci_data_read(opaque, tmp_addr, size);

    if (addr & 0x3c) {
        DPRINTF(TARGET_FMT_plx" val %x \n", addr, val);
    }

    return val;
}

static void ls7a_pci_config_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    hwaddr tmp_addr;

    if(addr & 0x1000000)
        tmp_addr = addr & 0xffff;
    else
        tmp_addr = addr & 0xffffff;

    pci_data_write(opaque, tmp_addr, val, size);
}

static const MemoryRegionOps ls7a_pci_config_ops = {
    .read = ls7a_pci_config_read,
    .write = ls7a_pci_config_write,
    /* Set to access 64bits data, because default to 32bits */
    .valid = {
	.min_access_size = 1,
	.max_access_size = 4,
    },
    /* Set to access 64bits data, because default to 32bits */
    .impl = {
	.min_access_size = 1,
	.max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

#define HT1LO_PCICFG_BASE 0x1a000000ul
#define HT1LO_PCICFG_SIZE 0x02000000ul

static void ls7a_pcihost_realize(DeviceState *dev, Error **errp)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);
    MemoryRegion *iomem = g_new(MemoryRegion, 1);

    phb->bus = pci_register_root_bus(DEVICE(dev), "pci",
		                     pci_ls7a_set_irq, pci_ls7a_map_irq,
                                     dev, get_system_memory(), get_system_io(),
                                     1 << 3, 128, TYPE_PCI_BUS);

    memory_region_init_io(iomem, NULL, &ls7a_pci_config_ops, phb->bus,
                          "ls7a_pci_conf", HT1LO_PCICFG_SIZE);

    memory_region_add_subregion(get_system_memory(), HT1LO_PCICFG_BASE, iomem);
}

PCIBus *ls7a_init(qemu_irq *pic)
{
    DeviceState *dev;
    PCIHostState *phb;
    Ls7aState *pcihost;
    PCILs7aState *pbs;
    PCIDevice *pdev;

    dev = qdev_create(NULL, TYPE_LS7A_PCI_HOST_BRIDGE);
    phb = PCI_HOST_BRIDGE(dev);
    pcihost = LS7A_PCI_HOST_BRIDGE(dev);
    pcihost->pic = pic;
    qdev_init_nofail(dev);

    pdev = pci_create(phb->bus, PCI_DEVFN(0, 0), TYPE_PCI_LS7A);
    pbs = PCI_LS7A(pdev);
    pbs->pcihost = pcihost;
    pcihost->pci_dev = pbs;
    qdev_init_nofail(DEVICE(pdev));

    return phb->bus;
}

static void ls7a_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ls7a_realize;
    k->vendor_id = 0x0014;
    k->device_id = 0x7a00;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    dc->desc = "Host bridge";
    dc->vmsd = &vmstate_ls7a;
    /*
     * PCI-facing part of the host bridge, not usable without the
     * host-facing part, which can't be device_add'ed, yet.
     */
    dc->user_creatable = false;
}

static const TypeInfo ls7a_info = {
    .name          = TYPE_PCI_LS7A,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCILs7aState),
    .class_init    = ls7a_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ls7a_pcihost_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ls7a_pcihost_realize;
}

static const TypeInfo ls7a_pcihost_info = {
    .name          = TYPE_LS7A_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(Ls7aState),
    .class_init    = ls7a_pcihost_class_init,
};

static void ls7a_register_types(void)
{
    type_register_static(&ls7a_pcihost_info);
    type_register_static(&ls7a_info);
}

type_init(ls7a_register_types)
