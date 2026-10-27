#ifndef FS_MKFS_H
#define FS_MKFS_H

#include <kernel/types.h>
#include <drivers/block.h>

// Format a block device with ext4 filesystem
int mkfs_ext4_format(block_device_t *dev);

// Add directory into an existing ext4 filesystem formatted by mkfs
int mkfs_ext4_add_dir(block_device_t *dev, uint32_t parent_ino, const char *name, uint32_t *new_ino_out);

// Add file with data into an existing ext4 filesystem formatted by mkfs
int mkfs_ext4_add_file(block_device_t *dev, uint32_t parent_ino, const char *name, const void *data, size_t size);

// Shell command handler for mkfs.ext4 / mkfs
void cmd_mkfs_ext4(int argc, char **argv);

#endif // FS_MKFS_H
