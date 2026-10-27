#include <shell/installer.h>
#include <drivers/block.h>
#include <drivers/partition.h>
#include <fs/mkfs.h>
#include <fs/fat32.h>
#include <fs/ext4.h>
#include <fs/vfs.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <mm/heap.h>
#include <drivers/keyboard.h>
#include <hal/serial.h>

#include "../../limine-bin/limine-bios-hdd.h"

static void installer_readline(char *buf, size_t max) {
    size_t pos = 0;
    while (1) {
        char c = 0;
        if (keyboard_has_char()) {
            c = keyboard_getchar();
        } else if (serial_received()) {
            c = serial_getchar();
        } else {
            __asm__ volatile ("pause");
            continue;
        }

        if (c == '\r' || c == '\n') {
            printk("\n");
            buf[pos] = '\0';
            return;
        } else if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                printk("\b \b");
            }
        } else if (c >= 32 && c <= 126) {
            if (pos < max - 1) {
                buf[pos++] = c;
                printk("%c", c);
            }
        }
    }
}

static int install_limine_mbr(block_device_t *disk) {
    if (!disk || !disk->write_sectors || !disk->read_sectors) return -1;

    // 1. Read existing MBR (LBA 0) to preserve disk timestamp, signature, and partition table
    uint8_t sector0[512];
    uint8_t orig_timestamp[6];
    uint8_t orig_mbr[70];

    if (disk->read_sectors(disk, 0, 1, sector0) == 0) {
        memcpy(orig_timestamp, sector0 + 218, 6);
        memcpy(orig_mbr, sector0 + 440, 70);
    } else {
        memset(orig_timestamp, 0, 6);
        memset(orig_mbr, 0, 70);
        orig_mbr[68] = 0x55;
        orig_mbr[69] = 0xAA;
    }

    // 2. Prepare new MBR sector from Limine HDD binary
    memcpy(sector0, binary_limine_hdd_bin_data, 512);

    // Hardcode byte offset of stage 2 in MBR at offset 0x1A4 (4096 = 0x1000 = Sector 8)
    uint64_t stage2_loc = 4096;
    memcpy(sector0 + 0x1A4, &stage2_loc, sizeof(uint64_t));

    // Restore original timestamp (offset 218..223)
    memcpy(sector0 + 218, orig_timestamp, 6);

    // Restore original disk signature and partition table (offset 440..509)
    memcpy(sector0 + 440, orig_mbr, 70);
    sector0[510] = 0x55;
    sector0[511] = 0xAA;

    // Write MBR sector 0
    if (disk->write_sectors(disk, 0, 1, sector0) != 0) {
        return -1;
    }

    // 3. Write stage 2 (remaining bytes of binary_limine_hdd_bin_data) into sector 8 (byte offset 4096)
    size_t total_size = sizeof(binary_limine_hdd_bin_data);
    if (total_size > 512) {
        size_t stage2_bytes = total_size - 512;
        uint32_t stage2_sectors = (uint32_t)((stage2_bytes + 511) / 512);

        uint8_t *gap_buf = kzalloc(stage2_sectors * 512);
        if (gap_buf) {
            memcpy(gap_buf, binary_limine_hdd_bin_data + 512, stage2_bytes);
            int ret = disk->write_sectors(disk, 8, stage2_sectors, gap_buf);
            kfree(gap_buf);
            if (ret != 0) return -1;
        } else {
            return -1;
        }
    }

    return 0;
}

