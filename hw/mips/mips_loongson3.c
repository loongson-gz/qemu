/*
 * Generic Loongson-3 Platform support
 *
 * Copyright (c) 2014-2020 Huacai Chen (chenhc@lemote.com)
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions are licensed under the terms of the GNU GPL,
 * version 2 or (at your option) any later version.
 */

/*
 * Generic PC Platform based on Loongson-3 CPU (MIPS64R2 with extensions,
 * 800~2000MHz)
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "cpu.h"
#include "elf.h"
#include "hw/boards.h"
#include "hw/block/flash.h"
#include "hw/char/serial.h"
#include "hw/mips/mips.h"
#include "hw/mips/cpudevs.h"
#include "hw/intc/i8259.h"
#include "hw/loader.h"
#include "hw/ide.h"
#include "hw/isa/superio.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/rtc/mc146818rtc.h"
#include "net/net.h"
#include "exec/address-spaces.h"
#include "sysemu/qtest.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "qemu/log.h"
#include "qemu/error-report.h"

#define INITRD_OFFSET		0x03ea0000
#define BOOTPARAM_ADDR		0x8ff00000
#define BOOTPARAM_PHYADDR	0x0ff00000
#define CFG_ADDR 		0x0f100000
#define FW_CONF_ADDR		0x0fff0000
#define PM_MMIO_ADDR		0x0e0010080000ULL
#define PM_MMIO_SIZE		0x100
#define PM_CNTL_MODE		0x10

#define PHYS_TO_VIRT(x) ((x) | ~(target_ulong)0x7fffffff)

/* Loongson-3 has a 2MB flash rom */
#define BIOS_SIZE               (2 * MiB)
#define LOONGSON_MAX_VCPUS      16

#define LOONGSON3_BIOSNAME "bios_loongson3.bin"

#define PCIE_IRQ_BASE	   3
#define VIRT_PCI_MEM_BASE  0x40000000ul
#define VIRT_PCI_MEM_SIZE  0x40000000ul
#define VIRT_PCI_IO_BASE   0x18000000ul
#define VIRT_PCI_IO_SIZE   0x000c0000ul

#define align(x) (((x) + 63) & ~63)

struct efi_memory_map_loongson {
    uint16_t vers;               /* version of efi_memory_map */
    uint32_t nr_map;             /* number of memory_maps */
    uint32_t mem_freq;           /* memory frequence */
    struct mem_map{
        uint32_t node_id;        /* node_id which memory attached to */
        uint32_t mem_type;       /* system memory, pci memory, pci io, etc. */
        uint64_t mem_start;      /* memory map start address */
        uint32_t mem_size;       /* each memory_map size, not the total size */
    } map[128];
} __attribute__((packed));

enum loongson_cpu_type {
    Legacy_2E = 0x0,
    Legacy_2F = 0x1,
    Legacy_3A = 0x2,
    Legacy_3B = 0x3,
    Legacy_1A = 0x4,
    Legacy_1B = 0x5,
    Legacy_2G = 0x6,
    Legacy_2H = 0x7,
    Loongson_1A = 0x100,
    Loongson_1B = 0x101,
    Loongson_2E = 0x200,
    Loongson_2F = 0x201,
    Loongson_2G = 0x202,
    Loongson_2H = 0x203,
    Loongson_3A = 0x300,
    Loongson_3B = 0x301
};

/*
 * Capability and feature descriptor structure for MIPS CPU
 */
struct efi_cpuinfo_loongson {
    uint16_t vers;               /* version of efi_cpuinfo_loongson */
    uint32_t processor_id;       /* PRID, e.g. 6305, 6306 */
    uint32_t cputype;            /* Loongson_3A/3B, etc. */
    uint32_t total_node;         /* num of total numa nodes */
    uint16_t cpu_startup_core_id;   /* Boot core id */
    uint16_t reserved_cores_mask;
    uint32_t cpu_clock_freq;     /* cpu_clock */
    uint32_t nr_cpus;
    char cpuname[64];
} __attribute__((packed));

#define MAX_UARTS 64
struct uart_device {
    uint32_t iotype; /* see include/linux/serial_core.h */
    uint32_t uartclk;
    uint32_t int_offset;
    uint64_t uart_base;
} __attribute__((packed));

