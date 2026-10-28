#include <drivers/block.h>
#include <hal/cpu.h>
#include <mm/heap.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define ATA_CMD_READ_PIO         0x20
#define ATA_CMD_READ_PIO_EXT     0x24
#define ATA_CMD_WRITE_PIO        0x30
#define ATA_CMD_WRITE_PIO_EXT    0x34
#define ATA_CMD_IDENTIFY         0xEC

#define ATA_SR_BSY               0x80
#define ATA_SR_DRDY              0x40
#define ATA_SR_DRQ               0x08
#define ATA_SR_ERR               0x01

typedef struct {
    uint16_t io_base;
    uint8_t  drive_sel; // 0xA0 (master) or 0xB0 (slave)
} ata_drive_info_t;

static block_device_t ata_devices[4];
static ata_drive_info_t ata_info[4];

static int ata_wait_bsy(uint16_t io_base) {
    int timeout = 100000;
    while ((inb(io_base + 7) & ATA_SR_BSY) && --timeout) {
        io_wait();
    }
    return timeout > 0 ? 0 : -1;
}

static int ata_wait_drq(uint16_t io_base) {
    int timeout = 100000;
    while (!(inb(io_base + 7) & ATA_SR_DRQ) && --timeout) {
        if (inb(io_base + 7) & ATA_SR_ERR) {
            return -1;
        }
        io_wait();
    }
    return timeout > 0 ? 0 : -1;
}

static int ata_read_sectors28(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer) {
    ata_drive_info_t *info = (ata_drive_info_t *)dev->priv;
    if (!info) return -1;
    uint16_t io = info->io_base;
    uint8_t drive_bit = (info->drive_sel == 0xB0) ? 0x10 : 0x00;
    uint16_t *buf = (uint16_t *)buffer;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_lba = (uint32_t)(lba + i);

        if (ata_wait_bsy(io) != 0) return -1;

        outb(io + 6, 0xE0 | drive_bit | ((cur_lba >> 24) & 0x0F));
        outb(io + 2, 1);
        outb(io + 3, (uint8_t)cur_lba);
        outb(io + 4, (uint8_t)(cur_lba >> 8));
        outb(io + 5, (uint8_t)(cur_lba >> 16));
        outb(io + 7, ATA_CMD_READ_PIO);

        if (ata_wait_bsy(io) != 0) return -1;
        if (ata_wait_drq(io) != 0) return -1;

        for (int w = 0; w < 256; w++) {
            *buf++ = inw(io + 0);
        }
    }
    return 0;
}

static int ata_write_sectors28(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer) {
    ata_drive_info_t *info = (ata_drive_info_t *)dev->priv;
    if (!info) return -1;
    uint16_t io = info->io_base;
    uint8_t drive_bit = (info->drive_sel == 0xB0) ? 0x10 : 0x00;
    const uint16_t *buf = (const uint16_t *)buffer;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_lba = (uint32_t)(lba + i);

        if (ata_wait_bsy(io) != 0) return -1;

        outb(io + 6, 0xE0 | drive_bit | ((cur_lba >> 24) & 0x0F));
        outb(io + 2, 1);
        outb(io + 3, (uint8_t)cur_lba);
        outb(io + 4, (uint8_t)(cur_lba >> 8));
        outb(io + 5, (uint8_t)(cur_lba >> 16));
        outb(io + 7, ATA_CMD_WRITE_PIO);

        if (ata_wait_bsy(io) != 0) return -1;
        if (ata_wait_drq(io) != 0) return -1;

        for (int w = 0; w < 256; w++) {
            outw(io + 0, *buf++);
        }
        io_wait();
    }
    return 0;
}

static void ata_probe_drive(int idx, uint16_t io_base, uint8_t drive_sel, const char *name) {
    outb(io_base + 6, drive_sel);
    for (int i = 0; i < 4; i++) io_wait();

    uint8_t status = inb(io_base + 7);
    if (status == 0xFF) return;

    outb(io_base + 2, 0);
    outb(io_base + 3, 0);
    outb(io_base + 4, 0);
    outb(io_base + 5, 0);

    outb(io_base + 7, ATA_CMD_IDENTIFY);
    for (int i = 0; i < 4; i++) io_wait();

    status = inb(io_base + 7);
    if (status == 0 || status == 0xFF) {
        return; // No device present
    }

    int timeout = 20000;
    while ((inb(io_base + 7) & ATA_SR_BSY) && --timeout) {
        io_wait();
    }
    if (timeout <= 0) return;

    uint8_t cl = inb(io_base + 4);
    uint8_t ch = inb(io_base + 5);
    if (cl != 0 || ch != 0) {
        return; // ATAPI device or not ATA
    }

    timeout = 20000;
    while (--timeout) {
        status = inb(io_base + 7);
        if (status & ATA_SR_ERR) return;
        if (status & ATA_SR_DRQ) break;
        io_wait();
    }
    if (timeout <= 0) return;

    uint16_t identify_buf[256];
    for (int i = 0; i < 256; i++) {
        identify_buf[i] = inw(io_base + 0);
    }

    uint32_t sectors_28 = identify_buf[60] | ((uint32_t)identify_buf[61] << 16);
    if (sectors_28 == 0) return;

    ata_info[idx].io_base = io_base;
    ata_info[idx].drive_sel = drive_sel;

    strcpy(ata_devices[idx].name, name);
    ata_devices[idx].sector_count = sectors_28;
    ata_devices[idx].sector_size = 512;
    ata_devices[idx].read_sectors = ata_read_sectors28;
    ata_devices[idx].write_sectors = ata_write_sectors28;
    ata_devices[idx].priv = &ata_info[idx];

    block_register_device(&ata_devices[idx]);

    printk("ATA: %s online: %llu MB (%llu sectors)\n",
           name,
           (uint64_t)(ata_devices[idx].sector_count * 512) / (1024 * 1024),
           (uint64_t)ata_devices[idx].sector_count);
}

void ata_init(void) {
    ata_probe_drive(0, 0x1F0, 0xA0, "hda"); // Primary Master
    ata_probe_drive(1, 0x1F0, 0xB0, "hdb"); // Primary Slave
    ata_probe_drive(2, 0x170, 0xA0, "hdc"); // Secondary Master
    ata_probe_drive(3, 0x170, 0xB0, "hdd"); // Secondary Slave
}
