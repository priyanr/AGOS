# Antigravity OS Architecture

## 1. System Overview

Antigravity OS is an x86_64 Long Mode operating system engineered in freestanding C11 with minimal assembly code (<150 lines total). It is designed to boot on both legacy BIOS and UEFI platforms using the Limine Boot Protocol v2, providing a preemptive multitasking kernel, a virtual file system with custom ext4 driver, interactive command shell, Microsoft GW-BASIC runtime, and native self-installation utilities.

```
+---------------------------------------------------------------------------------+
|                                User / Shell Space                               |
|   Interactive Shell   |   GW-BASIC Interpreter   |   fdisk   |   install   | ...|
+---------------------------------------------------------------------------------+
|                               Virtual File System                               |
|                         VFS Layer (vfs_node_t / dirent_t)                       |
|           ext4 Driver (Extents, Inodes)   |   mkfs.ext4 Formatter Engine        |
+---------------------------------------------------------------------------------+
|                                 Kernel Subsystems                               |
|     Preemptive Scheduler      |     Thread / Process Manager (64KB Stack)       |
|     VMM (PML4 4-Level Paging) |     PMM (Bitmap Frame Allocator)                |
|     Heap Manager (kmalloc)    |     Linear Framebuffer & Font Rendering         |
+---------------------------------------------------------------------------------+
|                            Hardware Abstraction Layer                           |
|       GDT / TSS       |       IDT (256 vectors)       |       8259 PIC / APIC   |
|       PIT (1000 Hz)   |       UART 16550 Serial       |       PS/2 Keyboard     |
|       PCI Enumerator  |       ATA PIO / AHCI SATA     |       MBR Partitions    |
+---------------------------------------------------------------------------------+
|                                   Hardware                                      |
|            x86_64 CPU (Long Mode)   |   RAM   |   Storage Disks / ISO           |
+---------------------------------------------------------------------------------+
```

---

### 1.1 Source Layout: shared kernel, per-architecture HAL and drivers (2026-10-28)

Everything except `kernel/arch/<arch>/` is compiled unchanged for x86_64 and AArch64: shell and
utilities, GW-BASIC, Paratrooper, scheduler, PMM/heap, VFS, ext4, FAT32, MBR partitions, block
registry, printk/framebuffer. The Makefile builds `COMMON_C_SOURCES` plus `kernel/arch/$(ARCH)/`.

| Layer | x86_64 (`kernel/arch/x86_64/`) | AArch64 Pi 3 (`kernel/arch/aarch64/`) |
|---|---|---|
| Entry | `boot.c` (Limine requests, `kmain`) | `boot.S`, `kmain.c` |
| HAL (`include/hal/platform.h`, `cpu.h`) | `platform.c`, `entry.S` (context switch), GDT/IDT/PIC/APIC, PIT | `platform.c` (IRQ dispatch, watchdog reboot), `boot.S` (IRQ entry, context switch), generic timer, `mmu.c` |
| Memory | `vmm.c` (PML4 + HHDM) | `mmu.c` (identity map; VMM calls are no-ops) |
| Drivers | PS/2 keyboard, PCI, ATA, AHCI, PC speaker, installer | USB (DWC2 + hub + HID keyboard), SD card (Arasan SDHCI), HDMI audio, Bluetooth |

- Both entry points end in the shared `kernel_start()` (`kernel/init/main.c`): mount root, start the
  scheduler, spawn the shell.
- Decision: the HAL is a few plain functions (`hal_thread_stack_init`, `hal_switch_context`,
  `hal_set_kernel_stack`, `hal_reboot`, `hal_poll_input`, `irq_save/irq_restore`, `cpu_relax`), not a
  driver framework. Device APIs (`keyboard.h`, `sound.h`, `gamepad.h`, `block.h`) were already shared.
- Decision: commands for hardware a machine lacks stay in the shared shell; the arch prints why
  (`lspci` and `install` on the Pi).

## 2. Boot & Early Initialization

