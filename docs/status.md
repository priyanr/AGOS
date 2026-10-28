# Antigravity OS Status Report

**Release**: Antigravity OS 1.0.0-preempt (x86_64)  
**Last Updated**: October 2026  
**Build Status**: Passing (0 warnings, 0 errors)

---

## 1. Feature Completion Matrix

| Subsystem / Feature | Status | Verification Notes |
| :--- | :---: | :--- |
| **Limine Boot Protocol v2** | Verified | Hybrid ISO boots on both BIOS and UEFI. Base revision, framebuffer, memmap, HHDM, executable, and module requests functioning. |
| **GDT & TSS** | Verified | Long mode 64-bit descriptors (kernel 0x08/0x10, user 0x1B/0x23) and TSS stack frame loaded. |
| **IDT & Interrupts** | Verified | 256 vector gates populated, exception handlers report CPU registers, 8259 PIC remapped to 0x20/0x28. |
| **Local APIC & PIT Timer** | Verified | Local APIC initialized with LINT0 ExtINT. PIT Timer running at 1000 Hz (1ms tick) driving preemption. |
| **Physical Memory (PMM)** | Verified | Dynamic bitmap allocator manages all available physical frames up to total detected RAM. |
| **Virtual Memory (VMM)** | Verified | 4-level PML4 paging with Higher Half Direct Map (HHDM) offset. |
| **Kernel Heap** | Verified | First-fit allocator with block coalescing (`kmalloc`, `kzalloc`, `kcalloc`, `kfree`), 4MB initial heap pool. |
| **Framebuffer Console** | Verified | 1024x768 (32 BPP) linear framebuffer with custom 8x16 bitmap font, smooth scrolling, and hardware cursor rendering. |
| **PS/2 Keyboard Driver** | Verified | Translated Scan Code Set 1 with shift/caps toggle; zero key repetition lag or double-press glitches. |
| **PCI Enumeration** | Verified | Bus scanner enumerates all device IDs, vendor IDs, class codes, and BAR addresses. |
| **ATA PIO Driver** | Verified | Probes primary/secondary master/slave channels (`hda`..`hdd`). ATAPI detection ignores non-disk devices. |
| **AHCI SATA Driver** | Verified | Discovers PCI Class 01 Subclass 06, maps ABAR MMIO, and accesses SATA hard disks (`sda`..`sdz`) via 48-bit DMA. |
| **MBR Partitioning** | Verified | `fdisk` lists (`-l`), creates (`create`), and deletes (`delete`) partitions; sets 1MB LBA alignment. |
| **ext4 Filesystem Read** | Verified | Custom driver reads ext4 superblocks, group descriptors, inode tables, extent trees (`0xF30A`), and directories. |
| **freestanding mkfs.ext4** | Verified | Pure C formatter creates compliant ext4 volumes with 4096-byte blocks, bitmaps, inode tables, and extent trees. |
| **freestanding mkfs.fat32**| Verified | Pure C formatter creates compliant FAT32 bootloader partitions with BPB, FSInfo, FAT tables, and LFN records. |
| **VFS CWD & Path Resolution**| Verified | Full working directory tracking (`vfs_cwd`), relative path canonicalization (`.`, `..`), `cd`, and `cp`. |
| **Preemptive Multitasking**| Verified | Preemptive Round-Robin scheduler interleaves background worker threads during timer ticks. |
| **GW-BASIC (1983)** | Verified | Microsoft GW-BASIC interpreter executes interactive statements and `.bas` programs (e.g. `fibonacci.bas`, `hello.bas`). |
| **Paratrooper (1982)** | Verified | Orion Software arcade game with 320x200 CGA upscaling, PIT PC Speaker audio, and full gameplay mechanics. |
| **Self-Installation Wizard**| Verified | `install` deploys dual-partition layout (FAT32 boot `sda1` + ext4 root `sda2`), writes Limine MBR, and hot-mounts `/`. |

---

## 2. Tested & Verified Environments

### 2.1 QEMU
- **BIOS Boot**: Verified with `qemu-system-x86_64 -cdrom antigravity-os.iso -drive file=target.img,format=raw,index=0,media=disk`.
- **Stand-alone Hard Disk Boot**: Verified booting directly from `target.img` without CD-ROM attached after running `install hda`.
- **Serial Debugging**: Verified clean bidirectional console communication via UART 16550 serial port (`-serial stdio`).

### 2.2 VMware Workstation / Player
- **Compatibility**: "Other Linux 64-bit" or "Other 64-bit" guest OS profile fully supported.
- **Disk Controller**: Supports both **SATA (AHCI)** (`sda`) and **IDE** (`hda`).
- **Triple Fault Resolution**: Resolved by partitioning disk with a 64MB FAT32 boot partition (`sda1`) hosting Limine Stage 2/3 BIOS bootloader files, and deploying the Linux ext4 root filesystem to `sda2`. Standalone hard disk boot directly from `.vmdk` verified.