#define MAX_SENSORS 64
#define SENSOR_TEMPER  0x00000001
#define SENSOR_VOLTAGE 0x00000002
#define SENSOR_FAN     0x00000004
struct sensor_device {
    char name[32];  /* a formal name */
    char label[64]; /* a flexible description */
    uint32_t type;       /* SENSOR_* */
    uint32_t id;         /* instance id of a sensor-class */
    uint32_t fan_policy; /* see arch/mips/include/asm/mach-loongson/loongson_hwmon.h */
    uint32_t fan_percent;/* only for constant speed policy */
    uint64_t base_addr;  /* base address of device registers */
} __attribute__((packed));

struct system_loongson {
    uint16_t vers;               /* version of system_loongson */
    uint32_t ccnuma_smp;         /* 0: no numa; 1: has numa */
    uint32_t sing_double_channel;/* 1: single; 2: double */
    uint32_t nr_uarts;
    struct uart_device uarts[MAX_UARTS];
    uint32_t nr_sensors;
    struct sensor_device sensors[MAX_SENSORS];
    char has_ec;
    char ec_name[32];
    uint64_t ec_base_addr;
    char has_tcm;
    char tcm_name[32];
    uint64_t tcm_base_addr;
    uint64_t workarounds; /* see workarounds.h */
    uint64_t of_dtb_addr; /* NULL if not support */
} __attribute__((packed));

struct irq_source_routing_table {
    uint16_t vers;
    uint16_t size;
    uint16_t rtr_bus;
    uint16_t rtr_devfn;
    uint32_t vendor;
    uint32_t device;
    uint32_t PIC_type;           /* conform use HT or PCI to route to CPU-PIC */
    uint64_t ht_int_bit;         /* 3A: 1<<24; 3B: 1<<16 */
    uint64_t ht_enable;          /* irqs used in this PIC */
    uint32_t node_id;            /* node id: 0x0-0; 0x1-1; 0x10-2; 0x11-3 */
    uint64_t pci_mem_start_addr;
    uint64_t pci_mem_end_addr;
    uint64_t pci_io_start_addr;
    uint64_t pci_io_end_addr;
    uint64_t pci_config_addr;
    uint16_t dma_mask_bits;
    uint16_t dma_noncoherent;
} __attribute__((packed));

struct interface_info {
    uint16_t vers;               /* version of the specificition */
    uint16_t size;
    uint8_t  flag;
    char description[64];
} __attribute__((packed));

#define MAX_RESOURCE_NUMBER 128
struct resource_loongson {
    uint64_t start;              /* resource start address */
    uint64_t end;                /* resource end address */
    char name[64];
    uint32_t flags;
};

struct archdev_data {};          /* arch specific additions */

struct board_devices {
    char name[64];               /* hold the device name */
    uint32_t num_resources;      /* number of device_resource */
    /* for each device's resource */
    struct resource_loongson resource[MAX_RESOURCE_NUMBER];
    /* arch specific additions */
    struct archdev_data archdata;
};

struct loongson_special_attribute {
    uint16_t vers;               /* version of this special */
    char special_name[64];       /* special_atribute_name */
    uint32_t loongson_special_type; /* type of special device */
    /* for each device's resource */
    struct resource_loongson resource[MAX_RESOURCE_NUMBER];
};

struct loongson_params {
    uint64_t memory_offset;      /* efi_memory_map_loongson struct offset */
    uint64_t cpu_offset;         /* efi_cpuinfo_loongson struct offset */
    uint64_t system_offset;      /* system_loongson struct offset */
    uint64_t irq_offset;         /* irq_source_routing_table struct offset */
    uint64_t interface_offset;   /* interface_info struct offset */
    uint64_t special_offset;     /* loongson_special_attribute struct offset */
    uint64_t boarddev_table_offset;  /* board_devices offset */
};

struct smbios_tables {
    uint16_t vers;               /* version of smbios */
    uint64_t vga_bios;           /* vga_bios address */
    struct loongson_params lp;
};

struct efi_reset_system_t {
    uint64_t ResetCold;
    uint64_t ResetWarm;
    uint64_t ResetType;
    uint64_t Shutdown;
    uint64_t DoSuspend; /* NULL if not support */
};

struct efi_loongson {
    uint64_t mps;                /* MPS table */
    uint64_t acpi;               /* ACPI table (IA64 ext 0.71) */
    uint64_t acpi20;             /* ACPI table (ACPI 2.0) */
    struct smbios_tables smbios; /* SM BIOS table */
    uint64_t sal_systab;         /* SAL system table */
    uint64_t boot_info;          /* boot info table */
};

