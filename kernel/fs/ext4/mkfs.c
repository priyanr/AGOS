#include <fs/mkfs.h>
#include <fs/ext4.h>
#include <mm/heap.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define EXT4_BLOCK_SIZE 4096
#define SECTORS_PER_BLOCK (EXT4_BLOCK_SIZE / 512)
#define DEFAULT_INODES_COUNT 1024
#define INODE_TABLE_BLOCKS (DEFAULT_INODES_COUNT * sizeof(ext4_inode_t) / EXT4_BLOCK_SIZE)

// Layout for Group 0:
// Block 0: Superblock (at offset 1024)
// Block 1: Group Descriptor Table
// Block 2: Block Bitmap
// Block 3: Inode Bitmap
// Blocks 4 .. 4 + INODE_TABLE_BLOCKS - 1: Inode Table (Blocks 4..35)
// Block 36: Root Directory (Inode 2)
// Block 37: lost+found Directory (Inode 11)
// First allocatable data block = 38
#define ROOT_DIR_BLOCK     (4 + INODE_TABLE_BLOCKS)
#define LOST_FOUND_BLOCK   (ROOT_DIR_BLOCK + 1)
#define METADATA_BLOCKS    (LOST_FOUND_BLOCK + 1) // 38 blocks

// Dedicated static I/O buffers to eliminate stack usage during mkfs operations
static uint8_t mkfs_meta_buf[EXT4_BLOCK_SIZE];
static uint8_t mkfs_inode_buf[EXT4_BLOCK_SIZE];
static uint8_t mkfs_dir_buf[EXT4_BLOCK_SIZE];
static uint8_t mkfs_block_buf[EXT4_BLOCK_SIZE];

static int write_fs_block(block_device_t *dev, uint64_t block_num, const void *buf) {
    uint64_t lba = block_num * SECTORS_PER_BLOCK;
    return dev->write_sectors(dev, lba, SECTORS_PER_BLOCK, buf);
}

static int read_fs_block(block_device_t *dev, uint64_t block_num, void *buf) {
    uint64_t lba = block_num * SECTORS_PER_BLOCK;
    return dev->read_sectors(dev, lba, SECTORS_PER_BLOCK, buf);
}

