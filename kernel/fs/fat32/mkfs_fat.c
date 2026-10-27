#include <fs/fat32.h>
#include <kernel/string.h>
#include <mm/heap.h>
#include <kernel/printk.h>

#define FAT32_ATTR_READONLY  0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       (FAT32_ATTR_READONLY | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
    uint8_t  boot_code[420];
    uint16_t signature;
} fat32_boot_sector_t;

typedef struct __attribute__((packed)) {
    uint32_t lead_sig;       // 0x41615252
    uint8_t  reserved1[480];
    uint32_t struct_sig;     // 0x61417272
    uint32_t free_count;
    uint32_t next_free;
    uint8_t  reserved2[12];
    uint32_t trail_sig;      // 0xAA550000
} fat32_fsinfo_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_high;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_low;
    uint32_t file_size;
} fat32_dir_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  seq;
    uint16_t name1[5];
    uint8_t  attr; // 0x0F
    uint8_t  type; // 0x00
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t first_cluster; // 0
    uint16_t name3[2];
} fat32_lfn_entry_t;

static uint8_t fat_lfn_checksum(const uint8_t *short_name) {
    uint8_t chk = 0;
    for (int i = 0; i < 11; i++) {
        chk = (uint8_t)((((chk & 1) << 7) | ((chk & 0xFE) >> 1)) + short_name[i]);
    }
    return chk;
}

static size_t append_lfn_entries(uint8_t *buf, size_t offset, const char *name, const uint8_t *short_name) {
    uint8_t chk = fat_lfn_checksum(short_name);
    size_t name_len = strlen(name);

    // Number of 13-character chunks needed (including terminating null)
    size_t num_chunks = (name_len + 1 + 12) / 13;

    for (size_t chunk_idx = num_chunks; chunk_idx > 0; chunk_idx--) {
        size_t idx = chunk_idx - 1;
        fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)(buf + offset);
        memset(lfn, 0, sizeof(fat32_lfn_entry_t));

        uint8_t seq = (uint8_t)(idx + 1);
        if (idx == num_chunks - 1) {
            seq |= 0x40; // LAST_LONG_ENTRY
        }
        lfn->seq = seq;
        lfn->attr = FAT32_ATTR_LFN;
        lfn->checksum = chk;

        for (int j = 0; j < 13; j++) {
            size_t char_pos = idx * 13 + j;
            uint16_t c;
            if (char_pos < name_len) {
                c = (uint16_t)(uint8_t)name[char_pos];
            } else if (char_pos == name_len) {
                c = 0x0000;
            } else {
                c = 0xFFFF;
            }

            if (j < 5) {
                lfn->name1[j] = c;
            } else if (j < 11) {
                lfn->name2[j - 5] = c;
            } else {
                lfn->name3[j - 11] = c;
            }
        }
        offset += sizeof(fat32_lfn_entry_t);
    }
    return offset;
}

static size_t append_dir_entry(uint8_t *buf, size_t offset, const uint8_t *short_name, uint8_t attr, uint32_t cluster, uint32_t size) {
    fat32_dir_entry_t *ent = (fat32_dir_entry_t *)(buf + offset);
    memset(ent, 0, sizeof(fat32_dir_entry_t));
    memcpy(ent->name, short_name, 11);
    ent->attr = attr;
    ent->cluster_high = (uint16_t)((cluster >> 16) & 0xFFFF);
    ent->cluster_low = (uint16_t)(cluster & 0xFFFF);
    ent->file_size = size;
    return offset + sizeof(fat32_dir_entry_t);
}

static void write_file_data(block_device_t *dev, uint32_t start_c, uint32_t data_start_sector, uint8_t sectors_per_cluster, const void *data, size_t sz) {
    uint32_t lba = data_start_sector + (start_c - 2) * sectors_per_cluster;
    uint32_t full_sectors = (uint32_t)(sz / 512);
    if (full_sectors > 0) {
        dev->write_sectors(dev, lba, full_sectors, data);
    }
    size_t rem = sz % 512;
    if (rem > 0) {
        uint8_t tail[512];
        memset(tail, 0, sizeof(tail));
        memcpy(tail, (const uint8_t *)data + full_sectors * 512, rem);
        dev->write_sectors(dev, lba + full_sectors, 1, tail);
    }
}