struct boot_params {
    struct efi_loongson efi;
    struct efi_reset_system_t reset_system;
};

static struct _fw_config {
    unsigned long ram_size;
    unsigned int mem_freq;
    unsigned int nr_cpus;
    unsigned int cpu_clock_freq;
} fw_config;

static struct _loaderparams {
    unsigned long ram_size;
    const char *kernel_cmdline;
    const char *kernel_filename;
    const char *initrd_filename;
    int64_t kernel_entry;
    unsigned long a0, a1, a2;
} loaderparams;

static void *boot_params_p;
static void *boot_params_buf;
static qemu_irq *i8259;

static unsigned int bios_boot_code[] = {
    0x40086000,   /* mfc0    t0, CP0_STATUS                                        */
    0x240900E2,   /* li      t1, 0x00e2       #{cu3,cu2,cu1,cu0,status_fr}<={0111} */
    0x01094025,   /* or      t0, t0, t1                                            */
    0x40886000,   /* mtc0    t0, CP0_STATUS                                        */
    0x00000000,
    0x40086000,   /* mfc0    t0, CP0_STATUS                                        */
    0x3C090040,   /* lui     t1, 0x40         #bev                                 */
    0x01094025,   /* or      t0, t0, t1                                            */
    0x40886000,   /* mtc0    t0, CP0_STATUS                                        */
    0x00000000,
    0x40806800,   /* mtc0    zero, CP0_CAUSE                                       */
    0x00000000,
    0x400A7801,   /* mfc0    t2, $15, 1                                            */
    0x314A00FF,   /* andi    t2, 0x0ff                                             */
    0x3C089000,   /* dli     t0, 0x900000003ff01000                                */
    0x00084438,
    0x35083FF0,
    0x00084438,
    0x35081000,
    0x314B0003,   /* andi    t3, t2, 0x3      #local cpuid                         */
    0x000B5A00,   /* sll     t3, 8                                                 */
    0x010B4025,   /* or      t0, t0, t3                                            */
    0x314C000C,   /* andi    t4, t2, 0xc      #node id                             */
    0x000C62BC,   /* dsll    t4, 42                                                */
    0x010C4025,   /* or      t0, t0, t4                                            */
                  /* waitforinit:                                                  */
    0xDD020020,   /* ld      v0, FN_OFF(t0)   #FN_OFF 0x020                        */
    0x1040FFFE,   /* beqz    v0, waitforinit                                       */
    0x00000000,   /* nop                                                           */
    0xDD1D0028,   /* ld      sp, SP_OFF(t0)   #FN_OFF 0x028                        */
    0xDD1C0030,   /* ld      gp, GP_OFF(t0)   #FN_OFF 0x030                        */
    0xDD050038,   /* ld      a1, A1_OFF(t0)   #FN_OFF 0x038                        */
    0x00400008,   /* jr      v0               #byebye                              */
    0x00000000,   /* nop                                                           */
    0x1000FFFF,   /* 1:  b   1b                                                    */
    0x00000000,   /* nop                                                           */

                  /* Reset                                                         */
    0x3C0C9000,   /* dli     t0, 0x90000e0010080010                                */
    0x358C0E00,
    0x000C6438,
    0x358C1008,
    0x000C6438,
    0x358C0010,
    0x240D0000,   /* li      t1, 0x00                                              */
    0xA18D0000,   /* sb      t1, (t0)                                              */
    0x1000FFFF,   /* 1:  b   1b                                                    */
    0x00000000,   /* nop                                                           */

                  /* Shutdown                                                      */
    0x3C0C9000,   /* dli     t0, 0x90000e0010080010                                */
    0x358C0E00,
    0x000C6438,
    0x358C1008,
    0x000C6438,
    0x358C0010,
    0x240D00FF,   /* li      t1, 0xff                                              */
    0xA18D0000,   /* sb      t1, (t0)                                              */
    0x1000FFFF,   /* 1:  b   1b                                                    */
    0x00000000    /* nop                                                           */
};

static uint64_t loongson3_pm_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void loongson3_pm_write(void *opaque, hwaddr addr, uint64_t val,unsigned size)
{
    if (addr != PM_CNTL_MODE)
        return;

    switch (val) {
    case 0x00:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    case 0xff:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return;
    default:
        return;
    }
}

