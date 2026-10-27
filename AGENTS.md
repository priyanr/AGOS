# Antigravity OS - Agent Information

## Project Identity
- **Name**: Antigravity OS
- **Target Architecture**: x86_64 (64-bit Long Mode)
- **Boot Protocol**: Limine Boot Protocol v2 (BIOS & UEFI hybrid)
- **Language**: Freestanding C11 (`-ffreestanding -mno-red-zone -mno-sse -mno-80387 -nostdlib -O2`) with minimal x86_64 assembly (< 150 lines total)
- **Filesystem**: Native read/write ext4 driver with extent tree support (`0xF30A`) and 4096-byte blocks
- **Multitasking**: Preemptive round-robin scheduler with PIT timer interrupts (1000 Hz, 1ms tick)

## Key Subsystems
1. **Boot & Entry**: Limine bootloader protocol handshake, GDT, TSS, IDT (256 gates), 8259 PIC remapping, Local APIC timer/routing.
2. **Memory Management**:
   - PMM: Physical Memory Manager (bitmap-based frame allocator over Limine memory map).
   - VMM: Virtual Memory Manager (PML4 4-level paging with Limine Higher Half Direct Map offset).
   - Heap: First-fit linked list kernel heap allocator (`kmalloc`, `kzalloc`, `kcalloc`, `kfree`), 4MB initial heap pool.
3. **Hardware Drivers**:
   - Linear framebuffer console (`fb.c`) with 8x16 bitmap font.
   - 16550 UART serial driver for kernel debugging (`COM1` at 0x3F8).
   - PS/2 Keyboard driver with Scan Code Set 1 translation.
   - PCI bus scanner with Class/Subclass enumeration.
   - ATA PIO storage driver supporting primary/secondary master/slave channels (`hda`..`hdd`).
   - PCI AHCI SATA storage driver supporting 48-bit DMA LBA (`sda`..`sdz`).
4. **Storage & Partitioning**:
   - MBR partition table scanner and partition abstraction (`hda1`..`hda4`, `sda1`..`sda4`).
   - Partition creation and deletion utility (`fdisk`).
5. **Filesystem**:
   - VFS (Virtual File System) abstraction with node callbacks (`read`, `readdir`, `finddir`), active CWD tracking (`vfs_cwd`), and relative path canonicalization (`.` and `..`).
   - Custom ext4 read driver with extents and directory traversal.
   - Custom freestanding `mkfs.ext4` formatter and extent tree file/directory creator.
   - Custom freestanding `mkfs.fat32` formatter generating compliant BPB, FSInfo, FAT tables, and LFN records for bootloader partitions.
6. **User Space / Shell**:
   - Interactive shell with history, cursor, dynamic CWD prompt (`root@antigravity:<cwd>#`), and built-in commands (`cd`, `ls`, `cat`, `cp`).
   - Integrated Microsoft GW-BASIC (1983) interpreter binary.
   - Built-in Paratrooper (1982) Orion Software CGA arcade game (`paratrooper`).
   - Native hard drive installer (`install`) automating dual-partition deployment (64MB FAT32 boot partition + ext4 root partition), Limine MBR bootloader installation, and live root hot-mounting.

## Documentation References
- [`docs/architecture.md`](docs/architecture.md): Complete technical architecture, subsystem interaction diagrams, memory layouts, and driver designs.
- [`docs/status.md`](docs/status.md): Project status matrix, verified virtualization targets (QEMU, VMware Workstation/Player), bug fixes, and future roadmap.
- [`docs/utilities.md`](docs/utilities.md): Comprehensive reference guide for all built-in utilities (`fdisk`, `mkfs.ext4`, `install`, `gwbasic`, `paratrooper`, `cd`, `ls`, `cat`, `cp`, `ps`, `threads`, `lspci`, `meminfo`, `uname`, `clear`, `help`, `reboot`).
- [`docs/arm_ble_xbox_roadmap.md`](docs/arm_ble_xbox_roadmap.md): Raspberry Pi AArch64 architecture, Bluetooth LE Xbox controller pairing, VCHIQ HDMI sound, and packaging guide.

## Build Requirements
- GNU Make, GCC (`gcc`), GNU Linker (`ld`), `xorriso`, `limine` CLI.