int mkfs_fat32_boot_format(block_device_t *dev, uint64_t start_lba,
                           const void *k_data, size_t k_size,
                           const void *sys_data, size_t sys_size,
                           const char *conf_data, size_t conf_size) {
    if (!dev || !dev->write_sectors) return -1;

    uint32_t total_sectors = (uint32_t)dev->sector_count;
    if (total_sectors < 65536) {
        printk("mkfs_fat32: Partition too small for FAT32 (%u sectors, min 65536)!\n", total_sectors);
        return -1;
    }

    uint16_t reserved_sectors = 32;
    uint8_t  num_fats = 2;
    uint8_t  sectors_per_cluster = 1;
    uint32_t sectors_per_fat = 1024;
    uint32_t data_start_sector = reserved_sectors + (num_fats * sectors_per_fat); // 2080

    // 1. Prepare Boot Sector (BPB)
    fat32_boot_sector_t bs;
    memset(&bs, 0, sizeof(bs));
    bs.jmp_boot[0] = 0xEB; bs.jmp_boot[1] = 0x58; bs.jmp_boot[2] = 0x90;
    memcpy(bs.oem_name, "MSWIN4.1", 8);
    bs.bytes_per_sector = 512;
    bs.sectors_per_cluster = sectors_per_cluster;
    bs.reserved_sectors = reserved_sectors;
    bs.num_fats = num_fats;
    bs.media_type = 0xF8; // Fixed hard disk
    bs.sectors_per_track = 32;
    bs.num_heads = 64;
    bs.hidden_sectors = (uint32_t)start_lba;
    bs.total_sectors_32 = total_sectors;
    bs.sectors_per_fat_32 = sectors_per_fat;
    bs.root_cluster = 2;
    bs.fs_info_sector = 1;
    bs.backup_boot_sector = 6;
    bs.drive_number = 0x80;
    bs.boot_signature = 0x29;
    bs.volume_id = 0x20261025;
    memcpy(bs.volume_label, "BOOT       ", 11);
    memcpy(bs.fs_type, "FAT32   ", 8);
    bs.signature = 0xAA55;

    // Write primary and backup boot sector
    dev->write_sectors(dev, 0, 1, &bs);
    dev->write_sectors(dev, 6, 1, &bs);

    // 2. Prepare FSInfo sector
    fat32_fsinfo_t fsi;
    memset(&fsi, 0, sizeof(fsi));
    fsi.lead_sig = 0x41615252; // "RRaA"
    fsi.struct_sig = 0x61417272; // "rrAa"
    fsi.free_count = total_sectors - data_start_sector - 2000;
    fsi.next_free = 3;
    fsi.trail_sig = 0xAA550000;

    dev->write_sectors(dev, 1, 1, &fsi);
    dev->write_sectors(dev, 7, 1, &fsi);

    // Zero out reserved sectors 2..5 and 8..31
    uint8_t zero_sec[512];
    memset(zero_sec, 0, sizeof(zero_sec));
    for (uint32_t s = 2; s < 6; s++) dev->write_sectors(dev, s, 1, zero_sec);
    for (uint32_t s = 8; s < reserved_sectors; s++) dev->write_sectors(dev, s, 1, zero_sec);

    // 3. Allocate Clusters & Build In-Memory FAT Table
    // Cluster 0: 0x0FFFFFF8, Cluster 1: 0x0FFFFFFF, Cluster 2 (root dir): 0x0FFFFFFF
    // We allocate a buffer for up to 4096 clusters (16 KB = 32 sectors)
    uint32_t fat_entries = 4096;
    uint32_t *fat_buf = (uint32_t *)kzalloc(fat_entries * sizeof(uint32_t));
    if (!fat_buf) return -1;

    fat_buf[0] = 0x0FFFFFF8;
    fat_buf[1] = 0x0FFFFFFF;
    fat_buf[2] = 0x0FFFFFFF; // Root directory is at cluster 2

    uint32_t cur_cluster = 3;

    #define ALLOC_CHAIN(size) ({ \
        uint32_t needed = ((uint32_t)(size) + 511) / 512; \
        if (needed == 0) needed = 1; \
        uint32_t start_c = cur_cluster; \
        for (uint32_t i = 0; i < needed - 1; i++) { \
            fat_buf[start_c + i] = start_c + i + 1; \
        } \
        fat_buf[start_c + needed - 1] = 0x0FFFFFFF; \
        cur_cluster += needed; \
        start_c; \
    })

    uint32_t sys_c   = ALLOC_CHAIN(sys_size);
    uint32_t conf_c  = ALLOC_CHAIN(conf_size);
    uint32_t k_c     = ALLOC_CHAIN(k_size);
    uint32_t sys2_c  = ALLOC_CHAIN(sys_size);
    uint32_t conf2_c = ALLOC_CHAIN(conf_size);
    uint32_t limine_dir_c = ALLOC_CHAIN(512);
    uint32_t boot_dir_c   = ALLOC_CHAIN(512);

    #undef ALLOC_CHAIN

    // 4. Write FAT1 and FAT2 tables
    // Active portion (32 sectors)
    dev->write_sectors(dev, reserved_sectors, fat_entries * sizeof(uint32_t) / 512, fat_buf);
    dev->write_sectors(dev, reserved_sectors + sectors_per_fat, fat_entries * sizeof(uint32_t) / 512, fat_buf);
    kfree(fat_buf);

    // Zero out the remaining sectors of FAT1 and FAT2
    uint32_t active_sectors = fat_entries * sizeof(uint32_t) / 512;
    for (uint32_t s = active_sectors; s < sectors_per_fat; s++) {
        dev->write_sectors(dev, reserved_sectors + s, 1, zero_sec);
        dev->write_sectors(dev, reserved_sectors + sectors_per_fat + s, 1, zero_sec);
    }

    // Helper macro to calculate LBA of a cluster
    #define CLUSTER_LBA(c) (data_start_sector + ((c) - 2) * sectors_per_cluster)

    // 5. Write File Data
    write_file_data(dev, sys_c, data_start_sector, sectors_per_cluster, sys_data, sys_size);
    write_file_data(dev, conf_c, data_start_sector, sectors_per_cluster, conf_data, conf_size);
    write_file_data(dev, k_c, data_start_sector, sectors_per_cluster, k_data, k_size);
    write_file_data(dev, sys2_c, data_start_sector, sectors_per_cluster, sys_data, sys_size);
    write_file_data(dev, conf2_c, data_start_sector, sectors_per_cluster, conf_data, conf_size);

    // 6. Build and Write Directory Clusters
    uint8_t dir_sector[512];

    // (A) /boot/limine directory
    memset(dir_sector, 0, sizeof(dir_sector));
    size_t off = 0;
    off = append_dir_entry(dir_sector, off, (const uint8_t *)".          ", FAT32_ATTR_DIRECTORY, limine_dir_c, 0);
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"..         ", FAT32_ATTR_DIRECTORY, boot_dir_c, 0);
    off = append_lfn_entries(dir_sector, off, "limine-bios.sys", (const uint8_t *)"LIMINE~1SYS");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"LIMINE~1SYS", FAT32_ATTR_ARCHIVE, sys2_c, (uint32_t)sys_size);
    off = append_lfn_entries(dir_sector, off, "limine.conf", (const uint8_t *)"LIMINE~1CON");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"LIMINE~1CON", FAT32_ATTR_ARCHIVE, conf2_c, (uint32_t)conf_size);
    dev->write_sectors(dev, CLUSTER_LBA(limine_dir_c), 1, dir_sector);

    // (B) /boot directory
    memset(dir_sector, 0, sizeof(dir_sector));
    off = 0;
    off = append_dir_entry(dir_sector, off, (const uint8_t *)".          ", FAT32_ATTR_DIRECTORY, boot_dir_c, 0);
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"..         ", FAT32_ATTR_DIRECTORY, 0, 0);
    off = append_lfn_entries(dir_sector, off, "kernel.elf", (const uint8_t *)"KERNEL  ELF");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"KERNEL  ELF", FAT32_ATTR_ARCHIVE, k_c, (uint32_t)k_size);
    off = append_lfn_entries(dir_sector, off, "limine", (const uint8_t *)"LIMINE     ");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"LIMINE     ", FAT32_ATTR_DIRECTORY, limine_dir_c, 0);
    dev->write_sectors(dev, CLUSTER_LBA(boot_dir_c), 1, dir_sector);

    // (C) Root directory (Cluster 2)
    memset(dir_sector, 0, sizeof(dir_sector));
    off = 0;
    off = append_lfn_entries(dir_sector, off, "boot", (const uint8_t *)"BOOT       ");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"BOOT       ", FAT32_ATTR_DIRECTORY, boot_dir_c, 0);
    off = append_lfn_entries(dir_sector, off, "limine-bios.sys", (const uint8_t *)"LIMINE~1SYS");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"LIMINE~1SYS", FAT32_ATTR_ARCHIVE, sys_c, (uint32_t)sys_size);
    off = append_lfn_entries(dir_sector, off, "limine.conf", (const uint8_t *)"LIMINE~1CON");
    off = append_dir_entry(dir_sector, off, (const uint8_t *)"LIMINE~1CON", FAT32_ATTR_ARCHIVE, conf_c, (uint32_t)conf_size);
    dev->write_sectors(dev, CLUSTER_LBA(2), 1, dir_sector);

    #undef CLUSTER_LBA

    return 0;
}
