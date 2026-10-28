#include <fs/fat32.h>
#include <fs/vfs.h>
#include <mm/heap.h>
#include <kernel/string.h>
#include <kernel/printk.h>

// Read-only FAT32 driver: mounts the Pi SD card boot partition (or any FAT32 volume) as a VFS
// tree. Long file names are supported; sectors must be 512 bytes.

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_LFN       0x0F
#define FAT_END_OF_CHAIN   0x0FFFFFF8

typedef struct {
    block_device_t *dev;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_start;     // First sector of FAT #1
    uint32_t data_start;    // First sector of cluster 2
    uint32_t root_cluster;
    uint32_t cluster_count;
    uint8_t *cluster_buf;
    uint8_t fat_sector[512];
    uint32_t fat_sector_lba; // Sector cached in fat_sector, 0 = none
} fat32_fs_t;

typedef struct {
    fat32_fs_t *fs;
    uint32_t first_cluster;
} fat32_node_data_t;

typedef struct {
    char name[128];
    uint8_t attr;
    uint32_t first_cluster;
    uint32_t size;
} fat32_entry_t;

// Byte-wise little-endian reads: on-disk fields are unaligned (AArch64 runs -mstrict-align)
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool names_equal_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (lower_ascii(*a) != lower_ascii(*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool read_cluster(fat32_fs_t *fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->cluster_count + 2) return false;
    uint64_t lba = fs->data_start + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
    return fs->dev->read_sectors(fs->dev, lba, fs->sectors_per_cluster, fs->cluster_buf) == 0;
}

// Next cluster in the chain, or 0 at the end of the chain / on a read error
static uint32_t next_cluster(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t lba = fs->fat_start + (cluster * 4) / 512;
    if (fs->fat_sector_lba != lba) {
        if (fs->dev->read_sectors(fs->dev, lba, 1, fs->fat_sector) != 0) return 0;
        fs->fat_sector_lba = lba;
    }
    uint32_t next = rd32(&fs->fat_sector[(cluster * 4) % 512]) & 0x0FFFFFFF;
    if (next < 2 || next >= FAT_END_OF_CHAIN) return 0;
    return next;
}

// 8.3 name of a short directory entry, honouring the NT lower-case flags
static void short_name(const uint8_t *e, char *out) {
    size_t n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        out[n++] = (e[12] & 0x08) ? lower_ascii((char)e[i]) : (char)e[i];
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            out[n++] = (e[12] & 0x10) ? lower_ascii((char)e[i]) : (char)e[i];
        }
    }
    out[n] = '\0';
}

// Walk a directory: return the entry number want_index (counting only real files and
// directories), or the entry called want_name when it is not NULL
static bool find_entry(fat32_fs_t *fs, uint32_t dir_cluster, uint32_t want_index,
                       const char *want_name, fat32_entry_t *out) {
    char lfn[128];
    bool have_lfn = false;
    uint32_t index = 0;

    for (uint32_t cluster = dir_cluster; cluster != 0; cluster = next_cluster(fs, cluster)) {
        if (!read_cluster(fs, cluster)) return false;
        for (uint32_t off = 0; off < fs->cluster_bytes; off += 32) {
            const uint8_t *e = &fs->cluster_buf[off];
            if (e[0] == 0x00) return false; // End of directory
            if (e[0] == 0xE5) {             // Deleted
                have_lfn = false;
                continue;
            }

            if (e[11] == FAT_ATTR_LFN) {
                // Long-name pieces come last-first; each holds 13 UTF-16 characters
                uint32_t order = e[0] & 0x1F;
                if (e[0] & 0x40) {
                    memset(lfn, 0, sizeof(lfn));
                    have_lfn = true;
                }
                if (order == 0 || order > 9) continue;
                static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int i = 0; i < 13; i++) {
                    uint16_t ch = rd16(&e[pos[i]]);
                    uint32_t at = (order - 1) * 13 + i;
                    if (ch == 0x0000 || ch == 0xFFFF || at >= sizeof(lfn) - 1) break;
                    lfn[at] = (ch < 128) ? (char)ch : '?';
                }
                continue;
            }

            if (e[11] & FAT_ATTR_VOLUME_ID) {
                have_lfn = false;
                continue;
            }

            fat32_entry_t entry;
            if (have_lfn && lfn[0]) {
                strncpy(entry.name, lfn, sizeof(entry.name) - 1);
                entry.name[sizeof(entry.name) - 1] = '\0';
            } else {
                short_name(e, entry.name);
            }
            have_lfn = false;
            entry.attr = e[11];
            entry.first_cluster = ((uint32_t)rd16(&e[20]) << 16) | rd16(&e[26]);
            entry.size = rd32(&e[28]);

            bool match = want_name ? names_equal_nocase(entry.name, want_name) : (index == want_index);
            if (match) {
                *out = entry;
                return true;
            }
            index++;
        }
    }
    return false;
}

