#include <shell/shell.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/fb.h>
#include <hal/serial.h>
#include <hal/cpu.h>
#include <drivers/keyboard.h>
#include <drivers/pci.h>
#include <mm/pmm.h>
#include <fs/vfs.h>
#include <sched/sched.h>
#include <hal/timer.h>
#include <shell/gwbasic.h>
#include <drivers/partition.h>
#include <fs/mkfs.h>
#include <fs/ext4.h>
#include <mm/heap.h>
#include <shell/installer.h>
#include <shell/paratrooper.h>

#define MAX_LINE 256
#define MAX_ARGS 16

static char getchar_any(void) {
    while (1) {
        if (keyboard_has_char()) {
            return keyboard_getchar();
        }
        if (serial_received()) {
            return serial_getchar();
        }
        sched_yield();
        __asm__ volatile ("pause");
    }
}

static void readline(char *buf, size_t max) {
    size_t pos = 0;
    while (1) {
        fb_draw_cursor(true);
        char c = getchar_any();
        fb_draw_cursor(false);

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

static void cmd_help(void) {
    printk("Available Shell Commands:\n");
    printk("  help       - Show this help message\n");
    printk("  clear      - Clear the screen\n");
    printk("  uname      - Print system information\n");
    printk("  meminfo    - Show physical memory usage\n");
    printk("  lspci      - Enumerate hardware PCI devices\n");
    printk("  fdisk [-l|create|delete] - Partition management utility\n");
    printk("  mkfs.ext4 <device>       - Format partition with ext4 filesystem\n");
    printk("  install [device]         - Install Antigravity OS to hard disk\n");
    printk("  cd [path]  - Change current working directory\n");
    printk("  ls [path]  - List directory contents on ext4\n");
    printk("  cat <file> - Display file contents\n");
    printk("  cp <src> <dest> - Copy a file on ext4\n");
    printk("  ps         - Show current thread / process status\n");
    printk("  threads    - Spawn preemptive background worker threads\n");
    printk("  gwbasic    - Microsoft GW-BASIC (1983) Interpreter\n");
    printk("  paratrooper - Paratrooper (1982) Orion Software arcade game\n");
    printk("  reboot     - Restart the machine\n");
}

static void cmd_uname(void) {
    printk("AntigravityOS 1.0.0-preempt (x86_64) SMP\n");
    uint64_t ticks = timer_get_ticks();
    printk("Uptime: %llu ms (timer: 1000 Hz, %llu.%03llu s)\n", ticks, ticks / 1000, ticks % 1000);
}

static void cmd_meminfo(void) {
    uint64_t total = pmm_get_total_memory() / (1024 * 1024);
    uint64_t free = pmm_get_free_memory() / (1024 * 1024);
    uint64_t used = pmm_get_used_memory() / (1024 * 1024);
    printk("Memory Information:\n");
    printk("  Total Physical RAM: %llu MB\n", total);
    printk("  Used Memory:        %llu MB\n", used);
    printk("  Free Memory:        %llu MB\n", free);
}

static void cmd_cd(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "/";
    if (strcmp(target, "~") == 0) target = "/";
    int res = vfs_chdir(target);
    if (res == -1) {
        printk("cd: %s: No such file or directory\n", target);
    } else if (res == -2) {
        printk("cd: %s: Not a directory\n", target);
    }
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : ".";
    vfs_node_t *dir = vfs_open(path, 0);
    if (!dir) {
        printk("ls: cannot access '%s': No such file or directory\n", path);
        return;
    }

    if (!(dir->flags & FS_DIRECTORY)) {
        printk("%s\n", dir->name);
        return;
    }

    uint32_t idx = 0;
    dirent_t *d;
    while ((d = vfs_readdir(dir, idx++)) != NULL) {
        if (d->type == 2) {
            printk("[DIR]  %s\n", d->name);
        } else {
            printk("[FILE] %s\n", d->name);
        }
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        printk("Usage: cat <filename>\n");
        return;
    }

    vfs_node_t *file = vfs_open(argv[1], 0);
    if (!file) {
        printk("cat: %s: No such file or directory\n", argv[1]);
        return;
    }

    if (file->flags & FS_DIRECTORY) {
        printk("cat: %s: Is a directory\n", argv[1]);
        return;
    }

    char buf[128];
    uint64_t offset = 0;
    ssize_t bytes;
    while ((bytes = vfs_read(file, offset, sizeof(buf) - 1, buf)) > 0) {
        buf[bytes] = '\0';
        printk("%s", buf);
        offset += bytes;
    }
    printk("\n");
}

static void cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        printk("Usage: cp <source> <destination>\n");
        return;
    }

    vfs_node_t *src_node = vfs_open(argv[1], 0);
    if (!src_node) {
        printk("cp: cannot stat '%s': No such file or directory\n", argv[1]);
        return;
    }

    if (src_node->flags & FS_DIRECTORY) {
        printk("cp: -r not specified; omitting directory '%s'\n", argv[1]);
        return;
    }

    size_t file_size = (size_t)src_node->size;
    void *data = NULL;
    if (file_size > 0) {
        data = kmalloc(file_size);
        if (!data) {
            printk("cp: out of memory allocating %llu bytes\n", (uint64_t)file_size);
            return;
        }
        ssize_t rd = vfs_read(src_node, 0, file_size, data);
        if (rd < 0 || (size_t)rd != file_size) {
            printk("cp: error reading '%s'\n", argv[1]);
            kfree(data);
            return;
        }
    }

    char dest_res[256];
    vfs_resolve_path(argv[2], dest_res, sizeof(dest_res));

    vfs_node_t *dest_node = vfs_open(dest_res, 0);
    char dest_dir_path[256];
    char dest_file_name[128];

    if (dest_node && (dest_node->flags & FS_DIRECTORY)) {
        strncpy(dest_dir_path, dest_res, sizeof(dest_dir_path) - 1);
        dest_dir_path[sizeof(dest_dir_path) - 1] = '\0';

        char src_res[256];
        vfs_resolve_path(argv[1], src_res, sizeof(src_res));
        const char *base = strrchr(src_res, '/');
        base = base ? base + 1 : src_res;
        strncpy(dest_file_name, base, sizeof(dest_file_name) - 1);
        dest_file_name[sizeof(dest_file_name) - 1] = '\0';
    } else {
        char *last_slash = strrchr(dest_res, '/');
        if (last_slash == dest_res) {
            strcpy(dest_dir_path, "/");
            strncpy(dest_file_name, last_slash + 1, sizeof(dest_file_name) - 1);
            dest_file_name[sizeof(dest_file_name) - 1] = '\0';
        } else if (last_slash != NULL) {
            *last_slash = '\0';
            strncpy(dest_dir_path, dest_res, sizeof(dest_dir_path) - 1);
            dest_dir_path[sizeof(dest_dir_path) - 1] = '\0';
            strncpy(dest_file_name, last_slash + 1, sizeof(dest_file_name) - 1);
            dest_file_name[sizeof(dest_file_name) - 1] = '\0';
        } else {
            strcpy(dest_dir_path, "/");
            strncpy(dest_file_name, dest_res, sizeof(dest_file_name) - 1);
            dest_file_name[sizeof(dest_file_name) - 1] = '\0';
        }
    }

    vfs_node_t *parent_node = vfs_open(dest_dir_path, 0);
    if (!parent_node || !(parent_node->flags & FS_DIRECTORY)) {
        printk("cp: cannot create regular file in '%s': Not a directory\n", dest_dir_path);
        if (data) kfree(data);
        return;
    }

    block_device_t *dev = ext4_get_device(parent_node);
    if (!dev) {
        printk("cp: target filesystem does not support writing\n");
        if (data) kfree(data);
        return;
    }

    uint32_t parent_ino = (uint32_t)parent_node->inode;
    if (mkfs_ext4_add_file(dev, parent_ino, dest_file_name, data, file_size) != 0) {
        printk("cp: failed writing '%s' to '%s'\n", dest_file_name, dest_dir_path);
    }

    if (data) kfree(data);
}

