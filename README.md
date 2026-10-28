# Antigravity OS

<p align="center">
  <b>A modern, freestanding x86_64 operating system built from scratch in C11.</b>
  <br />
  Featuring preemptive multitasking, a custom ext4 filesystem engine with extent trees, pure-C <code>mkfs.ext4</code> formatter, MBR partition manager, native self-installer, ATA/AHCI storage drivers, and integrated Microsoft GW-BASIC runtime.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Architecture-x86__64%20(Long%20Mode)-blue?style=flat-square" alt="x86_64" />
  <img src="https://img.shields.io/badge/Language-C11%20Freestanding-00599C?style=flat-square" alt="C11 Freestanding" />
  <img src="https://img.shields.io/badge/Bootloader-Limine%20v2%20(BIOS%20%2B%20UEFI)-orange?style=flat-square" alt="Limine" />
  <img src="https://img.shields.io/badge/Filesystem-ext4%20(Extent%20Trees)-green?style=flat-square" alt="ext4" />
  <img src="https://img.shields.io/badge/License-MIT-purple?style=flat-square" alt="License" />
</p>

---

## Demo Video

<p align="center">
  <a href="https://youtu.be/e1AXccxDFdU" target="_blank">
    <img src="https://img.youtube.com/vi/e1AXccxDFdU/maxresdefault.jpg" alt="Antigravity OS Demo Video" width="800" style="max-width: 100%; border-radius: 8px;" />
  </a>
  <br />
  <em>▶️ <b><a href="https://youtu.be/e1AXccxDFdU">Watch the full Antigravity OS demo on YouTube</a></b> (VMware Workstation SATA installation, GW-BASIC, Paratrooper, Preemptive Multitasking)</em>
</p>

> **Demo Timeline**:
> 1. **VMware Setup** (00:00): Stock VM creation with 8 GB SATA HDD & 256 MB RAM without custom tweaks.
> 2. **Live ISO Boot & Installer** (00:09): Live boot via Limine, checking raw disk with `fdisk -l`, and running `install sda`.
> 3. **Dual-Partition Deployment**: Automated FAT32 boot partition + ext4 root partition formatting and file deployment.
> 4. **Standalone SATA Boot** (00:45): Direct boot from SATA hard drive (`sda2`) with ext4 root filesystem mounted.
> 5. **Ext4 Traversal**: Directory and file inspection (`ls`, `cd bin`, `ls`).
> 6. **GW-BASIC** (01:05): Running Microsoft GW-BASIC (1983), loading and executing `hello.bas`.
> 7. **Paratrooper (1982)** (01:28): CGA arcade gameplay with smooth linear framebuffer rendering.
> 8. **Kernel Multitasking** (01:58): Spawning preemptive background worker threads concurrent with interactive shell.
>
> *(An offline copy is also available in the repository at [`docs/demo.mp4`](docs/demo.mp4))*

---

## 🧪 About This Experiment

> [!NOTE]
> **Disclaimer**: This OS is an experimental project and may have bugs.
>
> * **Tooling & AI Model**: This project was an experiment built using the **Antigravity CLI (`agy`)** and **Gemini 3.8 Flash (Medium)**.
> * **Speed & Efficiency**: The entire operating system was built from scratch in **under 3 hours** within a single session limit, consuming **just ~5% of the weekly quota**.
> * **AI Pair Programming**: Other than one stubborn keyboard input bug that Gemini struggled to fix — where **Claude Opus** was brought in and pinpointed the issue in under a minute — **everything else across the entire OS was written end-to-end by Gemini**.

---

## Highlights

- ⚡ **x86_64 Freestanding Kernel**: Written in clean, modular C11 (`-ffreestanding -mno-red-zone -nostdlib -O2`) with under 150 lines of total assembly.
- 🚀 **Dual Boot Hybrid Protocol**: Powered by the Limine Boot Protocol v2, booting out of the box on both legacy BIOS and modern UEFI machines.
- ⏱️ **Preemptive Multitasking**: Preemptive Round-Robin scheduler driven by hardware PIT timer interrupts (1000 Hz / 1ms tick) with dedicated 64 KB kernel thread stacks.
- 🗄️ **Native ext4 & FAT32 Filesystem Engines**:
  - Custom extent-tree (`0xF30A`) and inode traversal driver for reading ext4 partitions.
  - Complete standalone **`mkfs.ext4`** formatter implemented inside the kernel without external tools.
  - Custom freestanding **`mkfs.fat32`** boot partition formatter generating BPB, FSInfo, FAT tables, and LFN entries.
