#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <drivers/block.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <hal/cpu.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define SATA_SIG_ATA    0x00000101 // SATA drive
#define SATA_SIG_ATAPI  0xEB140101 // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101 // Enclosure management bridge
#define SATA_SIG_PM     0x96690101 // Port multiplier

#define AHCI_DEV_BUSY   0x80
#define AHCI_DEV_DRQ    0x08
#define HBA_PxIS_TFES   (1 << 30)

#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct __attribute__((packed)) {
    uint8_t cfl:5;
    uint8_t a:1;
    uint8_t w:1;
    uint8_t p:1;
    uint8_t r:1;
    uint8_t b:1;
    uint8_t c:1;
    uint8_t rsv0:1;
    uint8_t pmp:4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} hba_cmd_header_t;

typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} hba_prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt_entry[1];
} hba_cmd_tbl_t;

typedef struct __attribute__((packed)) {
    uint8_t  fis_type;
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} fis_reg_h2d_t;

typedef struct {
    hba_port_t *port;
    paddr_t mem_phys;
    void *mem_virt;
    hba_cmd_header_t *cmd_headers;
    hba_cmd_tbl_t *cmd_table;
    void *bounce_virt;
    paddr_t bounce_phys;
} ahci_device_t;

#define MAX_AHCI_DISKS 8
static ahci_device_t ahci_devices[MAX_AHCI_DISKS];
static block_device_t ahci_block_devs[MAX_AHCI_DISKS];
static int ahci_disk_count = 0;

static void ahci_stop_port(hba_port_t *port) {
    port->cmd &= ~1; // Clear ST
    int timeout = 100000;
    while ((port->cmd & (1 << 15)) && --timeout) io_wait();

    port->cmd &= ~(1 << 4); // Clear FRE
    timeout = 100000;
    while ((port->cmd & (1 << 14)) && --timeout) io_wait();
}

static void ahci_start_port(hba_port_t *port) {
    int timeout = 100000;
    while ((port->cmd & (1 << 15)) && --timeout) io_wait();

    port->cmd |= (1 << 4); // Set FRE
    port->cmd |= (1 << 0); // Set ST
}

static int ahci_issue_command(hba_port_t *port) {
    port->serr = 0xFFFFFFFF;
    port->is = 0xFFFFFFFF;

    int spin = 0;
    while ((port->tfd & (AHCI_DEV_BUSY | AHCI_DEV_DRQ)) && spin < 1000000) {
        spin++;
        io_wait();
    }
    if (spin == 1000000) {
        return -1;
    }

    port->ci = 1; // Issue slot 0

    while (1) {
        if ((port->ci & 1) == 0) break;
        if (port->is & HBA_PxIS_TFES) {
            return -1;
        }
        io_wait();
    }

    if (port->is & HBA_PxIS_TFES) {
        return -1;
    }
    return 0;
}

