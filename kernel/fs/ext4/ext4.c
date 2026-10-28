#include <fs/ext4.h>
#include <mm/heap.h>
#include <kernel/string.h>
#include <kernel/printk.h>

static int ext4_read_block(ext4_fs_t *fs, uint64_t block_num, void *buffer) {
    uint32_t sectors_per_block = fs->block_size / fs->dev->sector_size;
    uint64_t lba = fs->part_lba_start + (block_num * sectors_per_block);
    return fs->dev->read_sectors(fs->dev, lba, sectors_per_block, buffer);
}

static int ext4_read_inode(ext4_fs_t *fs, uint32_t ino, ext4_inode_t *inode_out) {
    if (ino == 0) return -1;

    uint32_t group = (ino - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (ino - 1) % fs->sb.s_inodes_per_group;

    uint64_t inode_table_block = fs->bgds[group].bg_inode_table_lo;
    uint32_t inode_size = fs->sb.s_inode_size ? fs->sb.s_inode_size : 128;
    uint64_t byte_offset = (uint64_t)index * inode_size;
    uint64_t block_offset = byte_offset / fs->block_size;
    uint32_t in_block_offset = byte_offset % fs->block_size;

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext4_read_block(fs, inode_table_block + block_offset, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    memcpy(inode_out, block_buf + in_block_offset, sizeof(ext4_inode_t));
    kfree(block_buf);
    return 0;
}

static uint64_t ext4_find_block_in_extent(ext4_fs_t *fs, ext4_extent_header_t *eh, uint32_t logical_block) {
    if (eh->eh_magic != EXT4_EXTENTS_MAGIC) {
        return 0;
    }

    if (eh->eh_depth == 0) {
        ext4_extent_t *ext = (ext4_extent_t *)(eh + 1);
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (logical_block >= ext[i].ee_block && logical_block < ext[i].ee_block + ext[i].ee_len) {
                uint64_t start = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
                return start + (logical_block - ext[i].ee_block);
            }
        }
        return 0;
    } else {
        ext4_extent_idx_t *idx = (ext4_extent_idx_t *)(eh + 1);
        uint64_t next_block = 0;
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (logical_block >= idx[i].ei_block) {
                next_block = ((uint64_t)idx[i].ei_leaf_hi << 32) | idx[i].ei_leaf_lo;
            } else {
                break;
            }
        }

        if (next_block == 0) return 0;

        uint8_t *child_buf = kmalloc(fs->block_size);
        if (!child_buf) return 0;

        if (ext4_read_block(fs, next_block, child_buf) != 0) {
            kfree(child_buf);
            return 0;
        }

        uint64_t phys = ext4_find_block_in_extent(fs, (ext4_extent_header_t *)child_buf, logical_block);
        kfree(child_buf);
        return phys;
    }
}

static uint64_t ext4_map_block(ext4_fs_t *fs, ext4_inode_t *inode, uint32_t logical_block) {
    ext4_extent_header_t *eh = (ext4_extent_header_t *)inode->i_block;
    return ext4_find_block_in_extent(fs, eh, logical_block);
}

typedef struct {
    ext4_fs_t *fs;
    uint32_t ino;
    ext4_inode_t inode;
} ext4_node_data_t;

static ssize_t ext4_vfs_read(vfs_node_t *node, uint64_t offset, size_t size, void *buffer) {
    ext4_node_data_t *data = (ext4_node_data_t *)node->ptr;
    ext4_fs_t *fs = data->fs;
    uint64_t file_size = ((uint64_t)data->inode.i_size_high << 32) | data->inode.i_size_lo;

    if (offset >= file_size) return 0;
    if (offset + size > file_size) {
        size = file_size - offset;
    }

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    size_t bytes_read = 0;
    uint8_t *dest = (uint8_t *)buffer;

    while (bytes_read < size) {
        uint64_t cur_offset = offset + bytes_read;
        uint32_t logical_block = cur_offset / fs->block_size;
        uint32_t in_block = cur_offset % fs->block_size;
        size_t chunk = fs->block_size - in_block;
        if (chunk > size - bytes_read) {
            chunk = size - bytes_read;
        }

        uint64_t phys_block = ext4_map_block(fs, &data->inode, logical_block);
        if (phys_block == 0) {
            memset(dest + bytes_read, 0, chunk); // Sparse file zero-fill
        } else {
            if (ext4_read_block(fs, phys_block, block_buf) != 0) {
                kfree(block_buf);
                return -1;
            }
            memcpy(dest + bytes_read, block_buf + in_block, chunk);
        }

        bytes_read += chunk;
    }

    kfree(block_buf);
    return bytes_read;
}

static dirent_t shared_dirent;

static dirent_t *ext4_vfs_readdir(vfs_node_t *node, uint32_t index) {
    ext4_node_data_t *data = (ext4_node_data_t *)node->ptr;
    ext4_fs_t *fs = data->fs;
    uint64_t dir_size = ((uint64_t)data->inode.i_size_high << 32) | data->inode.i_size_lo;

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return NULL;

    uint32_t cur_idx = 0;
    uint64_t offset = 0;

    while (offset < dir_size) {
        uint32_t logical_block = offset / fs->block_size;
        uint64_t phys_block = ext4_map_block(fs, &data->inode, logical_block);
        if (phys_block == 0 || ext4_read_block(fs, phys_block, block_buf) != 0) {
            break;
        }

        uint32_t block_offset = 0;
        while (block_offset < fs->block_size && (offset + block_offset) < dir_size) {
            ext4_dir_entry_2_t *entry = (ext4_dir_entry_2_t *)(block_buf + block_offset);
            if (entry->rec_len == 0) break;

            if (entry->inode != 0) {
                if (cur_idx == index) {
                    size_t nlen = entry->name_len < 127 ? entry->name_len : 127;
                    memcpy(shared_dirent.name, entry->name, nlen);
                    shared_dirent.name[nlen] = '\0';
                    shared_dirent.ino = entry->inode;
                    shared_dirent.type = entry->file_type;
                    kfree(block_buf);
                    return &shared_dirent;
                }
                cur_idx++;
            }

            block_offset += entry->rec_len;
        }
        offset += fs->block_size;
    }

    kfree(block_buf);
    return NULL;
}