- 💾 **Dual Storage Stack (IDE + SATA AHCI)**:
  - **ATA PIO Driver**: Probes primary and secondary master/slave channels (`hda`..`hdd`).
  - **PCI AHCI Driver**: Scans PCI Class 01 Subclass 06, maps ABAR MMIO registers, and transfers sectors via 48-bit DMA LBA (`sda`..`sdz`).
- 🛠️ **Built-in Storage & Partition Tools**:
  - **`fdisk`**: Scans, lists, creates (1MB alignment), and deletes MBR partitions on any drive.
  - **`mkfs.ext4`**: Formats raw disk partitions with 4096-byte blocks and compliant directory structures.
  - **`install`**: Automated installation wizard that creates a dual-partition layout (64MB FAT32 boot + ext4 root), writes the Limine MBR bootloader, deploys binaries, and mounts `/`.
- 📟 **Interactive Shell & Console**: Linear 1024x768x32 framebuffer terminal with smooth 64-bit word scrolling, custom 8x16 bitmap font, dual keyboard (PS/2) and serial (UART 16550 COM1) I/O.
- 📜 **Classic Microsoft GW-BASIC (1983)**: Fully integrated GW-BASIC interpreter capable of running interactive sessions and standalone `.bas` script files from ext4 storage.
- 🪂 **Paratrooper (1982) Arcade Game**: Pure freestanding C11 source port of the legendary 1982 Orion Software game featuring upscaled CGA Mode 4 graphics, real-time PC Speaker audio, and authentic troop drop/takeover mechanics.
- 🍓 **Raspberry Pi 3 (AArch64) Port**: `make ARCH=aarch64` builds a bootable SD card image. Paratrooper runs over HDMI with HDMI audio, played with an **Xbox Wireless Controller over Bluetooth LE** through the kernel's own BCM43438 driver and BLE host (pairing, encryption, HID over GATT). See [`docs/arm_ble_xbox_roadmap.md`](docs/arm_ble_xbox_roadmap.md).

---

## Architecture Overview

```
+---------------------------------------------------------------------------------+
|                                User / Shell Space                               |
|   Interactive Shell   |   GW-BASIC Interpreter   |   Paratrooper Arcade Game    |
|   fdisk Partitioning  |   mkfs.ext4 Formatter    |   Automated Self-Installer   |
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

For comprehensive architectural design, subsystem layouts, and memory maps, see [**`docs/architecture.md`**](docs/architecture.md).

---

### Execution Mode & Binary Format Notes

> [!NOTE]
> - **Privilege Level**: Currently, **no User Mode (Ring 3) is implemented**. All threads, the interactive shell, and built-in runtimes (GW-BASIC, Paratrooper, utilities) execute in **Kernel Mode (Ring 0)** with direct hardware access.
> - **Kernel Binary**: The kernel itself boots as a freestanding 64-bit ELF executable (`kernel.elf`) loaded via the Limine boot protocol. Built-in programs are linked directly into the kernel image.
> - **ELF Userland Loader (Roadmap)**: Loading standalone ELF64 user binaries from ext4 storage into separate virtual page tables with Ring 3 privilege transitions (`syscall`/`sysret`) is currently under development on the roadmap.

---

## Quickstart: Running with QEMU

### 1. Boot Live ISO Environment

```bash
# Boot the live ISO with an attached hard disk for testing
qemu-system-x86_64 -boot d \
  -cdrom antigravity-os.iso \
  -drive file=disk.img,format=raw,index=0,media=disk \
  -serial stdio \
  -m 512M
```

### 2. Test in SATA / AHCI Mode (Modern Storage)

```bash
# Create a 64MB test target disk
qemu-img create -f raw target.img 64M

