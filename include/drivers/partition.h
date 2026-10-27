#ifndef DRIVERS_PARTITION_H
#define DRIVERS_PARTITION_H

#include <kernel/types.h>
#include <drivers/block.h>

#define MBR_SIGNATURE 0xAA55
#define MBR_PARTITION_COUNT 4

#define PARTITION_TYPE_EMPTY   0x00
#define PARTITION_TYPE_FAT32   0x0C
#define PARTITION_TYPE_LINUX   0x83 // Linux ext2/ext3/ext4

typedef struct __attribute__((packed)) {
    uint8_t  boot_indicator; // 0x80 = active/bootable, 0x00 = inactive
    uint8_t  start_chs[3];
    uint8_t  partition_type; // 0x83 = Linux ext4
    uint8_t  end_chs[3];
    uint32_t start_lba;      // Sector offset from start of disk
    uint32_t sector_count;   // Total sectors in partition
} mbr_partition_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t bootstrap_code[446];
    mbr_partition_entry_t entries[MBR_PARTITION_COUNT];
    uint16_t signature; // 0xAA55
} mbr_sector_t;

// Scan all physical disks and register partitions (e.g. hda1)
void partition_scan_all(void);

// Scan a specific disk for MBR partitions and register child block devices (e.g. hda1)
int partition_scan_disk(block_device_t *parent);

// Create an MBR partition table on a disk
int partition_create_mbr(block_device_t *parent, uint32_t part_index, uint8_t type, uint64_t start_lba, uint64_t sector_count, bool bootable);

// Delete an MBR partition entry
int partition_delete_mbr(block_device_t *parent, uint32_t part_index);

// Get start LBA of a partition block device
uint64_t partition_get_start_lba(block_device_t *dev);

// Shell command handler
void cmd_fdisk(int argc, char **argv);

#endif // DRIVERS_PARTITION_H