static ssize_t fat32_vfs_read(vfs_node_t *node, uint64_t offset, size_t size, void *buffer) {
    fat32_node_data_t *data = (fat32_node_data_t *)node->ptr;
    fat32_fs_t *fs = data->fs;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = (size_t)(node->size - offset);

    uint32_t cluster = data->first_cluster;
    for (uint64_t skip = offset / fs->cluster_bytes; skip > 0 && cluster; skip--) {
        cluster = next_cluster(fs, cluster);
    }

    size_t done = 0;
    uint32_t in_cluster = (uint32_t)(offset % fs->cluster_bytes);
    while (done < size && cluster) {
        if (!read_cluster(fs, cluster)) return done > 0 ? (ssize_t)done : -1;
        size_t chunk = fs->cluster_bytes - in_cluster;
        if (chunk > size - done) chunk = size - done;
        memcpy((uint8_t *)buffer + done, fs->cluster_buf + in_cluster, chunk);
        done += chunk;
        in_cluster = 0;
        cluster = next_cluster(fs, cluster);
    }
    return (ssize_t)done;
}

static dirent_t shared_dirent;

static dirent_t *fat32_vfs_readdir(vfs_node_t *node, uint32_t index) {
    fat32_node_data_t *data = (fat32_node_data_t *)node->ptr;
    fat32_entry_t entry;
    if (!find_entry(data->fs, data->first_cluster, index, NULL, &entry)) return NULL;

    strncpy(shared_dirent.name, entry.name, sizeof(shared_dirent.name) - 1);
    shared_dirent.name[sizeof(shared_dirent.name) - 1] = '\0';
    shared_dirent.ino = entry.first_cluster;
    shared_dirent.type = (entry.attr & FAT_ATTR_DIRECTORY) ? 2 : 1;
    return &shared_dirent;
}

static vfs_node_t *make_node(fat32_fs_t *fs, const char *name, uint32_t first_cluster,
                             uint32_t size, bool is_dir);

static vfs_node_t *fat32_vfs_finddir(vfs_node_t *node, const char *name) {
    fat32_node_data_t *data = (fat32_node_data_t *)node->ptr;
    fat32_entry_t entry;
    if (!find_entry(data->fs, data->first_cluster, 0, name, &entry)) return NULL;

    bool is_dir = (entry.attr & FAT_ATTR_DIRECTORY) != 0;
    uint32_t cluster = entry.first_cluster;
    if (is_dir && cluster == 0) cluster = data->fs->root_cluster; // ".." of a top-level dir
    return make_node(data->fs, entry.name, cluster, entry.size, is_dir);
}

static vfs_node_t *make_node(fat32_fs_t *fs, const char *name, uint32_t first_cluster,
                             uint32_t size, bool is_dir) {
    vfs_node_t *node = kzalloc(sizeof(vfs_node_t));
    fat32_node_data_t *data = kzalloc(sizeof(fat32_node_data_t));
    if (!node || !data) {
        if (node) kfree(node);
        if (data) kfree(data);
        return NULL;
    }
    data->fs = fs;
    data->first_cluster = first_cluster;

    strncpy(node->name, name, sizeof(node->name) - 1);
    node->flags = is_dir ? FS_DIRECTORY : FS_FILE;
    node->size = size;
    node->inode = first_cluster;
    node->read = is_dir ? NULL : fat32_vfs_read;
    node->readdir = is_dir ? fat32_vfs_readdir : NULL;
    node->finddir = is_dir ? fat32_vfs_finddir : NULL;
    node->ptr = (vfs_node_t *)data;
    return node;
}

vfs_node_t *fat32_mount(block_device_t *dev) {
    if (!dev || dev->sector_size != 512) return NULL;

    uint8_t *bpb = kmalloc(512);
    if (!bpb) return NULL;
    if (dev->read_sectors(dev, 0, 1, bpb) != 0 || bpb[510] != 0x55 || bpb[511] != 0xAA) {
        kfree(bpb);
        return NULL;
    }

    uint32_t bytes_per_sector = rd16(&bpb[11]);
    uint32_t sectors_per_cluster = bpb[13];
    uint32_t reserved = rd16(&bpb[14]);
    uint32_t fat_count = bpb[16];
    uint32_t fat16_size = rd16(&bpb[22]);
    uint32_t total16 = rd16(&bpb[19]);
    uint32_t total32 = rd32(&bpb[32]);
    uint32_t fat_size = rd32(&bpb[36]);
    uint32_t root_cluster = rd32(&bpb[44]);
    kfree(bpb);

    // FAT32 has no 16-bit FAT size and no fixed root directory
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || fat16_size != 0 || fat_size == 0 ||
        fat_count == 0 || root_cluster < 2) {
        return NULL;
    }

    fat32_fs_t *fs = kzalloc(sizeof(fat32_fs_t));
    if (!fs) return NULL;
    fs->dev = dev;
    fs->sectors_per_cluster = sectors_per_cluster;
    fs->cluster_bytes = sectors_per_cluster * 512;
    fs->fat_start = reserved;
    fs->data_start = reserved + fat_count * fat_size;
    fs->root_cluster = root_cluster;
    uint32_t total = total16 ? total16 : total32;
    fs->cluster_count = (total > fs->data_start) ? (total - fs->data_start) / sectors_per_cluster : 0;
    fs->cluster_buf = kmalloc(fs->cluster_bytes);
    if (!fs->cluster_buf || fs->cluster_count == 0) {
        if (fs->cluster_buf) kfree(fs->cluster_buf);
        kfree(fs);
        return NULL;
    }

    printk("FAT32: Mounted %s (%u-byte clusters, %u clusters, read-only)\n",
           dev->name, fs->cluster_bytes, fs->cluster_count);
    return make_node(fs, "/", root_cluster, 0, true);
}
