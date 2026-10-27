#include <drivers/partition.h>
#include <mm/heap.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define MAX_PARTITION_DEVICES 16

typedef struct {
    block_device_t *parent;
    uint64_t start_lba;
    uint32_t part_index;
    block_device_t dev;
} partition_dev_t;

static partition_dev_t part_devices[MAX_PARTITION_DEVICES];
static int part_dev_count = 0;

static int partition_read_sectors(block_device_t *dev, uint64_t lba, uint32_t count, void *buffer) {
    partition_dev_t *p = (partition_dev_t *)dev->priv;
    if (!p || !p->parent) return -1;
    if (lba + count > dev->sector_count) return -1;
    return p->parent->read_sectors(p->parent, p->start_lba + lba, count, buffer);
}

static int partition_write_sectors(block_device_t *dev, uint64_t lba, uint32_t count, const void *buffer) {
    partition_dev_t *p = (partition_dev_t *)dev->priv;
    if (!p || !p->parent) return -1;
    if (lba + count > dev->sector_count) return -1;
    return p->parent->write_sectors(p->parent, p->start_lba + lba, count, buffer);
}

uint64_t partition_get_start_lba(block_device_t *dev) {
    if (!dev || !dev->priv) return 0;
    partition_dev_t *p = (partition_dev_t *)dev->priv;
    return p->start_lba;
}

int partition_scan_disk(block_device_t *parent) {
    if (!parent || !parent->read_sectors) return -1;

    // Do not scan child partitions recursively (e.g. hda1)
    if (parent->priv != NULL && parent->name[strlen(parent->name) - 1] >= '0' &&
        parent->name[strlen(parent->name) - 1] <= '9') {
        return 0;
    }

    mbr_sector_t mbr;
    if (parent->read_sectors(parent, 0, 1, &mbr) != 0) {
        return -1;
    }

    if (mbr.signature != MBR_SIGNATURE) {
        return 0; // Not MBR partitioned
    }

    int found = 0;
    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        mbr_partition_entry_t *e = &mbr.entries[i];
        if (e->partition_type == PARTITION_TYPE_EMPTY || e->sector_count == 0) {
            continue;
        }

        char name[32];
        snprintf(name, sizeof(name), "%s%d", parent->name, i + 1);

        // Check if already registered
        block_device_t *existing = block_get_device(name);
        if (existing) {
            found++;
            continue;
        }

        if (part_dev_count >= MAX_PARTITION_DEVICES) break;

        partition_dev_t *p = &part_devices[part_dev_count++];
        p->parent = parent;
        p->start_lba = e->start_lba;
        p->part_index = i + 1;

        strncpy(p->dev.name, name, sizeof(p->dev.name) - 1);
        p->dev.sector_count = e->sector_count;
        p->dev.sector_size = parent->sector_size;
        p->dev.read_sectors = partition_read_sectors;
        p->dev.write_sectors = partition_write_sectors;
        p->dev.priv = p;

        block_register_device(&p->dev);
        found++;

        printk("Partition: %s online (Start LBA: %u, Size: %llu MB, Type: 0x%02x%s)\n",
               p->dev.name, e->start_lba,
               (uint64_t)(e->sector_count * 512) / (1024 * 1024),
               e->partition_type,
               (e->boot_indicator & 0x80) ? " [Bootable]" : "");
    }

    return found;
}

void partition_scan_all(void) {
    block_device_t *dev = block_get_first_device();
    while (dev) {
        // Only scan top-level physical disks (hda, hdb, hdc, hdd)
        size_t len = strlen(dev->name);
        if (len > 0 && !(dev->name[len - 1] >= '0' && dev->name[len - 1] <= '9')) {
            partition_scan_disk(dev);
        }
        dev = dev->next;
    }
}

int partition_create_mbr(block_device_t *parent, uint32_t part_index, uint8_t type, uint64_t start_lba, uint64_t sector_count, bool bootable) {
    if (!parent || !parent->write_sectors || part_index < 1 || part_index > 4) {
        return -1;
    }

    mbr_sector_t mbr;
    if (parent->read_sectors(parent, 0, 1, &mbr) != 0 || mbr.signature != MBR_SIGNATURE) {
        memset(&mbr, 0, sizeof(mbr));
        mbr.signature = MBR_SIGNATURE;
    }

    mbr_partition_entry_t *e = &mbr.entries[part_index - 1];
    e->boot_indicator = bootable ? 0x80 : 0x00;
    e->partition_type = type;
    e->start_lba = (uint32_t)start_lba;
    e->sector_count = (uint32_t)sector_count;

    // Fake simple LBA-to-CHS for compatibility
    e->start_chs[0] = 0x00; e->start_chs[1] = 0x02; e->start_chs[2] = 0x00;
    e->end_chs[0] = 0xFF; e->end_chs[1] = 0xFF; e->end_chs[2] = 0xFF;

    if (parent->write_sectors(parent, 0, 1, &mbr) != 0) {
        printk("fdisk: Error writing MBR sector to %s!\n", parent->name);
        return -1;
    }

    // Re-scan partitions
    partition_scan_disk(parent);
    return 0;
}

