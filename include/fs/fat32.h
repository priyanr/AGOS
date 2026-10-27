#ifndef FS_FAT32_H
#define FS_FAT32_H

#include <kernel/types.h>
#include <drivers/block.h>

// Format a bootable FAT32 partition with Limine BIOS boot files
int mkfs_fat32_boot_format(block_device_t *dev, uint64_t start_lba,
                           const void *k_data, size_t k_size,
                           const void *sys_data, size_t sys_size,
                           const char *conf_data, size_t conf_size);

#endif // FS_FAT32_H