### 2.1 Limine Boot Protocol Handshake
The kernel defines Limine protocol requests inside the `.limine_requests` ELF section:
- **Base Revision Request**: Revision 6 negotiation.
- **Framebuffer Request**: Acquires linear framebuffer address, width, height, pitch, and BPP (typically 1024x768x32).
- **Memory Map Request**: Discovers physical memory entries (usable RAM, bootloader reserved, ACPI, MMIO).
- **Higher Half Direct Map (HHDM) Request**: Maps all physical RAM at a fixed virtual offset (`0xffff800000000000` or dynamic Limine base).
- **Executable File Request**: Provides kernel ELF image location in memory, enabling the self-installer to deploy the OS without external media drivers.
- **Module Request**: Discovers additional boot modules (e.g., `limine-bios.sys`).

### 2.2 CPU State Setup
- **GDT & TSS** ([`kernel/arch/x86_64/gdt.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/gdt.c)):
  - Null Descriptor (0x00).
  - 64-bit Kernel Code Segment (0x08) with `L=1`, `D=0`, ring 0.
  - Kernel Data Segment (0x10).
  - User Code Segment (0x1B) with ring 3 privileges.
  - User Data Segment (0x23) with ring 3 privileges.
  - Task State Segment (TSS) for handling stack switches on privilege transitions and hardware interrupts (`RSP0` and `IST`).
- **IDT** ([`kernel/arch/x86_64/idt.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/idt.c)):
  - Full 256-entry Interrupt Descriptor Table populated with 64-bit interrupt gates (`DPL=0` or `DPL=3`).
  - CPU exceptions 0..31 with exception dump reporting.
  - Hardware IRQ lines 32..47 mapped from PIC.
- **Interrupt Controllers** ([`kernel/arch/x86_64/pic.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/pic.c) & [`kernel/arch/x86_64/apic.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/apic.c)):
  - Dual 8259 PIC remapped to vectors `0x20..0x27` (Master) and `0x28..0x2F` (Slave).
  - Local APIC detected and initialized, with LINT0 ExtINT routed for legacy PIC compatibility.