int mkfs_ext4_format(block_device_t *dev) {
    if (!dev || !dev->write_sectors || !dev->read_sectors) {
        printk("mkfs.ext4: Invalid block device.\n");
        return -1;
    }

    uint64_t total_blocks = (dev->sector_count * dev->sector_size) / EXT4_BLOCK_SIZE;
    if (total_blocks < 256) {
        printk("mkfs.ext4: Partition too small (%llu blocks, minimum 256).\n", total_blocks);
        return -1;
    }

    printk("mkfs.ext4: Formatting %s (%llu MB, %llu blocks of 4096 bytes)...\n",
           dev->name, (dev->sector_count * dev->sector_size) / (1024 * 1024), total_blocks);

    uint8_t *block_buf = kzalloc(EXT4_BLOCK_SIZE);
    if (!block_buf) return -1;

    // 1. Superblock (Block 0, offset 1024)
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    ext4_super_block_t *sb = (ext4_super_block_t *)(block_buf + EXT4_SUPERBLOCK_OFFSET);
    sb->s_inodes_count = DEFAULT_INODES_COUNT;
    sb->s_blocks_count_lo = (uint32_t)total_blocks;
    sb->s_r_blocks_count_lo = (uint32_t)(total_blocks / 20);
    sb->s_free_blocks_count_lo = (uint32_t)(total_blocks - METADATA_BLOCKS);
    sb->s_free_inodes_count = DEFAULT_INODES_COUNT - 11;
    sb->s_first_data_block = 0; // 0 for 4KB block size
    sb->s_log_block_size = 2;   // 1024 << 2 = 4096
    sb->s_log_cluster_size = 2;
    sb->s_blocks_per_group = (8 * EXT4_BLOCK_SIZE); // 32768 blocks per group
    sb->s_clusters_per_group = (8 * EXT4_BLOCK_SIZE);
    sb->s_inodes_per_group = DEFAULT_INODES_COUNT;
    sb->s_magic = EXT4_MAGIC;
    sb->s_state = 1;      // Clean
    sb->s_errors = 1;     // Continue
    sb->s_creator_os = 0; // Linux
    sb->s_rev_level = 1;  // Dynamic
    sb->s_first_ino = 11;
    sb->s_inode_size = sizeof(ext4_inode_t); // 128 bytes
    sb->s_feature_compat = 0;
    sb->s_feature_incompat = 0x0042; // EXTENTS | FILETYPE
    sb->s_feature_ro_compat = 0x0001; // SPARSE_SUPER
    strncpy(sb->s_volume_name, "ANTIGRAVITY", sizeof(sb->s_volume_name) - 1);

    write_fs_block(dev, 0, block_buf);

    // 2. Block Group Descriptor Table (Block 1)
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    ext4_group_desc_t *bgd = (ext4_group_desc_t *)block_buf;
    bgd->bg_block_bitmap_lo = 2;
    bgd->bg_inode_bitmap_lo = 3;
    bgd->bg_inode_table_lo = 4;
    bgd->bg_free_blocks_count_lo = (uint16_t)(total_blocks - METADATA_BLOCKS);
    bgd->bg_free_inodes_count_lo = (uint16_t)(DEFAULT_INODES_COUNT - 11);
    bgd->bg_used_dirs_count_lo = 2; // root dir and lost+found

    write_fs_block(dev, 1, block_buf);

    // 3. Block Bitmap (Block 2)
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    // Mark blocks 0 .. METADATA_BLOCKS - 1 as used
    for (size_t b = 0; b < METADATA_BLOCKS; b++) {
        block_buf[b / 8] |= (1 << (b % 8));
    }
    write_fs_block(dev, 2, block_buf);

    // 4. Inode Bitmap (Block 3)
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    // Mark inodes 1 .. 11 as used (bits 0 .. 10)
    for (int i = 0; i < 11; i++) {
        block_buf[i / 8] |= (1 << (i % 8));
    }
    write_fs_block(dev, 3, block_buf);

    // 5. Inode Table (Blocks 4 .. 4 + INODE_TABLE_BLOCKS - 1)
    // Clear all inode table blocks first
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    for (size_t t = 0; t < INODE_TABLE_BLOCKS; t++) {
        write_fs_block(dev, 4 + t, block_buf);
    }

    // Set up Inode 2 (Root directory '/')
    // Inode 2 is in Block 4, index 1 (since inode 1 is index 0)
    read_fs_block(dev, 4, block_buf);
    ext4_inode_t *root_inode = (ext4_inode_t *)(block_buf + sizeof(ext4_inode_t)); // index 1 = inode 2
    root_inode->i_mode = 0x41ED; // Directory, 0755
    root_inode->i_uid = 0;
    root_inode->i_gid = 0;
    root_inode->i_size_lo = EXT4_BLOCK_SIZE;
    root_inode->i_links_count = 3;
    root_inode->i_blocks_lo = SECTORS_PER_BLOCK;
    root_inode->i_flags = 0x00080000; // EXT4_EXTENTS_FL

    ext4_extent_header_t *reh = (ext4_extent_header_t *)root_inode->i_block;
    reh->eh_magic = EXT4_EXTENTS_MAGIC;
    reh->eh_entries = 1;
    reh->eh_max = 4;
    reh->eh_depth = 0;

    ext4_extent_t *rext = (ext4_extent_t *)(reh + 1);
    rext->ee_block = 0;
    rext->ee_len = 1;
    rext->ee_start_hi = 0;
    rext->ee_start_lo = ROOT_DIR_BLOCK;

    // Set up Inode 11 ('lost+found')
    // Inode 11 is in Block 4, index 10
    ext4_inode_t *lf_inode = (ext4_inode_t *)(block_buf + 10 * sizeof(ext4_inode_t));
    lf_inode->i_mode = 0x41C0; // Directory, 0700
    lf_inode->i_size_lo = EXT4_BLOCK_SIZE;
    lf_inode->i_links_count = 2;
    lf_inode->i_blocks_lo = SECTORS_PER_BLOCK;
    lf_inode->i_flags = 0x00080000;

    ext4_extent_header_t *lfeh = (ext4_extent_header_t *)lf_inode->i_block;
    lfeh->eh_magic = EXT4_EXTENTS_MAGIC;
    lfeh->eh_entries = 1;
    lfeh->eh_max = 4;
    lfeh->eh_depth = 0;

    ext4_extent_t *lfext = (ext4_extent_t *)(lfeh + 1);
    lfext->ee_block = 0;
    lfext->ee_len = 1;
    lfext->ee_start_hi = 0;
    lfext->ee_start_lo = LOST_FOUND_BLOCK;

    write_fs_block(dev, 4, block_buf);

    // 6. Root Directory Data Block
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    uint8_t *p = block_buf;

    // '.' entry
    ext4_dir_entry_2_t *d1 = (ext4_dir_entry_2_t *)p;
    d1->inode = EXT4_ROOT_INO;
    d1->name_len = 1;
    d1->file_type = 2; // Directory
    d1->name[0] = '.';
    d1->rec_len = 12;
    p += d1->rec_len;

    // '..' entry
    ext4_dir_entry_2_t *d2 = (ext4_dir_entry_2_t *)p;
    d2->inode = EXT4_ROOT_INO;
    d2->name_len = 2;
    d2->file_type = 2;
    d2->name[0] = '.';
    d2->name[1] = '.';
    d2->rec_len = 12;
    p += d2->rec_len;

    // 'lost+found' entry
    ext4_dir_entry_2_t *d3 = (ext4_dir_entry_2_t *)p;
    d3->inode = 11;
    d3->name_len = 10;
    d3->file_type = 2;
    strcpy(d3->name, "lost+found");
    d3->rec_len = (uint16_t)(EXT4_BLOCK_SIZE - 24); // Spans remaining block

    write_fs_block(dev, ROOT_DIR_BLOCK, block_buf);

    // 7. lost+found Data Block
    memset(block_buf, 0, EXT4_BLOCK_SIZE);
    p = block_buf;

    d1 = (ext4_dir_entry_2_t *)p;
    d1->inode = 11;
    d1->name_len = 1;
    d1->file_type = 2;
    d1->name[0] = '.';
    d1->rec_len = 12;
    p += d1->rec_len;

    d2 = (ext4_dir_entry_2_t *)p;
    d2->inode = EXT4_ROOT_INO;
    d2->name_len = 2;
    d2->file_type = 2;
    d2->name[0] = '.';
    d2->name[1] = '.';
    d2->rec_len = (uint16_t)(EXT4_BLOCK_SIZE - 12);

    write_fs_block(dev, LOST_FOUND_BLOCK, block_buf);

    kfree(block_buf);
    printk("mkfs.ext4: Format complete! Filesystem initialized with %llu free blocks.\n",
           total_blocks - METADATA_BLOCKS);
    return 0;
}