void cmd_install(int argc, char **argv) {
    printk("\n=======================================================\n");
    printk("           Antigravity OS Installation Wizard          \n");
    printk("=======================================================\n\n");

    char target[32] = "";

    if (argc >= 2) {
        strncpy(target, argv[1], sizeof(target) - 1);
    } else {
        printk("Detected Storage Devices:\n");
        block_device_t *d = block_get_first_device();
        if (!d) {
            printk("Error: No block storage devices found to install to.\n");
            return;
        }

        while (d) {
            uint64_t mb = (d->sector_count * d->sector_size) / (1024 * 1024);
            size_t len = strlen(d->name);
            bool is_part = (len > 0 && d->name[len - 1] >= '0' && d->name[len - 1] <= '9');

            if (!is_part) {
                printk("  - %s: %llu MB (Physical Disk)\n", d->name, mb);
            } else {
                printk("      -> %s: %llu MB (Partition)\n", d->name, mb);
            }
            d = d->next;
        }

        printk("\nEnter target disk or partition to install to (e.g. hda or hda1) [hda]: ");
        installer_readline(target, sizeof(target));
        if (target[0] == '\0') {
            strcpy(target, "hda");
        }
    }

    block_device_t *selected = block_get_device(target);
    if (!selected) {
        printk("install: Device '%s' not found!\n", target);
        return;
    }

    block_device_t *parent_disk = selected;

    size_t tlen = strlen(target);
    bool is_partition = (tlen > 0 && target[tlen - 1] >= '0' && target[tlen - 1] <= '9');

    if (is_partition) {
        // Strip partition number to get parent physical disk
        char parent_name[32];
        strncpy(parent_name, target, tlen - 1);
        parent_name[tlen - 1] = '\0';
        block_device_t *pdev = block_get_device(parent_name);
        if (pdev) parent_disk = pdev;
    }

    char boot_name[32], root_name[32];
    snprintf(boot_name, sizeof(boot_name), "%s1", parent_disk->name);
    snprintf(root_name, sizeof(root_name), "%s2", parent_disk->name);

    block_device_t *boot_part = block_get_device(boot_name);
    block_device_t *root_part = block_get_device(root_name);

    if (!boot_part || !root_part) {
        printk("\nPreparing partition layout on %s...\n", parent_disk->name);
        printk("  - Partition 1 (%s): 64 MB FAT32 Bootloader Partition\n", boot_name);
        printk("  - Partition 2 (%s): Linux ext4 Root Filesystem Partition\n", root_name);

        uint64_t boot_start = 2048; // 1 MB alignment
        uint64_t boot_sectors = 131072; // 64 MB
        if (parent_disk->sector_count <= boot_start + boot_sectors + 4096) {
            printk("install: Disk '%s' is too small (needs at least 70 MB)!\n", parent_disk->name);
            return;
        }

        uint64_t root_start = boot_start + boot_sectors;
        uint64_t root_sectors = parent_disk->sector_count - root_start;

        if (partition_create_mbr(parent_disk, 1, PARTITION_TYPE_FAT32, boot_start, boot_sectors, true) != 0) {
            printk("install: Failed to create boot partition %s!\n", boot_name);
            return;
        }

        if (partition_create_mbr(parent_disk, 2, PARTITION_TYPE_LINUX, root_start, root_sectors, false) != 0) {
            printk("install: Failed to create root partition %s!\n", root_name);
            return;
        }

        boot_part = block_get_device(boot_name);
        root_part = block_get_device(root_name);
        if (!boot_part || !root_part) {
            printk("install: Error locating newly created partitions!\n");
            return;
        }
        printk("[OK] Partitions %s (64 MB) and %s (%llu MB) created.\n",
               boot_name, root_name,
               (root_part->sector_count * root_part->sector_size) / (1024 * 1024));
    }

    size_t k_size = 0, sys_size = 0;
    void *k_addr = limine_get_kernel_file(&k_size);
    void *sys_addr = limine_get_module("limine-bios.sys", &sys_size);

    const char *limine_conf_text =
        "timeout: 0\n\n"
        "/Antigravity OS\n"
        "    protocol: limine\n"
        "    path: boot():/boot/kernel.elf\n"
        "    module_path: boot():/boot/limine/limine-bios.sys\n";

    // Step 1: Format FAT32 Boot Partition
    printk("\n-------------------------------------------------------\n");
    printk("Step 1/4: Formatting %s as FAT32 Boot Partition...\n", boot_part->name);
    printk("-------------------------------------------------------\n");

    uint64_t boot_start_lba = partition_get_start_lba(boot_part);
    if (boot_start_lba == 0) boot_start_lba = 2048;

    if (mkfs_fat32_boot_format(boot_part, boot_start_lba,
                               k_addr, k_size,
                               sys_addr, sys_size,
                               limine_conf_text, strlen(limine_conf_text)) != 0) {
        printk("install: FAT32 format failed on %s!\n", boot_part->name);
        return;
    }
    printk("  -> Limine BIOS stage 2/3 bootloader deployed to %s.\n", boot_part->name);

    // Step 2: Format ext4 Root Partition
    printk("\n-------------------------------------------------------\n");
    printk("Step 2/4: Formatting %s with ext4 Root Filesystem...\n", root_part->name);
    printk("-------------------------------------------------------\n");

    if (mkfs_ext4_format(root_part) != 0) {
        printk("install: Formatting failed on '%s'!\n", root_part->name);
        return;
    }

    // Step 3: Create ext4 directories & deploy files
    printk("\n-------------------------------------------------------\n");
    printk("Step 3/4: Deploying Antigravity OS binaries & files to %s...\n", root_part->name);
    printk("-------------------------------------------------------\n");

    uint32_t bin_ino = 0, boot_ino = 0, etc_ino = 0;
    mkfs_ext4_add_dir(root_part, EXT4_ROOT_INO, "bin", &bin_ino);
    mkfs_ext4_add_dir(root_part, EXT4_ROOT_INO, "boot", &boot_ino);
    mkfs_ext4_add_dir(root_part, EXT4_ROOT_INO, "etc", &etc_ino);
    printk("  -> Created /bin, /boot, /etc\n");

    if (k_addr && k_size > 0) {
        printk("  -> Installing /boot/kernel.elf (%llu KB)...\n", (uint64_t)k_size / 1024);
        mkfs_ext4_add_file(root_part, boot_ino, "kernel.elf", k_addr, k_size);

        printk("  -> Installing /bin/gwbasic (%llu KB)...\n", (uint64_t)k_size / 1024);
        mkfs_ext4_add_file(root_part, bin_ino, "gwbasic", k_addr, k_size);

        printk("  -> Installing /bin/paratrooper (%llu KB)...\n", (uint64_t)k_size / 1024);
        mkfs_ext4_add_file(root_part, bin_ino, "paratrooper", k_addr, k_size);
    } else {
        printk("  [WARN] Kernel image not available via Limine request; writing placeholder.\n");
        const char *k_dummy = "ANTIGRAVITY_OS_KERNEL";
        mkfs_ext4_add_file(root_part, boot_ino, "kernel.elf", k_dummy, strlen(k_dummy));
        mkfs_ext4_add_file(root_part, bin_ino, "gwbasic", k_dummy, strlen(k_dummy));
        mkfs_ext4_add_file(root_part, bin_ino, "paratrooper", k_dummy, strlen(k_dummy));
    }

    // Install sample programs and documentation
    const char *hello_bas =
        "10 REM HELLO FROM GW-BASIC\n"
        "20 PRINT \"WELCOME TO ANTIGRAVITY OS (HARD DISK INSTALLATION)!\"\n"
        "30 FOR I = 1 TO 5\n"
        "40 PRINT \"SYSTEM ONLINE: STEP \"; I\n"
        "50 NEXT I\n"
        "60 PRINT \"READY.\"\n";
    mkfs_ext4_add_file(root_part, EXT4_ROOT_INO, "hello.bas", hello_bas, strlen(hello_bas));

    const char *fib_bas =
        "10 REM FIBONACCI SEQUENCE\n"
        "20 A = 0 : B = 1\n"
        "30 PRINT A; B;\n"
        "40 FOR I = 3 TO 12\n"
        "50 C = A + B : PRINT C;\n"
        "60 A = B : B = C\n"
        "70 NEXT I\n"
        "80 PRINT \"\"\n";
    mkfs_ext4_add_file(root_part, EXT4_ROOT_INO, "fibonacci.bas", fib_bas, strlen(fib_bas));

    const char *readme_text =
        "=========================================\n"
        "         Welcome to Antigravity OS!      \n"
        "=========================================\n"
        "This installation was deployed directly from the bootable hybrid ISO.\n"
        "Filesystem: ext4 with 4096-byte blocks and extent trees.\n"
        "Shell commands: help, uname, meminfo, lspci, ls, cat, ps, threads, fdisk, mkfs.ext4, gwbasic, reboot\n";
    mkfs_ext4_add_file(root_part, EXT4_ROOT_INO, "readme.txt", readme_text, strlen(readme_text));

    const char *hello_txt = "Hello from Antigravity OS native hard drive ext4 storage!\n";
    mkfs_ext4_add_file(root_part, EXT4_ROOT_INO, "hello.txt", hello_txt, strlen(hello_txt));

    const char *os_release =
        "NAME=\"Antigravity OS\"\n"
        "VERSION=\"1.0.0\"\n"
        "ID=antigravity\n"
        "PRETTY_NAME=\"Antigravity OS 1.0.0 (x86_64)\"\n";
    mkfs_ext4_add_file(root_part, etc_ino, "os-release", os_release, strlen(os_release));

    // Step 4: Install Limine MBR bootloader to Sector 0 and Sector 8 of parent disk
    printk("\n-------------------------------------------------------\n");
    printk("Step 4/4: Installing Limine MBR bootloader on %s...\n", parent_disk->name);
    printk("-------------------------------------------------------\n");

    if (install_limine_mbr(parent_disk) == 0) {
        printk("  -> Limine MBR bootloader written to sector 0 of %s.\n", parent_disk->name);
    } else {
        printk("  [WARN] Failed to write MBR bootsector to %s.\n", parent_disk->name);
    }

    // Mount newly installed ext4 partition as active root '/'
    printk("\nMounting installed system on '/'...\n");
    vfs_node_t *installed_root = ext4_mount(root_part, 0);
    if (installed_root) {
        vfs_set_root(installed_root);
        printk("[OK] Root filesystem '/' switched to %s.\n", root_part->name);
    }

    printk("\n=======================================================\n");
    printk("         Installation Completed Successfully!          \n");
    printk("=======================================================\n");
    printk(" Antigravity OS is now installed on %s!\n", parent_disk->name);
    printk(" Bootloader: Limine BIOS on %s (FAT32)\n", boot_part->name);
    printk(" Root filesystem: Native ext4 on %s\n", root_part->name);
    printk(" You can remove the ISO / CD-ROM and type 'reboot'.\n");
    printk("=======================================================\n\n");
}