static void cmd_ps(void) {
    thread_t *curr = sched_get_current_thread();
    process_t *proc = sched_get_current_process();

    printk("PID  TID  STATE    NAME\n");
    if (curr) {
        printk("%u    %u    RUNNING  %s (%s)\n",
               (uint32_t)(proc ? proc->pid : 0),
               (uint32_t)curr->tid,
               curr->name,
               proc ? proc->name : "kernel");
    }
}

static void worker_thread(void *arg) {
    const char *id = (const char *)arg;
    for (int i = 0; i < 3; i++) {
        printk("[%s worker] Running preemptively in background (tick %d)\n", id, i);
        // Sleep or do work
        for (volatile int j = 0; j < 10000000; j++);
    }
    printk("[%s worker] Finished and exiting.\n", id);
}

static void cmd_threads(void) {
    printk("Spawning two preemptible background worker threads...\n");
    thread_create(NULL, "worker_1", worker_thread, "Thread-A");
    thread_create(NULL, "worker_2", worker_thread, "Thread-B");
    printk("Threads created. The preemptive scheduler will interleave them!\n");
}

static void cmd_reboot(void) {
    printk("Rebooting hardware...\n");
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE); // Pulse reset line
    hlt();
}

static void execute_line(char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    char *argv[MAX_ARGS];
    int argc = 0;
    char *p = line;

    while (*p && argc < MAX_ARGS) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }

    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "clear") == 0) {
        fb_clear(0x00000000);
    } else if (strcmp(argv[0], "uname") == 0) {
        cmd_uname();
    } else if (strcmp(argv[0], "meminfo") == 0) {
        cmd_meminfo();
    } else if (strcmp(argv[0], "lspci") == 0) {
        pci_dump_devices();
    } else if (strcmp(argv[0], "cd") == 0) {
        cmd_cd(argc, argv);
    } else if (strcmp(argv[0], "cp") == 0) {
        cmd_cp(argc, argv);
    } else if (strcmp(argv[0], "ls") == 0) {
        cmd_ls(argc, argv);
    } else if (strcmp(argv[0], "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(argv[0], "ps") == 0) {
        cmd_ps();
    } else if (strcmp(argv[0], "threads") == 0) {
        cmd_threads();
    } else if (strcmp(argv[0], "fdisk") == 0) {
        cmd_fdisk(argc, argv);
    } else if (strcmp(argv[0], "mkfs.ext4") == 0 || strcmp(argv[0], "mkfs") == 0) {
        cmd_mkfs_ext4(argc, argv);
    } else if (strcmp(argv[0], "install") == 0) {
        cmd_install(argc, argv);
    } else if (strcmp(argv[0], "gwbasic") == 0 || strcmp(argv[0], "/bin/gwbasic") == 0) {
        cmd_gwbasic(argc, argv);
    } else if (strcmp(argv[0], "paratrooper") == 0 || strcmp(argv[0], "/bin/paratrooper") == 0) {
        cmd_paratrooper(argc, argv);
    } else if (strcmp(argv[0], "reboot") == 0) {
        cmd_reboot();
    } else {
        printk("Unknown command: '%s'. Type 'help' for available commands.\n", argv[0]);
    }
}

void shell_init(void) {
    printk("\n=========================================\n");
    printk("      Welcome to Antigravity OS!         \n");
    printk("      Type 'help' for commands.          \n");
    printk("=========================================\n\n");
}

void shell_run(void *arg) {
    (void)arg;
    shell_init();
    char line[MAX_LINE];

    while (1) {
        printk("root@antigravity:%s# ", vfs_getcwd());
        readline(line, sizeof(line));
        execute_line(line);
    }
}