static int ahci_read_sectors(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer) {
    ahci_device_t *adev = (ahci_device_t *)dev->priv;
    if (!adev || !adev->port) return -1;
    hba_port_t *port = adev->port;

    uint8_t *dst = (uint8_t *)buffer;
    uint32_t max_chunk_sectors = 16; // 8 KB per bounce transfer

    while (count > 0) {
        uint32_t chunk = (count > max_chunk_sectors) ? max_chunk_sectors : count;
        uint32_t byte_count = chunk * 512;

        hba_cmd_header_t *cmd_hdr = &adev->cmd_headers[0];
        memset(cmd_hdr, 0, sizeof(hba_cmd_header_t));
        cmd_hdr->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
        cmd_hdr->w = 0; // Read
        cmd_hdr->prdtl = 1;
        cmd_hdr->ctba = (uint32_t)(adev->mem_phys + 1280);
        cmd_hdr->ctbau = (uint32_t)((adev->mem_phys + 1280) >> 32);

        hba_cmd_tbl_t *cmd_tbl = adev->cmd_table;
        memset(cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
        cmd_tbl->prdt_entry[0].dba = (uint32_t)adev->bounce_phys;
        cmd_tbl->prdt_entry[0].dbau = (uint32_t)(adev->bounce_phys >> 32);
        cmd_tbl->prdt_entry[0].dbc = byte_count - 1;
        cmd_tbl->prdt_entry[0].i = 1;

        fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)cmd_tbl->cfis;
        cmdfis->fis_type = 0x27;
        cmdfis->c = 1;
        cmdfis->command = ATA_CMD_READ_DMA_EXT;
        cmdfis->device = 1 << 6; // LBA mode

        cmdfis->lba0 = (uint8_t)lba;
        cmdfis->lba1 = (uint8_t)(lba >> 8);
        cmdfis->lba2 = (uint8_t)(lba >> 16);
        cmdfis->lba3 = (uint8_t)(lba >> 24);
        cmdfis->lba4 = (uint8_t)(lba >> 32);
        cmdfis->lba5 = (uint8_t)(lba >> 40);

        cmdfis->countl = (uint8_t)(chunk & 0xFF);
        cmdfis->counth = (uint8_t)((chunk >> 8) & 0xFF);

        if (ahci_issue_command(port) != 0) {
            return -1;
        }

        memcpy(dst, adev->bounce_virt, byte_count);

        dst += byte_count;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_write_sectors(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer) {
    ahci_device_t *adev = (ahci_device_t *)dev->priv;
    if (!adev || !adev->port) return -1;
    hba_port_t *port = adev->port;

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t max_chunk_sectors = 16;

    while (count > 0) {
        uint32_t chunk = (count > max_chunk_sectors) ? max_chunk_sectors : count;
        uint32_t byte_count = chunk * 512;

        memcpy(adev->bounce_virt, src, byte_count);

        hba_cmd_header_t *cmd_hdr = &adev->cmd_headers[0];
        memset(cmd_hdr, 0, sizeof(hba_cmd_header_t));
        cmd_hdr->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
        cmd_hdr->w = 1; // Write
        cmd_hdr->prdtl = 1;
        cmd_hdr->ctba = (uint32_t)(adev->mem_phys + 1280);
        cmd_hdr->ctbau = (uint32_t)((adev->mem_phys + 1280) >> 32);

        hba_cmd_tbl_t *cmd_tbl = adev->cmd_table;
        memset(cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
        cmd_tbl->prdt_entry[0].dba = (uint32_t)adev->bounce_phys;
        cmd_tbl->prdt_entry[0].dbau = (uint32_t)(adev->bounce_phys >> 32);
        cmd_tbl->prdt_entry[0].dbc = byte_count - 1;
        cmd_tbl->prdt_entry[0].i = 1;

        fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)cmd_tbl->cfis;
        cmdfis->fis_type = 0x27;
        cmdfis->c = 1;
        cmdfis->command = ATA_CMD_WRITE_DMA_EXT;
        cmdfis->device = 1 << 6; // LBA mode

        cmdfis->lba0 = (uint8_t)lba;
        cmdfis->lba1 = (uint8_t)(lba >> 8);
        cmdfis->lba2 = (uint8_t)(lba >> 16);
        cmdfis->lba3 = (uint8_t)(lba >> 24);
        cmdfis->lba4 = (uint8_t)(lba >> 32);
        cmdfis->lba5 = (uint8_t)(lba >> 40);

        cmdfis->countl = (uint8_t)(chunk & 0xFF);
        cmdfis->counth = (uint8_t)((chunk >> 8) & 0xFF);

        if (ahci_issue_command(port) != 0) {
            return -1;
        }

        src += byte_count;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static uint64_t ahci_identify_device(ahci_device_t *adev) {
    hba_port_t *port = adev->port;

    hba_cmd_header_t *cmd_hdr = &adev->cmd_headers[0];
    memset(cmd_hdr, 0, sizeof(hba_cmd_header_t));
    cmd_hdr->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_hdr->w = 0; // Read
    cmd_hdr->prdtl = 1;
    cmd_hdr->ctba = (uint32_t)(adev->mem_phys + 1280);
    cmd_hdr->ctbau = (uint32_t)((adev->mem_phys + 1280) >> 32);

    hba_cmd_tbl_t *cmd_tbl = adev->cmd_table;
    memset(cmd_tbl, 0, sizeof(hba_cmd_tbl_t));
    cmd_tbl->prdt_entry[0].dba = (uint32_t)adev->bounce_phys;
    cmd_tbl->prdt_entry[0].dbau = (uint32_t)(adev->bounce_phys >> 32);
    cmd_tbl->prdt_entry[0].dbc = 512 - 1;
    cmd_tbl->prdt_entry[0].i = 1;

    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)cmd_tbl->cfis;
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = ATA_CMD_IDENTIFY;
    cmdfis->device = 0;

    if (ahci_issue_command(port) != 0) {
        return 0;
    }

    uint16_t *buf = (uint16_t *)adev->bounce_virt;
    uint32_t sectors_28 = buf[60] | ((uint32_t)buf[61] << 16);
    uint64_t sectors_48 = 0;
    if (buf[83] & (1 << 10)) { // 48-bit address supported
        sectors_48 = (uint64_t)buf[100] |
                     ((uint64_t)buf[101] << 16) |
                     ((uint64_t)buf[102] << 32) |
                     ((uint64_t)buf[103] << 48);
    }

    return (sectors_48 > 0) ? sectors_48 : (uint64_t)sectors_28;
}

static void probe_sata_port(hba_port_t *port, int port_no) {
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3 || ipm != 1) {
        return; // No drive or not active
    }

    uint32_t sig = port->sig;
    if (sig != SATA_SIG_ATA) {
        return; // Ignore ATAPI (CD-ROM) or port multiplier for raw disk driver
    }

    if (ahci_disk_count >= MAX_AHCI_DISKS) return;

    // Allocate 2 physical pages: 1 for headers/structures, 1 for 4KB bounce buffer
    paddr_t mem_page = pmm_alloc_pages(2);
    if (!mem_page) return;

    void *mem_virt = vmm_phys_to_virt(mem_page);
    memset(mem_virt, 0, 8192);

    ahci_device_t *adev = &ahci_devices[ahci_disk_count];
    adev->port = port;
    adev->mem_phys = mem_page;
    adev->mem_virt = mem_virt;
    adev->cmd_headers = (hba_cmd_header_t *)mem_virt;
    adev->cmd_table = (hba_cmd_tbl_t *)((uint8_t *)mem_virt + 1280);
    adev->bounce_phys = mem_page + 4096;
    adev->bounce_virt = (void *)((uint8_t *)mem_virt + 4096);

    ahci_stop_port(port);

    port->clb = (uint32_t)adev->mem_phys;
    port->clbu = (uint32_t)(adev->mem_phys >> 32);
    port->fb = (uint32_t)(adev->mem_phys + 1024);
    port->fbu = (uint32_t)((adev->mem_phys + 1024) >> 32);

    port->serr = 0xFFFFFFFF;
    port->is = 0xFFFFFFFF;

    ahci_start_port(port);

    uint64_t sectors = ahci_identify_device(adev);
    if (sectors == 0) {
        pmm_free_pages(mem_page, 2);
        return;
    }

    block_device_t *bdev = &ahci_block_devs[ahci_disk_count];
    snprintf(bdev->name, sizeof(bdev->name), "sd%c", 'a' + ahci_disk_count);
    bdev->sector_count = sectors;
    bdev->sector_size = 512;
    bdev->read_sectors = ahci_read_sectors;
    bdev->write_sectors = ahci_write_sectors;
    bdev->priv = adev;

    block_register_device(bdev);

    printk("AHCI: %s (Port %d) online: %llu MB (%llu sectors)\n",
           bdev->name, port_no,
           (bdev->sector_count * 512) / (1024 * 1024),
           bdev->sector_count);

    ahci_disk_count++;
}

void ahci_init(void) {
    pci_device_t *pci_dev = pci_find_class(0x01, 0x06); // Class 01 (Mass Storage), Subclass 06 (SATA)
    if (!pci_dev) {
        return; // No AHCI SATA controller present
    }

    // Enable PCI Bus Mastering and Memory Space
    uint32_t cmd = pci_read_config32(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04);
    pci_write_config32(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, cmd | 0x07);

    uint64_t abar_phys = pci_dev->bar[5] & ~0xFFF;
    if (abar_phys == 0) return;

    address_space_t *as = vmm_get_kernel_address_space();
    uint64_t abar_virt = abar_phys + vmm_get_hhdm();
    for (uint64_t p = 0; p < 4; p++) {
        vmm_map_page(as->pml4_virt, abar_virt + (p * 4096), abar_phys + (p * 4096), PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    }

    hba_mem_t *abar = (hba_mem_t *)abar_virt;

    // Request AHCI Enable (GHC.AE = 1)
    abar->ghc |= (1U << 31);

    uint32_t pi = abar->pi;
    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            probe_sata_port(&abar->ports[i], i);
        }
    }
}