- **PS/2 Keyboard Controller Subsystem** ([`kernel/arch/x86_64/drivers/keyboard.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/drivers/keyboard.c)):
  - 8042 controller configuration: drains residue buffer, enables port 1 (`0xAE`), reads/writes CCB (enabling IRQ1 and scan-code translation).
  - Handles PS/2 Scan Code Set 2 break code prefix `0xF0` and extended code prefix `0xE0`, accurately distinguishing key presses from key releases in virtual environments (VMware/QEMU).
  - Dynamic key tracking array `key_down[128]` with a 250ms typematic repeat debounce window to eliminate character stuttering and duplicate keystrokes.
  - Dual interrupt and status port `0x64` polling fallback ensures zero missed keystrokes across heavy load.

---

## 3. Memory Architecture

### 3.1 Physical Memory Manager (PMM)
- Implemented in [`kernel/mm/pmm.c`](file:///F:/priyan/Projects/AI/OS/kernel/mm/pmm.c).
- Bit-array representing 4KB pages up to the highest physical address detected from the Limine memory map.
- Fast page allocation (`pmm_alloc_page()`, `pmm_alloc_pages(n)`) using bit scans and atomic page-free operations (`pmm_free_page()`).

### 3.2 Virtual Memory Manager (VMM)
- Implemented in [`kernel/arch/x86_64/vmm.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/vmm.c).
- Standard x86_64 4-level paging (PML4 -> PDP -> PD -> PT).
- Kernel space mapped using Higher Half Direct Map (HHDM).
- Page mapping functions (`vmm_map_page()`, `vmm_unmap_page()`) supporting write, user-mode, and cache disable flags.

### 3.3 Kernel Heap
- Implemented in [`kernel/mm/heap.c`](file:///F:/priyan/Projects/AI/OS/kernel/mm/heap.c).
- First-fit linked-list memory allocator.
- Initial heap pool allocated dynamically from PMM (4MB initial pool).
- Supports `kmalloc()`, `kzalloc()`, `kcalloc()`, and `kfree()` with block coalescing on free.

---

## 4. Scheduling & Multitasking

- Implemented in [`kernel/sched/sched.c`](file:///F:/priyan/Projects/AI/OS/kernel/sched/sched.c) and [`kernel/arch/x86_64/entry.S`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/entry.S).
- **Scheduler**: Preemptive Round-Robin scheduler driven by PIT Timer IRQ0 at 1000 Hz (1ms quantum).
- **Thread Context**: Each thread maintains a 64 KB kernel stack, saved registers (`R15..R8`, `RDI`, `RSI`, `RBP`, `RBX`, `RDX`, `RCX`, `RAX`), `RIP`, `CS`, `RFLAGS`, `RSP`, `SS`.
- **Preemption Switch**: Triggered during timer tick or voluntarily through `sched_yield()`. The assembly helper `hal_switch_context` preserves callee-saved registers and switches stack pointers.
- **Idle Handling**: Idle loop executes `sched_yield()` followed by x86 `pause` (`__builtin_ia32_pause()`) to minimize host CPU consumption and allow smooth hardware interrupt delivery.

---

## 5. Storage & Filesystem Architecture

### 5.1 Storage Driver & Partition Layer
- **ATA PIO Driver** ([`kernel/arch/x86_64/drivers/ata.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/drivers/ata.c)):
  - Controls IDE primary (0x1F0) and secondary (0x170) channels.
  - Automatically identifies ATA hard disks vs ATAPI CD-ROM drives using signature bytes `0xEB14` / `0x9669`.
  - Supports 28-bit LBA PIO sector reads and writes with DRQ polling and hardware timeouts.
- **AHCI / SATA Driver** ([`kernel/arch/x86_64/drivers/ahci.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/drivers/ahci.c)):
  - Discovers PCI SATA controllers (Class 01 Subclass 06) and configures ABAR (BAR5) MMIO registers.
  - Supports SATA disks (`sda`..`sdz`) with 48-bit LBA DMA read/write commands (`0x25` READ DMA EXT, `0x35` WRITE DMA EXT).
  - Provides native out-of-the-box storage compatibility for VMware Workstation/Player virtual SATA disks (`sata0:0`).
- **MBR Partition Layer** ([`kernel/drivers/storage/partition.c`](file:///F:/priyan/Projects/AI/OS/kernel/drivers/storage/partition.c)):
  - Reads MBR sector 0 and validates `0xAA55` signature.
  - Registers sub-block devices (`hda1`, `sda1`, etc.) by forwarding I/O with start LBA offset calculations.

### 5.2 Virtual File System (VFS) & Path Subsystem
- **VFS Abstraction** ([`kernel/fs/vfs.c`](file:///F:/priyan/Projects/AI/OS/kernel/fs/vfs.c)):
  - Tree-based POSIX-like directory and file abstraction.
  - Node callbacks: `read()`, `write()`, `readdir()`, `finddir()`.
  - Maintains the active Current Working Directory (`vfs_cwd[256]`, initialized to `"/"`).
  - Path Canonicalization (`vfs_resolve_path()`): Automatically resolves relative paths, `.` (current directory), `..` (parent directory), double slashes, and maintains root boundaries.
  - Directory Navigation: `vfs_chdir()` validates directory existence before switching; `vfs_getcwd()` supplies the active path to shell prompts.
  - Transparent Path Opening: `vfs_open()` seamlessly resolves relative filenames against the active `vfs_cwd`.

### 5.3 ext4 Filesystem Engine
- **ext4 Driver** ([`kernel/fs/ext4/ext4.c`](file:///F:/priyan/Projects/AI/OS/kernel/fs/ext4/ext4.c)):
  - Formatted with 4096-byte blocks (`s_log_block_size = 2`).
  - Extent Tree support (`ext4_extent_header_t` magic `0xF30A`, leaf extents `ext4_extent_t`).
  - Inode traversal and directory block parsing for file searches and sequential file reading.
- **Freestanding mkfs Engine** ([`kernel/fs/ext4/mkfs.c`](file:///F:/priyan/Projects/AI/OS/kernel/fs/ext4/mkfs.c)):
  - Formats raw block partitions directly from kernel space without external utilities.
  - Initializes Superblock, Block Group Descriptors, Block/Inode Bitmaps, Inode tables, Inode 2 (`/`), Inode 11 (`lost+found`).
  - Supports adding directories (`mkfs_ext4_add_dir`), writing files (`mkfs_ext4_add_file`) with extent structures, and updating directory entries.
  - Uses static block buffers to guarantee zero stack overflow during deep operations.

### 5.4 FAT32 Bootloader Formatter & Dual-Partition Architecture
- **FAT32 Engine** ([`kernel/fs/fat32/mkfs_fat.c`](file:///F:/priyan/Projects/AI/OS/kernel/fs/fat32/mkfs_fat.c)):
  - Implements `mkfs_fat32_boot_format()` in pure freestanding C to format a compliant FAT32 boot partition.
  - Writes standard BPB (BIOS Parameter Block), Extended Boot Record, FSInfo sector (free clusters and next free cluster tracking), FAT allocation tables (FAT1 & FAT2), and root directory cluster.
  - Writes compliant 8.3 short directory entries and Long File Name (LFN) checksum records for bootloader assets (`limine-bios.sys`, `limine.conf`, and `/boot/kernel.elf`).
- **Limine Dual-Partition Scheme**:
  - Limine BIOS Stage 2 bootloader lacks an ext4 filesystem driver and can only access FAT32 or ISO9660.
  - Target disks are partitioned into:
    - **Partition 1 (`sda1` / `hda1`)**: 64 MB FAT32 Boot Partition (Type `0x0C`, Bootable) hosting Limine BIOS stage 2/3 files and configuration.
    - **Partition 2 (`sda2` / `hda2`)**: ext4 Root Filesystem (Type `0x83`) hosting the operating system binaries, configurations, and user data.
  - Stage 1 MBR bootcode is written to Sector 0, and Stage 2 is written to Sector 8 (`0x1000`).

### 5.5 Auto-Root Filesystem Discovery
- Implemented in [`kernel/init/main.c`](file:///F:/priyan/Projects/AI/OS/kernel/init/main.c) (`kernel_start()`, shared by both architectures).
- On kernel boot, automatically scans block devices in prioritized candidate order: `sda2`, `hda2`, `sda1`, `hda1`, `sda`, `hda`, `mmc2` (ext4), then the Pi SD boot partition `mmc1` (FAT32, read-only).
- Validates the ext4 superblock magic (`0xEF53`).
- Mounts the first valid volume as root `/`. If none is present, proceeds cleanly in Live ISO boot mode.
- The shell reads `/antigravity.cfg` at start; `autostart=paratrooper` launches the game first (Pi default file sets `autostart=shell`).

---

## 6. Shell & User Utilities

- **Interactive Shell** ([`kernel/shell/shell.c`](file:///F:/priyan/Projects/AI/OS/kernel/shell/shell.c)):
  - Framebuffer-based text rendering with hardware cursor toggle.
  - Dual-input handling from PS/2 keyboard and UART serial port.
  - Built-in command dispatcher for system diagnosis, filesystem browsing, and hardware control:
    - Filesystem navigation: `cd` (working directory tracking), `ls` (lists directories/files), `cat` (displays file contents), `cp` (copies files to directories or new files).
    - Disk and partition management: `fdisk`, `mkfs.ext4`, and `install`.
    - Diagnostics & control: `ps`, `threads`, `lspci`, `meminfo`, `uname`, `clear`, `reboot`, and `help`.
- **Microsoft GW-BASIC Engine** ([`kernel/shell/gwbasic.c`](file:///F:/priyan/Projects/AI/OS/kernel/shell/gwbasic.c)):
  - Complete 1983-era GW-BASIC interpreter capable of running interactive sessions and standalone `.bas` script files.
- **Paratrooper (1982) Arcade Game Engine** ([`kernel/shell/paratrooper.c`](file:///F:/priyan/Projects/AI/OS/kernel/shell/paratrooper.c)):
  - Pure freestanding C11 source port of Orion Software's 1982 classic IBM PC game by Greg Kuperberg.
  - Pixel-identical CGA Mode 4 (320x200, 4-color) software rendering with 2x/3x integer upscaling to linear framebuffer.
  - Native PC Speaker tone and sound effects generator using 8254 PIT Channel 2 (ports 0x42/0x43/0x61).
  - Embedded assets from `original-pt.com` and IBM CGA font data compiled directly into the kernel.
- **Native OS Installer** ([`kernel/arch/x86_64/installer.c`](file:///F:/priyan/Projects/AI/OS/kernel/arch/x86_64/installer.c)):
  - Automated 4-step wizard that partitions disks with dual FAT32 + ext4 layout, formats both filesystems, deploys kernel ELF, bootloader files, configuration, and Limine MBR bootsector, and hot-mounts the root filesystem.

## 7. Raspberry Pi 3 (AArch64) Port

Built with `make ARCH=aarch64`: the shared kernel plus `kernel/arch/aarch64/`.
Details, protocol flow and debugging: [`arm_ble_xbox_roadmap.md`](arm_ble_xbox_roadmap.md).

- **Boot**: the GPU firmware loads `kernel8.img` at `0x80000`; `boot.S` drops to EL1, installs the
  EL1 vector table and zeroes BSS. `mmu.c` then turns on an identity map with caches.
  - Decision: build with `-mstrict-align`. Without it GCC emitted unaligned stores (e.g. a 7-byte
    array on the stack), which fault with the MMU off and hung the boot silently.
  - Decision: unhandled exceptions print ESR/ELR/FAR on screen (`exception_report()`) instead of hanging.
- **Console**: mini-UART on GPIO 14/15 (`serial.c`); the PL011 is reserved for the Bluetooth chip.
- **Bluetooth** (`kernel/arch/aarch64/drivers/bluetooth/`):
  - `bcm43438.c`: BT_ON via mailbox (expander GPIO 128), 32 kHz LPO on GPIO 43, PL011 on GPIO 32/33
    at 115200, patchram upload, LE scan/connect, HCI command/ACL transport.
  - Decision: UART RX is interrupt-driven (`bcm43438_uart_irq` fills a 4 KB ring). The PL011 FIFO
    is 16 bytes, which overflowed during one Paratrooper frame and lost advertisements.
  - `ble_host.c`: own minimal BLE host for one controller (L2CAP, SMP legacy Just Works with software
    AES-128, ATT/GATT). Decision: no third-party stack; the Xbox controller only needs one bonded,
    encrypted link with HID notifications, and the full HID-host sequence (bond, identity keys, read
    HID values, then subscribe) is what makes pairing mode finish.
  - `xbox_ble.c`: HID report to Gamepad HAL. Paratrooper reads the HAL, not Bluetooth.
- **Audio** (`kernel/arch/aarch64/drivers/sound_hdmi.c`): HDMI MAI FIFO fed by DMA channel 5 (DREQ 17) from a
  looping buffer of IEC958 subframes. Decision: drive the HDMI audio hardware directly (Linux vc4_hdmi
  sequence) instead of VCHIQ, which needs a large shared-memory protocol with the GPU.
- **Shared kernel on the Pi** (2026-10-28): `kmain_aarch64` brings up the HAL and drivers and calls
  `kernel_start()`, so the Pi runs the same shell, GW-BASIC, scheduler, VFS and filesystems as x86
  (see 1.1). Boots to the shell; `autostart=paratrooper` in `antigravity.cfg` on the SD card starts the game.
  - Interrupts: `irq_entry` (boot.S) saves x0-x18, x29, x30, ELR, SPSR and calls `irq_dispatch()`
    (platform.c): EL1 physical timer at 1 kHz (scheduler tick, routed via the core-0 local
    controller) and the Bluetooth UART RX drain (`bcm43438_uart_irq`).
  - Decision: the Pi build uses `-mgeneral-regs-only`. The context switch saves only general
    registers (as on x86 with `-mno-sse`), so no C code may use FP/SIMD registers.
  - Polled devices (Bluetooth, USB) are serviced by `hal_poll_input()` from every input wait loop,
    with a busy flag so only one thread polls at a time.
- **USB keyboard** (`drivers/usb_dwc2.c`, `drivers/usb_keyboard.c`): DWC2 host with internal DMA,
  polled, one channel, one packet at a time. The Pi 3B root device is the LAN9514 hub, so keyboards
  are low/full-speed behind a high-speed hub: split transactions, with interrupt start-splits timed
  to the microframe (Circle/USPi scheme). Hub ports are re-scanned once a second while no keyboard is
  attached (hot-plug). HID boot protocol, US layout, key repeat in software; arrow keys map to the
  same '4' '6' '8' '5' as the PS/2 driver.
- **SD card** (`drivers/sd_emmc.c`): Arasan SDHCI, PIO, 1-bit, single-block commands; GPIO 48-53
  switched from SDHOST (used by the firmware) to ALT3. Registered as block device `mmc`.
- **FAT32** (`kernel/fs/fat32/fat32.c`, shared): read-only, long file names; mounts the boot partition.
- **Debug flag**: `BLE_DEBUG_SCREEN` (`include/drivers/bluetooth.h`).