static const MemoryRegionOps loongson3_pm_ops = {
    .read  = loongson3_pm_read,
    .write = loongson3_pm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static struct efi_memory_map_loongson *init_memory_map(void *g_map)
{
    struct efi_memory_map_loongson *emap = g_map;

    emap->nr_map = 2;
    emap->mem_freq = 300000000;

    emap->map[0].node_id = 0;
    emap->map[0].mem_type = 1;
    emap->map[0].mem_start = 0x0;
    emap->map[0].mem_size = (loaderparams.ram_size > 0x10000000
                            ? 256 : (loaderparams.ram_size >> 20)) - 16;

    emap->map[1].node_id = 0;
    emap->map[1].mem_type = 2;
    emap->map[1].mem_start = 0x90000000;
    emap->map[1].mem_size = (loaderparams.ram_size > 0x10000000
                            ? (loaderparams.ram_size >> 20) - 256 : 0);

    return emap;
}

static int get_host_cpu_freq(void)
{
    int fd = 0, freq = 0;
    char buf[1024], *buf_p;

    if ((fd = open("/proc/cpuinfo", O_RDONLY)) == -1) {
        fprintf(stderr, "Failed to open /proc/cpuinfo!\n");
        return 0;
    }

    if (read(fd, buf, 1024) < 0) {
        close(fd);
        fprintf(stderr, "Failed to read /proc/cpuinfo!\n");
        return 0;
    }
    close(fd);

    buf_p = strstr(buf, "model name");
    while (*buf_p != '@') buf_p++;

    buf_p += 2;
    memcpy(buf, buf_p, 12);
    buf_p = buf;
    while ((*buf_p >= '0') && (*buf_p <= '9')) buf_p++;
    *buf_p = '\0';

    freq = atoi(buf);

    return freq * 1000 * 1000;
}

static struct efi_cpuinfo_loongson *init_cpu_info(void *g_cpuinfo_loongson)
{
    struct efi_cpuinfo_loongson *c = g_cpuinfo_loongson;

    c->cputype  = Loongson_3A;
    c->processor_id = 0x14C000;
    c->cpu_clock_freq = get_host_cpu_freq();
    if (!c->cpu_clock_freq)
        c->cpu_clock_freq = 400000000;

    c->cpu_startup_core_id = 0;
    c->nr_cpus = current_machine->smp.cpus;
    c->total_node = (current_machine->smp.cpus + 3) / 4;

    return c;
}

static struct system_loongson *init_system_loongson(void *g_system)
{
    struct system_loongson *s = g_system;

    s->ccnuma_smp = 0;
    s->sing_double_channel = 1;
    s->nr_uarts = 1;
    s->uarts[0].iotype = 2;
    s->uarts[0].int_offset = 2;
    s->uarts[0].uartclk = 25000000;
    s->uarts[0].uart_base = 0x1fe001e0;

    return s;
}

static struct irq_source_routing_table *init_irq_source(void *g_irq_source)
{
    struct irq_source_routing_table *irq_info = g_irq_source;

    irq_info->node_id = 0;
    irq_info->PIC_type = 0;
    irq_info->dma_mask_bits = 64;
    irq_info->pci_mem_start_addr = VIRT_PCI_MEM_BASE;
    irq_info->pci_mem_end_addr   = VIRT_PCI_MEM_BASE + VIRT_PCI_MEM_SIZE - 1;
    irq_info->pci_io_start_addr  = VIRT_PCI_IO_BASE;

    return irq_info;
}

static struct interface_info *init_interface_info(void *g_interface)
{
    struct interface_info *interface = g_interface;

    interface->vers = 0x01;
    strcpy(interface->description, "UEFI_Version_v1.0");

    return interface;
}

static struct board_devices *board_devices_info(void *g_board)
{
    struct board_devices *bd = g_board;

    strcpy(bd->name, "Loongson-3A-VIRT-1w-V1.00-demo");

    return bd;
}

static struct loongson_special_attribute *init_special_info(void *g_special)
{
    struct loongson_special_attribute *special = g_special;

    strcpy(special->special_name, "2014-09-11");

    return special;
}

static void init_loongson_params(struct loongson_params *lp)
{
    void *p = boot_params_p;

    lp->memory_offset = (unsigned long long)init_memory_map(p)
                        - (unsigned long long)lp;
    p += align(sizeof(struct efi_memory_map_loongson));

    lp->cpu_offset = (unsigned long long)init_cpu_info(p)
                     - (unsigned long long)lp;
    p += align(sizeof(struct efi_cpuinfo_loongson));

    lp->system_offset = (unsigned long long)init_system_loongson(p)
                        - (unsigned long long)lp;
    p += align(sizeof(struct system_loongson));

    lp->irq_offset = (unsigned long long)init_irq_source(p)
                     - (unsigned long long)lp;
    p += align(sizeof(struct irq_source_routing_table));

    lp->interface_offset = (unsigned long long)init_interface_info(p)
                           - (unsigned long long)lp;
    p += align(sizeof(struct interface_info));

    lp->boarddev_table_offset = (unsigned long long)board_devices_info(p)
                                - (unsigned long long)lp;
    p+= align(sizeof(struct board_devices));

    lp->special_offset = (unsigned long long)init_special_info(p)
                         - (unsigned long long)lp;
    p+= align(sizeof(struct loongson_special_attribute));

    boot_params_p = p;
}

static void init_smbios(struct smbios_tables *smbios)
{
    smbios->vers = 1;
    init_loongson_params(&(smbios->lp));
}

static void init_efi(struct efi_loongson *efi)
{
    init_smbios(&(efi->smbios));
}

static void init_reset_system(struct efi_reset_system_t *reset)
{
    reset->Shutdown = 0xffffffffbfc000b0;
    reset->ResetCold = 0xffffffffbfc00088;
    reset->ResetWarm = 0xffffffffbfc00088;
}

static int init_boot_param(struct boot_params *bp)
{
    init_efi(&(bp->efi));
    init_reset_system(&(bp->reset_system));

    return 0;
}

static void fw_cfg_boot_set(void *opaque, const char *boot_device,
                            Error **errp)
{
    fw_cfg_modify_i16(opaque, FW_CFG_BOOT_DEVICE, boot_device[0]);
}

static void fw_conf_init(unsigned long ram_size)
{
    FWCfgState *fw_cfg;

    fw_cfg = fw_cfg_init_mem_wide(CFG_ADDR, CFG_ADDR + 8, 8, 0, NULL);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, (uint16_t)current_machine->smp.cpus);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, (uint16_t)current_machine->smp.max_cpus);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)ram_size);
    qemu_register_boot_set(fw_cfg_boot_set, fw_cfg);

    fw_config.ram_size = ram_size;
    fw_config.mem_freq = 300000000;
    fw_config.nr_cpus = current_machine->smp.cpus;
    fw_config.cpu_clock_freq = get_host_cpu_freq();
}