# Boot with an AHCI SATA controller
qemu-system-x86_64 -boot d \
  -cdrom antigravity-os.iso \
  -device ahci,id=ahci \
  -drive id=satadisk,file=target.img,if=none,format=raw \
  -device ide-hd,drive=satadisk,bus=ahci.0 \
  -serial stdio \
  -m 512M
```

### 3. Deploy to Hard Disk & Boot Standalone

From inside the Antigravity OS shell:
```bash
# One-command automated installation
root@antigravity:/# install sda

# Reboot after installation
root@antigravity:/# reboot
```

Then boot directly from the installed disk (without CD-ROM):
```bash
qemu-system-x86_64 -hda target.img -serial stdio -m 512M
```

---

## Running in VMware Workstation / Player

Antigravity OS runs **out of the box on VMware Workstation and VMware Player with zero VM configuration changes**:

### 1. Default VM Setup (Zero Modifications Required)
When you create a standard VM in VMware, it defaults to a **SATA** virtual hard disk (`sata0:0`). Unlike other hobbyist kernels that require downgrading disks to legacy IDE, Antigravity OS works directly with VMware's default SATA hardware:
- **Guest OS Profile**: Select **Other** &rarr; **Other 64-bit** (or **Other Linux 64-bit**).
- **RAM**: `512 MB` or higher.
- **Processors**: `1` or `2` vCPUs.
- **Hard Disk**: Leave VMware's default **SATA** (`sata0:0`) disk untouched. Antigravity OS's native AHCI driver automatically detects and brings it online as `sda`.
- **CD/DVD**: Attach `antigravity-os.iso`.

### 2. Live Boot & One-Command Installation
1. Power on the VM to boot into the Live ISO environment.
2. In the interactive terminal, run the automated installation wizard targeting the SATA disk:
   ```bash
   root@antigravity:/# install sda
   ```
3. The installer automatically executes all deployment steps:
   - Sets up an MBR table with a **64MB FAT32 Boot Partition** (`sda1`, Type `0x0C`, Bootable) + **Linux ext4 Root Partition** (`sda2`, Type `0x83`).
   - Formats `sda1` with FAT32 and deploys Limine BIOS bootloader files (`limine-bios.sys`, `limine.conf`, `kernel.elf`).
   - Formats `sda2` with 4KB ext4 blocks and installs `/bin/gwbasic`, `/bin/paratrooper`, `/boot/kernel.elf`, and configuration files.
   - Writes the Limine MBR Stage 1 bootloader to Sector 0 and Stage 2 to Sector 8 of `sda`.

### 3. Standalone SATA Boot
1. Disconnect or detach the ISO from the VM's CD/DVD drive.
2. Power on or restart the VM.
3. Antigravity OS boots directly and independently from the VMware SATA virtual hard disk (`.vmdk`) into the interactive shell!

---

## Built-in Shell Utilities

| Utility | Syntax | Description |
| :--- | :--- | :--- |
| `fdisk` | `fdisk [-l \| create \| delete]` | MBR partition table manager. Lists disks, creates bootable ext4 partitions (LBA 2048), and removes entries. |
| `mkfs.ext4` | `mkfs.ext4 <device>` | Freestanding ext4 formatter. Initializes 4KB blocks, bitmaps, inode tables, and extent trees. |
| `install` | `install [device]` | OS deployment wizard. Partitions disk, formats ext4, deploys kernel and Limine bootloader, and mounts `/`. |
| `gwbasic` | `gwbasic [script.bas]` | Microsoft GW-BASIC (1983) interpreter. Supports interactive REPL and executing `.bas` files. |
| `paratrooper` | `paratrooper` | Paratrooper (1982) arcade game with CGA 320x200 graphics and authentic PC Speaker audio. |
| `cd` | `cd [path]` | Changes current working directory with relative path, `..`, and `~` support. Updates prompt. |
| `ls` | `ls [path]` | Lists files and directories in current working directory or specified path on ext4 storage. |
| `cat` | `cat <file>` | Displays file contents to the console (relative or absolute path). |
| `cp` | `cp <src> <dest>` | Copies a file to a new destination or target directory on the ext4 filesystem. |
| `ps` | `ps` | Lists running threads and processes under the preemptive scheduler. |
| `threads` | `threads` | Spawns background worker threads to demonstrate preemptive timer interleaving. |
| `lspci` | `lspci` | Enumerates PCI hardware devices, vendor/device IDs, and BARs. |
| `meminfo` | `meminfo` | Reports physical RAM usage and allocator metrics. |
| `uname` | `uname` | Displays kernel release, CPU architecture, and uptime. |
| `clear` | `clear` | Clears linear framebuffer and resets terminal cursor. |
| `reboot` | `reboot` | Performs hardware reset via keyboard controller pulse. |

For detailed utility options, flags, and command examples, see [**`docs/utilities.md`**](docs/utilities.md).

---

## Building from Source

### Prerequisites

Install build essentials on Ubuntu / Debian / WSL2:

```bash
sudo apt update
sudo apt install -y build-essential gcc ld xorriso mtools
```

### Build Commands

```bash
# Clone the repository
git clone https://github.com/your-username/antigravity-os.git
cd antigravity-os