### 2.3 Real Hardware: Raspberry Pi 3 Model B (AArch64)
- **Architecture**: AArch64 (ARMv8-A 64-bit).
- **Physical Boot Chain**: VideoCore GPU loads `bootcode.bin` and `start.elf` from FAT32 partition, reads `config.txt` (`arm_64bit=1`, `hdmi_force_hotplug=1`, `hdmi_drive=2`, `config_hdmi_boost=4`), and loads `kernel8.img` to `0x80000`.
- **Physical Display & Framebuffer**: Linear 1024x768 (32 BPP) framebuffer allocated via VideoCore Mailbox property tags; full HDMI video output physically verified on real Raspberry Pi 3 hardware.
- **Audio** (verified on Pi 3B HDMI, 2026-10-28): the old VCHIQ driver was a placeholder that never sent audio. Replaced by `sound_hdmi.c`: HDMI MAI FIFO fed by DMA channel 5 (DREQ 17) looping over a 100 ms buffer of IEC958 subframes at 48 kHz, N/CTS clock regeneration from the mailbox pixel clock, Audio InfoFrame in HDMI RAM packet 4. Register sequence follows Linux vc4_hdmi; HSM clock assumed 163.682864 MHz. Debug line `[AUDIO]` shows MAI_CTL / DMA state.
- **Input & Gamepad**: Paratrooper arcade game running with keyboard/serial input and Gamepad HAL.
- **Bluetooth BLE Subsystem** (verified on Pi 3B rev `0xA02082`, 2026-10-27): BCM43438 powered via mailbox BT_ON (expander GPIO 128) with 32 kHz LPO on GPIO 43, PL011 on GPIO 32/33 at 115200, patchram firmware upload, LE scan, Xbox controller detection (HID UUID 0x1812 / gamepad appearance 0x03C4) and LE connection all work. UART RX is IRQ-driven into a 4 KB ring (EL1 vector table in `boot.S`); unhandled CPU exceptions now print ESR/ELR/FAR on screen. Console moved to the mini-UART (GPIO 14/15).
- **BLE host** (verified on Pi 3B with Xbox Wireless Controller F4:6A:D7:DB:45:68, 2026-10-28): `ble_host.c` does SMP legacy Just Works pairing (software AES-128; c1/s1 checked against the Core spec test vectors), LE encryption, GATT discovery (Find Information table + Read By Type 0x2803), subscribes to every CCCD in the table (the controller has two input-report CCCDs, 0x15 and 0x1F, and most value types arrive as unrecognised 128-bit UUIDs), parses any notification of gamepad size. Buttons, sticks and triggers verified on the `[GAMEPAD]` debug row. Xbox button bits: View 10, Menu 11, Guide 12, LS 13, RS 14.
- **Open (BLE)**: (1) in pairing mode the controller light keeps flashing after pairing, although input works; after a normal power-on it reconnects with a solid light. Possibly it expects host identity keys (we distribute none). (2) Keys are RAM-only; there is no SD card driver on the Pi yet to persist them. (3) Paratrooper launch is disabled on the Pi (`LAUNCH_PARATROOPER 0` in `kmain.c`) while debugging; re-enable once BLE is settled. (4) The 20 M RX count is gone since scanning resumes with duplicate filtering.

---

## 3. Performance & Stability Milestones

1. **Sluggishness & Timer Resolution Resolved**:
   - PIT timer moved to Mode 2 (rate generator, `0x34`) at 1000 Hz (1ms per tick).
   - Framebuffer scrolling accelerated with 64-bit word transfers (`uint64_t`).
   - Idle loop utilizes `sched_yield()` and `pause` to prevent 100% CPU starvation while preserving immediate interrupt delivery across VMware and QEMU.
2. **Keyboard Mapping & Key Repeat Resolved**:
   - Fixed PS/2 Set 2 scan-code collision that previously blocked the `e` key and misaligned number keys.
   - Solved double-character registration (`aa` when typing `a`): Handled PS/2 Scan Code Set 2 break code prefix `0xF0` so that key release events are not misparsed as key presses.
   - Implemented `key_down[128]` state array and a 250ms typematic repeat debounce threshold.
   - Added continuous status port `0x64` polling fallback in `keyboard_has_char()`, ensuring keystrokes are never lost even when interrupts are briefly masked.
3. **VMware Boot & Limine Stage 2 Resolution**:
   - Discovered Limine BIOS Stage 2 binary only supports FAT32 and ISO9660 filesystems.
   - Designed and implemented a freestanding FAT32 formatter (`mkfs_fat32_boot_format`) in kernel C.
   - Upgraded installer to create a 64MB FAT32 boot partition (`sda1`) hosting Limine Stage 2/3 binaries, and an ext4 root partition (`sda2`) hosting the OS.
   - Verified 100% stable standalone boot on VMware Workstation from SATA AHCI disks (`sda`).
4. **Stack Overflow in mkfs Eliminated**:
   - Replaced multiple 4096-byte stack allocations inside `alloc_free_inode` and `alloc_free_blocks` with dedicated static buffers.
   - Expanded thread stack size from 16 KB to 64 KB.
5. **Cache Flush Error Handled**:
   - Removed unneeded ATA `0xE7` cache flush command on PIO transfers that caused virtual IDE status register `ERR` flags.
6. **GW-BASIC Expression Loop & `$var` Compatibility Resolved**:
   - Fixed an infinite loop in `PRINT` where unconsumed tokens (such as shell-style `$i`) did not advance the parse pointer, causing continuous `0` output.
   - Added expression parser safety guards preventing any zero-progress infinite loops.
   - Added support for leading `$` variable syntax (e.g. `$i` automatically resolves to loop variable `i`), along with nested stack unwinding for `NEXT [var]`.

---

## 4. Future Roadmap

- [x] **AHCI / SATA Driver**: Native PCI AHCI DMA driver implemented and verified, supporting SATA disks (`sda`..`sdz`) in VMware and modern PCs.
- [ ] **ELF Executable Loader**: Expand userland execution to load separate dynamic or static ELF binaries into independent user page tables with Ring 3 privilege transitions.
- [ ] **SMP / Multi-Core AP Boot**: Wake up secondary Application Processors via INIT-SIPI-SIPI IPIs.
- [ ] **ext4 File Write in Runtime**: Expand the runtime ext4 driver to support modifying and appending files dynamically through POSIX-style `vfs_write()`.