static int set_prom_bootparam(ram_addr_t initrd_offset, long initrd_size)
{
    long params_size;
    char memenv[32];
    char highmemenv[32];
    void *params_buf;
    unsigned int *parg_env;
    int ret = 0;

    /* Allocate params_buf for command line. */
    params_size = 0x100000;
    params_buf = g_malloc0(params_size);

    /*
     * Layout of params_buf looks like this:
     * argv[0], argv[1], 0, env[0], env[1], ... env[i], 0,
     * argv[0]'s data, argv[1]'s data, env[0]'data, ..., env[i]'s data, 0
     */
    parg_env = (void *)params_buf;

    ret = (3 + 1) * 4;
    *parg_env++ = (BOOTPARAM_ADDR + ret);
    ret += (1 + snprintf(params_buf + ret, 256 - ret, "g"));

    /* argv1 */
    *parg_env++ = BOOTPARAM_ADDR + ret;
    if (initrd_size > 0)
        ret += (1 + snprintf(params_buf + ret, 256 - ret,
                "rd_start=0x" TARGET_FMT_lx " rd_size=%li %s",
                PHYS_TO_VIRT((uint32_t)initrd_offset),
                initrd_size, loaderparams.kernel_cmdline));
    else
        ret += (1 + snprintf(params_buf+ret, 256 - ret, "%s",
                loaderparams.kernel_cmdline));

    /* argv2 */
    *parg_env++ = BOOTPARAM_ADDR + 4*ret;

    /* env */
    sprintf(memenv, "%ld", loaderparams.ram_size > 0x10000000
            ? 256 : (loaderparams.ram_size >> 20));
    sprintf(highmemenv, "%ld", loaderparams.ram_size > 0x10000000
            ? (loaderparams.ram_size >> 20) - 256 : 0);

    setenv("memsize", memenv, 1);
    setenv("highmemsize", highmemenv, 1);

    ret = ((ret + 32) & ~31);

    boot_params_buf = (void *)(params_buf + ret);
    boot_params_p = boot_params_buf + align(sizeof(struct boot_params));

    init_boot_param(boot_params_buf);

    rom_add_blob_fixed("params", params_buf, params_size,
                       BOOTPARAM_PHYADDR);
    loaderparams.a0 = 2;
    loaderparams.a1 = 0xffffffff80000000ULL + BOOTPARAM_PHYADDR;
    loaderparams.a2 = 0xffffffff80000000ULL + BOOTPARAM_PHYADDR + ret;

    return 0;
}