static vfs_node_t *ext4_create_vfs_node(ext4_fs_t *fs, uint32_t ino, const char *name);

static vfs_node_t *ext4_vfs_finddir(vfs_node_t *node, const char *name) {
    uint32_t idx = 0;
    dirent_t *d;
    while ((d = ext4_vfs_readdir(node, idx++)) != NULL) {
        const char *entry_name = d->name;
        if (entry_name[0] == '/') entry_name++;
        if (strcmp(entry_name, name) == 0) {
            ext4_node_data_t *data = (ext4_node_data_t *)node->ptr;
            return ext4_create_vfs_node(data->fs, d->ino, entry_name);
        }
    }
    return NULL;
}

static vfs_node_t *ext4_create_vfs_node(ext4_fs_t *fs, uint32_t ino, const char *name) {
    ext4_node_data_t *data = kzalloc(sizeof(ext4_node_data_t));
    if (!data) return NULL;

    data->fs = fs;
    data->ino = ino;
    if (ext4_read_inode(fs, ino, &data->inode) != 0) {
        kfree(data);
        return NULL;
    }

    vfs_node_t *node = kzalloc(sizeof(vfs_node_t));
    if (!node) {
        kfree(data);
        return NULL;
    }

    strncpy(node->name, name ? name : "", sizeof(node->name) - 1);
    node->inode = ino;
    node->size = ((uint64_t)data->inode.i_size_high << 32) | data->inode.i_size_lo;
    node->ptr = (vfs_node_t *)data;

    // Check mode: directory vs regular file
    if ((data->inode.i_mode & 0xF000) == 0x4000) {
        node->flags = FS_DIRECTORY;
        node->readdir = ext4_vfs_readdir;
        node->finddir = ext4_vfs_finddir;
    } else {
        node->flags = FS_FILE;
        node->read = ext4_vfs_read;
    }

    return node;
}

vfs_node_t *ext4_mount(block_device_t *dev, uint64_t lba_offset) {
    if (!dev) return NULL;

    ext4_fs_t *fs = kzalloc(sizeof(ext4_fs_t));
    if (!fs) return NULL;

    fs->dev = dev;
    fs->part_lba_start = lba_offset;

    // Read superblock: 1024 bytes into partition
    uint8_t sector_buf[1024];
    uint64_t sb_lba = lba_offset + (EXT4_SUPERBLOCK_OFFSET / dev->sector_size);
    if (dev->read_sectors(dev, sb_lba, 2, sector_buf) != 0) {
        printk("ext4: Failed to read superblock from disk!\n");
        kfree(fs);
        return NULL;
    }

    memcpy(&fs->sb, sector_buf + (EXT4_SUPERBLOCK_OFFSET % dev->sector_size), sizeof(ext4_super_block_t));

    if (fs->sb.s_magic != EXT4_MAGIC) {
        printk("ext4: Invalid magic 0x%04x (expected 0x%04x)\n", fs->sb.s_magic, EXT4_MAGIC);
        kfree(fs);
        return NULL;
    }

    fs->block_size = 1024 << fs->sb.s_log_block_size;
    fs->groups_count = (fs->sb.s_blocks_count_lo + fs->sb.s_blocks_per_group - 1) / fs->sb.s_blocks_per_group;

    // Read block group descriptors
    uint64_t bgd_block = (fs->block_size == 1024) ? 2 : 1;
    size_t bgds_size = fs->groups_count * sizeof(ext4_group_desc_t);
    size_t bgds_blocks = (bgds_size + fs->block_size - 1) / fs->block_size;

    fs->bgds = kmalloc(bgds_blocks * fs->block_size);
    if (!fs->bgds) {
        kfree(fs);
        return NULL;
    }

    for (size_t b = 0; b < bgds_blocks; b++) {
        if (ext4_read_block(fs, bgd_block + b, (uint8_t *)fs->bgds + (b * fs->block_size)) != 0) {
            printk("ext4: Failed to read block group descriptors!\n");
            kfree(fs->bgds);
            kfree(fs);
            return NULL;
        }
    }

    printk("ext4: Filesystem mounted successfully! Block size: %u bytes, Block groups: %u\n",
           fs->block_size, fs->groups_count);

    // Mount root inode (inode 2)
    return ext4_create_vfs_node(fs, EXT4_ROOT_INO, "/");
}

block_device_t *ext4_get_device(vfs_node_t *node) {
    // Only ext4 directories carry ext4 data in node->ptr (a FAT32 node holds its own)
    if (!node || !node->ptr || node->readdir != ext4_vfs_readdir) return NULL;
    ext4_node_data_t *data = (ext4_node_data_t *)node->ptr;
    if (!data->fs) return NULL;
    return data->fs->dev;
}