static int alloc_free_inode(block_device_t *dev, uint32_t *out_ino) {
    if (read_fs_block(dev, 3, mkfs_meta_buf) != 0) return -1;

    for (uint32_t i = 11; i < DEFAULT_INODES_COUNT; i++) {
        if (!(mkfs_meta_buf[i / 8] & (1 << (i % 8)))) {
            mkfs_meta_buf[i / 8] |= (1 << (i % 8));
            write_fs_block(dev, 3, mkfs_meta_buf);
            *out_ino = i + 1; // 1-based inode

            // Update free inodes in superblock
            read_fs_block(dev, 0, mkfs_meta_buf);
            ext4_super_block_t *sb = (ext4_super_block_t *)(mkfs_meta_buf + EXT4_SUPERBLOCK_OFFSET);
            if (sb->s_free_inodes_count > 0) sb->s_free_inodes_count--;
            write_fs_block(dev, 0, mkfs_meta_buf);

            // Update free inodes in BGD
            read_fs_block(dev, 1, mkfs_meta_buf);
            ext4_group_desc_t *bgd = (ext4_group_desc_t *)mkfs_meta_buf;
            if (bgd->bg_free_inodes_count_lo > 0) bgd->bg_free_inodes_count_lo--;
            write_fs_block(dev, 1, mkfs_meta_buf);

            return 0;
        }
    }
    return -1;
}