static int64_t load_kernel(CPUMIPSState *env)
{
    long kernel_size;
    ram_addr_t initrd_offset;
    int64_t kernel_entry, kernel_low, kernel_high, initrd_size;

    kernel_size = load_elf(loaderparams.kernel_filename, NULL,
                           cpu_mips_kseg0_to_phys, NULL,
                           (uint64_t *)&kernel_entry,
                           (uint64_t *)&kernel_low, (uint64_t *)&kernel_high,
                           NULL, 0, EM_MIPS, 1, 0);
    if (kernel_size < 0) {
        error_report("could not load kernel '%s': %s",
                     loaderparams.kernel_filename,
                     load_elf_strerror(kernel_size));
        exit(1);
    }

    /* load initrd */
    initrd_size = 0;
    initrd_offset = 0;
    if (loaderparams.initrd_filename) {
        initrd_size = get_image_size(loaderparams.initrd_filename);
        if (initrd_size > 0) {
            initrd_offset = (kernel_high + ~INITRD_PAGE_MASK) &
                            INITRD_PAGE_MASK;
            initrd_offset = MAX(initrd_offset, INITRD_OFFSET);

            if (initrd_offset + initrd_size > ram_size) {
                error_report("memory too small for initial ram disk '%s'",
                             loaderparams.initrd_filename);
                exit(1);
            }

            initrd_size = load_image_targphys(loaderparams.initrd_filename,
                                              initrd_offset,
                                              ram_size - initrd_offset);
        }

        if (initrd_size == (target_ulong) -1) {
            error_report("could not load initial ram disk '%s'",
                         loaderparams.initrd_filename);
            exit(1);
        }
    }

    /* Setup prom parameters. */
    set_prom_bootparam(initrd_offset, initrd_size);

    return kernel_entry;
}

static void main_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    CPUMIPSState *env = &cpu->env;

    cpu_reset(CPU(cpu));

    /* Loongson-3 reset stuff */
    if (loaderparams.kernel_filename) {
        if (cpu == MIPS_CPU(first_cpu)) {
            env->active_tc.gpr[4] = loaderparams.a0;
            env->active_tc.gpr[5] = loaderparams.a1;
            env->active_tc.gpr[6] = loaderparams.a2;
            env->active_tc.PC = loaderparams.kernel_entry;
        }
        env->CP0_Status &= ~((1 << CP0St_BEV) | (1 << CP0St_ERL));
    }
}

static void loongson3_isa_init(qemu_irq intc)
{
    ISABus *isa_bus;

    isa_bus = isa_bus_new(NULL, get_system_memory(), get_system_io(), &error_abort);

    /* Interrupt controller */
    /* The 8259 -> IP3  */
    i8259 = i8259_init(isa_bus, intc);
    isa_bus_irqs(isa_bus, i8259);
    /* init other devices */
    isa_create_simple(isa_bus, "i8042");
    mc146818_rtc_init(isa_bus, 2000, NULL);
}

static inline void loongson3_pcie_init(MachineState *machine, qemu_irq *pic)
{
    int i;
    PCIBus *pci_bus;
    MemoryRegion *pci_io = g_new(MemoryRegion, 1);
    MemoryRegion *pci_mem = g_new(MemoryRegion, 1);

    memory_region_init(pci_mem, NULL, "pci-mem", VIRT_PCI_MEM_SIZE);
    memory_region_init_alias(pci_io, NULL, "pci-io", get_system_io(), 0, VIRT_PCI_IO_SIZE);
    memory_region_add_subregion(get_system_memory(), VIRT_PCI_IO_BASE, pci_io);
    memory_region_add_subregion(get_system_memory(), VIRT_PCI_MEM_BASE, pci_mem);

    pci_bus = ls7a_init(pic);

    pci_vga_init(pci_bus);

    for(i = 0; i < nb_nics; i++) {
        NICInfo *nd = &nd_table[i];

        if (!nd->model)
            nd->model = g_strdup("virtio");

        pci_nic_init_nofail(nd, pci_bus, nd->model, NULL);
    }
}