# Clean and compile kernel ELF, test ext4 image, and bootable hybrid ISO
make clean
make

# Run directly in QEMU
make run
```

The build produces:
- `kernel.elf`: The freestanding 64-bit kernel ELF executable.
- `disk.img`: Pre-populated 32MB ext4 test storage image.
- `antigravity-os.iso`: Hybrid bootable ISO with Limine BIOS & UEFI bootloaders.

---

## Project Structure

```text
├── Makefile                   # Build pipeline for kernel, ext4 disk, and hybrid ISO
├── linker.ld                  # Higher-half kernel linker script (0xffffffff80000000)
├── limine.conf                # Limine bootloader configuration
├── limine.h                   # Limine Boot Protocol v2 request declarations
├── AGENTS.md                  # Machine-readable technical specifications for AI agents
├── docs/
│   ├── architecture.md        # In-depth architectural specification and system diagrams
│   ├── status.md              # Feature completion matrix and verification reports
│   └── utilities.md           # Command reference manual for all shell utilities
├── include/                   # Kernel header declarations
│   ├── drivers/               # Block, partition, ATA, AHCI, PCI, keyboard headers
│   ├── fs/                    # VFS, ext4, and FAT32 boot mkfs declarations
│   ├── hal/                   # CPU, GDT, IDT, PIC, APIC, serial, timer headers
│   ├── kernel/                # Types, printk, font, framebuffer, string utilities
│   ├── mm/                    # PMM, VMM, and heap allocator declarations
│   └── sched/                 # Thread, process, and preemptive scheduler headers
├── kernel/                    # Kernel implementation
│   ├── arch/x86_64/           # GDT, IDT, PIC, APIC, timer, serial, assembly entry
│   ├── drivers/               # PS/2 keyboard, PCI, ATA PIO, AHCI SATA, MBR partition
│   ├── fs/                    # VFS, ext4 driver, and freestanding mkfs engine
│   ├── init/                  # Kernel entry point (kmain) and subsystem startup
│   ├── lib/                   # Framebuffer, font rendering, printk, string routines
│   ├── mm/                    # Physical PMM, virtual VMM, and kernel heap allocators
│   ├── sched/                 # Preemptive round-robin scheduler and context switcher
│   └── shell/                 # Interactive shell, GW-BASIC interpreter, Paratrooper game, OS installer
└── limine-bin/                # Limine BIOS/UEFI boot binaries and MBR stage 1.5 data
```

---

## Documentation Links

- 🏛️ [**Architecture Guide** (`docs/architecture.md`)](docs/architecture.md)
- 📊 [**System Status & Roadmap** (`docs/status.md`)](docs/status.md)
- 🛠️ [**Utilities & Command Reference** (`docs/utilities.md`)](docs/utilities.md)
- 🤖 [**Agent & LLM Developer Spec** (`AGENTS.md`)](AGENTS.md)

---

## License

Antigravity OS is released under the **MIT License**. See [LICENSE](LICENSE) for details.