static int alloc_free_blocks(block_device_t *dev, uint32_t count, uint32_t *out_start_block) {
    // Read superblock to get total blocks
    read_fs_block(dev, 0, mkfs_meta_buf);
    ext4_super_block_t *sb = (ext4_super_block_t *)(mkfs_meta_buf + EXT4_SUPERBLOCK_OFFSET);
    uint32_t total = sb->s_blocks_count_lo;

    // Read block bitmap
    if (read_fs_block(dev, 2, mkfs_meta_buf) != 0) return -1;

    uint32_t run = 0;
    uint32_t run_start = 0;

    for (uint32_t b = METADATA_BLOCKS; b < total; b++) {
        if (!(mkfs_meta_buf[b / 8] & (1 << (b % 8)))) {
            if (run == 0) run_start = b;
            run++;
            if (run == count) {
                // Found contiguous range! Mark as used
                for (uint32_t m = run_start; m < run_start + count; m++) {
                    mkfs_meta_buf[m / 8] |= (1 << (m % 8));
                }
                write_fs_block(dev, 2, mkfs_meta_buf);

                // Update free blocks counters in superblock
                read_fs_block(dev, 0, mkfs_meta_buf);
                sb = (ext4_super_block_t *)(mkfs_meta_buf + EXT4_SUPERBLOCK_OFFSET);
                if (sb->s_free_blocks_count_lo >= count) sb->s_free_blocks_count_lo -= count;
                write_fs_block(dev, 0, mkfs_meta_buf);

                // Update free blocks counters in BGD
                read_fs_block(dev, 1, mkfs_meta_buf);
                ext4_group_desc_t *bgd = (ext4_group_desc_t *)mkfs_meta_buf;
                if (bgd->bg_free_blocks_count_lo >= count) bgd->bg_free_blocks_count_lo -= count;
                write_fs_block(dev, 1, mkfs_meta_buf);

                *out_start_block = run_start;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

static int read_inode_data(block_device_t *dev, uint32_t ino, ext4_inode_t *inode_out) {
    uint32_t index = ino - 1;
    uint32_t block_offset = (index * sizeof(ext4_inode_t)) / EXT4_BLOCK_SIZE;
    uint32_t in_block = (index * sizeof(ext4_inode_t)) % EXT4_BLOCK_SIZE;

    if (read_fs_block(dev, 4 + block_offset, mkfs_inode_buf) != 0) return -1;
    memcpy(inode_out, mkfs_inode_buf + in_block, sizeof(ext4_inode_t));
    return 0;
}

static int write_inode_data(block_device_t *dev, uint32_t ino, const ext4_inode_t *inode_in) {
    uint32_t index = ino - 1;
    uint32_t block_offset = (index * sizeof(ext4_inode_t)) / EXT4_BLOCK_SIZE;
    uint32_t in_block = (index * sizeof(ext4_inode_t)) % EXT4_BLOCK_SIZE;

    if (read_fs_block(dev, 4 + block_offset, mkfs_inode_buf) != 0) return -1;
    memcpy(mkfs_inode_buf + in_block, inode_in, sizeof(ext4_inode_t));
    return write_fs_block(dev, 4 + block_offset, mkfs_inode_buf);
}

static int append_dir_entry(block_device_t *dev, uint32_t parent_ino, uint32_t child_ino, const char *name, uint8_t file_type) {
    ext4_inode_t parent_inode;
    if (read_inode_data(dev, parent_ino, &parent_inode) != 0) return -1;

    ext4_extent_header_t *eh = (ext4_extent_header_t *)parent_inode.i_block;
    if (eh->eh_magic != EXT4_EXTENTS_MAGIC || eh->eh_entries == 0) return -1;
    ext4_extent_t *ext = (ext4_extent_t *)(eh + 1);
    uint32_t dir_block = ext[0].ee_start_lo;

    if (read_fs_block(dev, dir_block, mkfs_dir_buf) != 0) return -1;

    size_t name_len = strlen(name);
    size_t new_entry_size = (sizeof(ext4_dir_entry_2_t) + name_len + 3) & ~3;

    // 1. Check if an entry with this name already exists in the directory
    uint32_t scan_off = 0;
    while (scan_off < EXT4_BLOCK_SIZE) {
        ext4_dir_entry_2_t *e = (ext4_dir_entry_2_t *)(mkfs_dir_buf + scan_off);
        if (e->rec_len == 0) break;
        if (e->inode != 0 && e->name_len == name_len && memcmp(e->name, name, name_len) == 0) {
            // Overwrite existing directory entry with new inode
            e->inode = child_ino;
            e->file_type = file_type;
            return write_fs_block(dev, dir_block, mkfs_dir_buf);
        }
        scan_off += e->rec_len;
    }

    // 2. Traverse to the last directory entry in the block to append
    uint32_t offset = 0;
    ext4_dir_entry_2_t *entry = (ext4_dir_entry_2_t *)mkfs_dir_buf;

    while (offset < EXT4_BLOCK_SIZE) {
        entry = (ext4_dir_entry_2_t *)(mkfs_dir_buf + offset);
        if (entry->rec_len == 0) break;

        size_t actual_entry_size = (sizeof(ext4_dir_entry_2_t) + entry->name_len + 3) & ~3;
        size_t available = entry->rec_len - actual_entry_size;

        if (available >= new_entry_size) {
            // Trim last entry to its actual size
            uint16_t old_rec = entry->rec_len;
            entry->rec_len = (uint16_t)actual_entry_size;

            // Append new entry in the remaining space
            ext4_dir_entry_2_t *new_e = (ext4_dir_entry_2_t *)(mkfs_dir_buf + offset + actual_entry_size);
            new_e->inode = child_ino;
            new_e->rec_len = (uint16_t)(old_rec - actual_entry_size);
            new_e->name_len = (uint8_t)name_len;
            new_e->file_type = file_type;
            memcpy(new_e->name, name, name_len);

            return write_fs_block(dev, dir_block, mkfs_dir_buf);
        }

        offset += entry->rec_len;
    }

    return -1;
}

int mkfs_ext4_add_dir(block_device_t *dev, uint32_t parent_ino, const char *name, uint32_t *new_ino_out) {
    uint32_t new_ino = 0;
    if (alloc_free_inode(dev, &new_ino) != 0) return -1;

    uint32_t dir_block = 0;
    if (alloc_free_blocks(dev, 1, &dir_block) != 0) return -1;

    // Write directory data block
    memset(mkfs_block_buf, 0, EXT4_BLOCK_SIZE);
    uint8_t *p = mkfs_block_buf;

    // '.'
    ext4_dir_entry_2_t *d1 = (ext4_dir_entry_2_t *)p;
    d1->inode = new_ino;
    d1->name_len = 1;
    d1->file_type = 2; // Dir
    d1->name[0] = '.';
    d1->rec_len = 12;
    p += d1->rec_len;

    // '..'
    ext4_dir_entry_2_t *d2 = (ext4_dir_entry_2_t *)p;
    d2->inode = parent_ino;
    d2->name_len = 2;
    d2->file_type = 2;
    d2->name[0] = '.';
    d2->name[1] = '.';
    d2->rec_len = (uint16_t)(EXT4_BLOCK_SIZE - 12);

    write_fs_block(dev, dir_block, mkfs_block_buf);

    // Write new inode
    ext4_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = 0x41ED; // Dir 0755
    ino.i_size_lo = EXT4_BLOCK_SIZE;
    ino.i_links_count = 2;
    ino.i_blocks_lo = SECTORS_PER_BLOCK;
    ino.i_flags = 0x00080000;

    ext4_extent_header_t *eh = (ext4_extent_header_t *)ino.i_block;
    eh->eh_magic = EXT4_EXTENTS_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    ext4_extent_t *ext = (ext4_extent_t *)(eh + 1);
    ext->ee_block = 0;
    ext->ee_len = 1;
    ext->ee_start_hi = 0;
    ext->ee_start_lo = dir_block;

    write_inode_data(dev, new_ino, &ino);

    // Link into parent directory
    append_dir_entry(dev, parent_ino, new_ino, name, 2);

    if (new_ino_out) *new_ino_out = new_ino;
    return 0;
}

int mkfs_ext4_add_file(block_device_t *dev, uint32_t parent_ino, const char *name, const void *data, size_t size) {
    uint32_t new_ino = 0;
    if (alloc_free_inode(dev, &new_ino) != 0) return -1;

    uint32_t n_blocks = (uint32_t)((size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE);
    if (n_blocks == 0) n_blocks = 1;

    uint32_t start_block = 0;
    if (alloc_free_blocks(dev, n_blocks, &start_block) != 0) return -1;

    // Write file data
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = size;

    for (uint32_t b = 0; b < n_blocks; b++) {
        memset(mkfs_block_buf, 0, EXT4_BLOCK_SIZE);
        size_t chunk = (remaining > EXT4_BLOCK_SIZE) ? EXT4_BLOCK_SIZE : remaining;
        if (chunk > 0 && src) {
            memcpy(mkfs_block_buf, src + (b * EXT4_BLOCK_SIZE), chunk);
            remaining -= chunk;
        }
        write_fs_block(dev, start_block + b, mkfs_block_buf);
    }

    // Write file inode
    ext4_inode_t ino;
    memset(&ino, 0, sizeof(ino));
    ino.i_mode = 0x81ED; // File 0755
    ino.i_size_lo = (uint32_t)size;
    ino.i_links_count = 1;
    ino.i_blocks_lo = n_blocks * SECTORS_PER_BLOCK;
    ino.i_flags = 0x00080000;

    ext4_extent_header_t *eh = (ext4_extent_header_t *)ino.i_block;
    eh->eh_magic = EXT4_EXTENTS_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    ext4_extent_t *ext = (ext4_extent_t *)(eh + 1);
    ext->ee_block = 0;
    ext->ee_len = (uint16_t)n_blocks;
    ext->ee_start_hi = 0;
    ext->ee_start_lo = start_block;

    write_inode_data(dev, new_ino, &ino);

    // Link into parent directory
    append_dir_entry(dev, parent_ino, new_ino, name, 1);
    return 0;
}

void cmd_mkfs_ext4(int argc, char **argv) {
    if (argc < 2) {
        printk("Usage: mkfs.ext4 <device>\nExample: mkfs.ext4 hda1\n");
        return;
    }

    const char *dev_name = argv[1];
    block_device_t *dev = block_get_device(dev_name);
    if (!dev) {
        printk("mkfs.ext4: Block device '%s' not found.\n", dev_name);
        return;
    }

    if (mkfs_ext4_format(dev) == 0) {
        printk("mkfs.ext4: Validating filesystem mount...\n");
        vfs_node_t *test_root = ext4_mount(dev, 0);
        if (test_root) {
            printk("[OK] ext4 filesystem on '%s' verified and mounted successfully!\n", dev_name);
        } else {
            printk("[WARN] ext4 format succeeded but verification mount returned error.\n");
        }
    } else {
        printk("mkfs.ext4: Failed to format '%s'!\n", dev_name);
    }
}