static void mips_loongson3_init(MachineState *machine)
{
    int i;
    long bios_size;
    MIPSCPU *cpu;
    CPUMIPSState *env;
    char *filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    ram_addr_t ram_size = machine->ram_size;
    MemoryRegion *address_space_mem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *bios = g_new(MemoryRegion, 1);
    MemoryRegion *iomem = g_new(MemoryRegion, 1);

    if (ram_size < 256 * 0x100000) {
        error_report("Loongson-3 need at least 256MB memory");
        exit(1);
    }

    for (i = 0; i < machine->smp.cpus; i++) {
        /* init CPUs */
        cpu = MIPS_CPU(cpu_create(machine->cpu_type));

        /* Init internal devices */
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        qemu_register_reset(main_cpu_reset, cpu);
    }
    env = &MIPS_CPU(first_cpu)->env;

    /* Allocate RAM/BIOS, 0x00000000~0x10000000 is alias of 0x80000000~0x90000000 */
    memory_region_init_rom(bios, NULL, "loongson3.bios", BIOS_SIZE, &error_fatal);
    memory_region_init_alias(ram, NULL, "loongson3.lowram", machine->ram, 0, 256 * 0x100000);
    memory_region_init_io(iomem, NULL, &loongson3_pm_ops, NULL, "loongson3_pm", PM_MMIO_SIZE);

    memory_region_add_subregion(address_space_mem, 0x00000000LL, ram);
    memory_region_add_subregion(address_space_mem, 0x1fc00000LL, bios);
    memory_region_add_subregion(address_space_mem, 0x80000000LL, machine->ram);
    memory_region_add_subregion(address_space_mem, PM_MMIO_ADDR, iomem);

    /*
     * We do not support flash operation, just loading pmon.bin as raw BIOS.
     * Please use -L to set the BIOS path and -bios to set bios name.
     */

    if (kernel_filename) {
        loaderparams.ram_size = ram_size;
        loaderparams.kernel_filename = kernel_filename;
        loaderparams.kernel_cmdline = kernel_cmdline;
        loaderparams.initrd_filename = initrd_filename;
        loaderparams.kernel_entry = load_kernel(env);
        rom_add_blob_fixed("bios", bios_boot_code, sizeof(bios_boot_code), 0x1fc00000LL);
    } else {
        if (bios_name == NULL) {
                bios_name = LOONGSON3_BIOSNAME;
        }
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        if (filename) {
            bios_size = load_image_targphys(filename, 0x1fc00000LL,
                                            BIOS_SIZE);
            g_free(filename);
        } else {
            bios_size = -1;
        }

        if ((bios_size < 0 || bios_size > BIOS_SIZE) &&
            !kernel_filename && !qtest_enabled()) {
            error_report("Could not load MIPS bios '%s'", bios_name);
            exit(1);
        }

        fw_conf_init(ram_size);
        rom_add_blob_fixed("fw_conf", (void*)&fw_config, sizeof(fw_config), FW_CONF_ADDR);
    }

    loongson3_isa_init(env->irq[3]);
    loongson3_pcie_init(machine, i8259);

    if (serial_hd(0))
        serial_mm_init(address_space_mem, 0x1fe001e0, 0, env->irq[2], 115200, serial_hd(0), DEVICE_NATIVE_ENDIAN);
}

static void mips_loongson3_machine_init(MachineClass *mc)
{
    mc->desc = "Generic Loongson-3 Platform";
    mc->init = mips_loongson3_init;
    mc->block_default_type = IF_IDE;
    mc->max_cpus = LOONGSON_MAX_VCPUS;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("Loongson-3A");
    mc->default_ram_id = "loongson3.highram";
    mc->default_ram_size = 560 * MiB;
    mc->kvm_type = mips_kvm_type;
    mc->minimum_page_bits = 14;
}

DEFINE_MACHINE("loongson3", mips_loongson3_machine_init)