int partition_delete_mbr(block_device_t *parent, uint32_t part_index) {
    if (!parent || !parent->write_sectors || part_index < 1 || part_index > 4) {
        return -1;
    }

    mbr_sector_t mbr;
    if (parent->read_sectors(parent, 0, 1, &mbr) != 0 || mbr.signature != MBR_SIGNATURE) {
        return -1;
    }

    memset(&mbr.entries[part_index - 1], 0, sizeof(mbr_partition_entry_t));

    if (parent->write_sectors(parent, 0, 1, &mbr) != 0) {
        return -1;
    }

    char name[32];
    snprintf(name, sizeof(name), "%s%d", parent->name, part_index);
    block_device_t *dev = block_get_device(name);
    if (dev) {
        block_unregister_device(dev);
    }

    return 0;
}

void cmd_fdisk(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-l") == 0) {
        printk("\n=======================================================\n");
        printk("               Detected Storage Devices                \n");
        printk("=======================================================\n");

        block_device_t *dev = block_get_first_device();
        if (!dev) {
            printk("No storage devices found.\n");
            return;
        }

        while (dev) {
            uint64_t mb = (dev->sector_count * dev->sector_size) / (1024 * 1024);
            size_t len = strlen(dev->name);
            bool is_part = (len > 0 && (dev->name[len - 1] >= '0' && dev->name[len - 1] <= '9'));

            if (!is_part) {
                printk("\n[DISK] %s: %llu MB (%llu sectors, sector size %u bytes)\n",
                       dev->name, mb, dev->sector_count, dev->sector_size);

                mbr_sector_t mbr;
                if (dev->read_sectors && dev->read_sectors(dev, 0, 1, &mbr) == 0 && mbr.signature == MBR_SIGNATURE) {
                    for (int i = 0; i < 4; i++) {
                        mbr_partition_entry_t *e = &mbr.entries[i];
                        if (e->partition_type != 0 && e->sector_count > 0) {
                            uint64_t p_mb = ((uint64_t)e->sector_count * 512) / (1024 * 1024);
                            const char *tname = (e->partition_type == 0x83) ? "Linux ext4" :
                                                (e->partition_type == 0x0C) ? "FAT32 LBA" : "Other";
                            printk("  -> %s%d: Start %u, %llu MB, Type 0x%02x (%s)%s\n",
                                   dev->name, i + 1, e->start_lba, p_mb,
                                   e->partition_type, tname,
                                   (e->boot_indicator & 0x80) ? " *" : "");
                        }
                    }
                } else {
                    printk("  (No partition table found - raw unpartitioned disk)\n");
                }
            }
            dev = dev->next;
        }
        printk("\nUsage:\n");
        printk("  fdisk -l                       - List all disks & partitions\n");
        printk("  fdisk create <disk> [size_mb]  - Create partition 1 on <disk>\n");
        printk("  fdisk delete <disk> <part_num> - Delete partition\n");
        return;
    }

    if (strcmp(argv[1], "create") == 0) {
        if (argc < 3) {
            printk("Usage: fdisk create <disk> [size_mb]\nExample: fdisk create hda\n");
            return;
        }
        const char *disk_name = argv[2];
        block_device_t *disk = block_get_device(disk_name);
        if (!disk) {
            printk("fdisk: Device '%s' not found.\n", disk_name);
            return;
        }

        uint64_t start_lba = 2048; // 1 MB alignment
        if (disk->sector_count <= start_lba) {
            printk("fdisk: Disk '%s' is too small to partition!\n", disk_name);
            return;
        }

        uint64_t sector_count = disk->sector_count - start_lba;
        if (argc >= 4) {
            uint64_t req_mb = 0;
            const char *p = argv[3];
            while (*p >= '0' && *p <= '9') req_mb = req_mb * 10 + (*p++ - '0');
            if (req_mb > 0) {
                uint64_t req_sec = (req_mb * 1024 * 1024) / 512;
                if (req_sec < sector_count) sector_count = req_sec;
            }
        }

        printk("fdisk: Creating MBR partition 1 on %s...\n", disk->name);
        printk("  Start LBA:    %llu\n", start_lba);
        printk("  Sectors:      %llu (%llu MB)\n", sector_count, (sector_count * 512) / (1024 * 1024));
        printk("  Type:         0x83 (Linux ext4)\n");
        printk("  Bootable:     Yes\n");

        if (partition_create_mbr(disk, 1, PARTITION_TYPE_LINUX, start_lba, sector_count, true) == 0) {
            printk("fdisk: Partition %s1 created successfully!\n", disk->name);
        } else {
            printk("fdisk: Failed to create partition on %s!\n", disk->name);
        }
        return;
    }

    if (strcmp(argv[1], "delete") == 0) {
        if (argc < 4) {
            printk("Usage: fdisk delete <disk> <partition_index_1_to_4>\n");
            return;
        }
        block_device_t *disk = block_get_device(argv[2]);
        int idx = argv[3][0] - '0';
        if (!disk || idx < 1 || idx > 4) {
            printk("fdisk: Invalid disk or partition index.\n");
            return;
        }

        if (partition_delete_mbr(disk, idx) == 0) {
            printk("fdisk: Partition %s%d deleted successfully.\n", disk->name, idx);
        } else {
            printk("fdisk: Failed to delete partition %s%d.\n", disk->name, idx);
        }
        return;
    }

    printk("Unknown fdisk option '%s'. Type 'fdisk' for help.\n", argv[1]);
}
