#include <kernel/types.h>
#include <kernel/printk.h>
#include <hal/cpu.h>
#include <hal/platform.h>
#include <drivers/block.h>
#include <drivers/partition.h>
#include <fs/vfs.h>
#include <fs/ext4.h>
#include <fs/fat32.h>
#include <sched/sched.h>
#include <shell/shell.h>

// Shared kernel start-up, identical on every architecture. The arch entry point (x86: Limine
// kmain in kernel/arch/x86_64/boot.c, Pi: kmain_aarch64) has already brought up the HAL,
// memory, the heap and the device drivers, which registered their disks as block devices.
void kernel_start(void) {
    // Storage Partitions & Filesystem Subsystem (VFS, ext4, FAT32)
    vfs_init();
    partition_scan_all();

    // Prefer an ext4 root: installed PC disk (sda2/hda2...) or an ext4 partition on the Pi SD
    // card. Otherwise mount the Pi SD card's FAT32 boot partition (read-only), which holds
    // antigravity.cfg.
    const char *ext4_candidates[] = { "sda2", "hda2", "sda1", "hda1", "sda", "hda", "mmc2" };
    const char *fat32_candidates[] = { "mmc1" };
    block_device_t *root_dev = NULL;
    vfs_node_t *root = NULL;

    for (size_t i = 0; i < sizeof(ext4_candidates) / sizeof(ext4_candidates[0]) && !root; i++) {
        block_device_t *cand = block_get_device(ext4_candidates[i]);
        if (cand) {
            root = ext4_mount(cand, 0);
            if (root) root_dev = cand;
        }
    }
    for (size_t i = 0; i < sizeof(fat32_candidates) / sizeof(fat32_candidates[0]) && !root; i++) {
        block_device_t *cand = block_get_device(fat32_candidates[i]);
        if (cand) {
            root = fat32_mount(cand);
            if (root) root_dev = cand;
        }
    }

    if (root && root_dev) {
        vfs_set_root(root);
        printk("[OK] Root filesystem mounted on '/' from %s.\n", root_dev->name);
    } else {
        printk("[INFO] No mounted root filesystem (Live ISO boot mode).\n");
        printk("       Use 'fdisk' and 'mkfs.ext4' or run 'install' to set up a hard disk.\n");
    }

    // Preemptive Multitasking & Scheduler
    sched_init();

    // Spawn Interactive Shell
    thread_create(NULL, "shell", shell_run, NULL);

    printk("[OK] Enabling CPU interrupts...\n");
    sti();

    sched_yield();

    // Kernel Idle Loop
    while (1) {
        sched_yield();
        cpu_relax();
    }
}
