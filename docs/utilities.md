# Antigravity OS Built-in Utilities

Antigravity OS features a set of core built-in command-line utilities accessible directly through the interactive kernel shell and serial console. This guide documents their syntax, operational details, flags, and usage examples.

---

## 1. Storage & Filesystem Utilities

### 1.1 `fdisk` — MBR Partition Table Utility
Manages Master Boot Record (MBR) partition tables on physical storage disks.

#### Syntax
```bash
fdisk -l
fdisk create <disk> [size_mb]
fdisk delete <disk> <part_num>
```

#### Commands & Arguments
- `-l`: Scans and lists all attached block storage disks (`hda`, `hdb`, etc.) along with their capacity, sector size, and any detected MBR partition entries.
- `create <disk> [size_mb]`: Creates an MBR partition table on `<disk>` with an active bootable Linux ext4 partition (`hda1`).
  - Sets 1MB LBA alignment (starts at LBA 2048).
  - Assigns partition type `0x83` (Linux ext4).
  - Marks boot indicator `0x80` (Active/Bootable).
  - If `[size_mb]` is omitted, allocates the entire remaining disk capacity.
- `delete <disk> <part_num>`: Deletes partition entry `<part_num>` (1 through 4) and zeros out its partition entry in the MBR.

#### Example
```bash
root@antigravity:/# fdisk -l
[DISK] hda: 64 MB (131072 sectors, sector size 512 bytes)
  (No partition table found - raw unpartitioned disk)

root@antigravity:/# fdisk create hda
fdisk: Creating MBR partition 1 on hda...
  Start LBA:    2048
  Sectors:      129024 (63 MB)
  Type:         0x83 (Linux ext4)
  Bootable:     Yes
Partition: hda1 online (Start LBA: 2048, Size: 63 MB, Type: 0x83 [Bootable])
fdisk: Partition hda1 created successfully!
```

---

### 1.2 `mkfs.ext4` / `mkfs` — ext4 Filesystem Formatter
Formats a raw storage disk or partition block device with a modern ext4 filesystem.

#### Syntax
```bash
mkfs.ext4 <device>
# or
mkfs <device>
```

#### Operational Details
- **Block Size**: 4096 bytes per filesystem block (`s_log_block_size = 2`).
- **Superblock**: Placed at byte offset 1024 inside Block 0, configured with magic `0xEF53` and `EXT4_FEATURE_RO_COMPAT_GDT_CSUM` / `EXT4_FEATURE_INCOMPAT_EXTENTS`.
- **Group Descriptors**: Written to Block 1.
- **Bitmaps**: Block bitmap allocated at Block 2; Inode bitmap allocated at Block 3.
- **Inode Table**: Blocks 4 through 67 (allocating 1024 inodes of 128 bytes each).
- **Default Inodes**:
  - Inode 2: Root Directory (`/`), pointing to extent tree in Block 68 with default `.` and `..` entries.
  - Inode 11: System `lost+found` directory, pointing to Block 69.
- **Auto-mount Verification**: Automatically attempts a test mount on completion to verify filesystem integrity.

#### Example
```bash
root@antigravity:/# mkfs.ext4 hda1
mkfs.ext4: Formatting hda1 (63 MB, 16128 blocks of 4096 bytes)...
mkfs.ext4: Format complete! Filesystem initialized with 16093 free blocks.
mkfs.ext4: Validating filesystem mount...
ext4: Filesystem mounted successfully! Block size: 4096 bytes, Block groups: 1
[OK] ext4 filesystem on 'hda1' verified and mounted successfully!
```

---

### 1.3 `install` — Native OS Installation Wizard
Deploys Antigravity OS directly from the live boot environment onto a target hard drive or partition, setting up the bootloader and files so the disk boots independently.

#### Syntax
```bash
install [target_device]
```

#### Installation Steps Automated
1. **Target Selection**: If no argument is provided, discovers all physical disks and prompts for selection.
2. **Auto-Partitioning**: If a physical disk (e.g. `hda`) is selected and has no partition table, automatically creates an MBR partition table with `hda1`.
3. **Partition Layout**: Prepares a dual-partition layout:
   - **Partition 1 (`sda1`)**: 64 MB FAT32 Bootloader Partition (Type `0x0C`, Bootable) hosting Limine Stage 2/3 and configuration.
   - **Partition 2 (`sda2`)**: Linux ext4 Root Filesystem Partition (Type `0x83`) occupying the rest of the disk.
4. **Bootloader Formatting (Step 1/4)**: Formats partition 1 with FAT32 and deploys `/limine.conf`, `/limine-bios.sys`, and `/boot/kernel.elf`.
5. **Root Filesystem Formatting (Step 2/4)**: Formats partition 2 with ext4 and creates directory structure (`/bin`, `/boot`, `/etc`).
6. **Payload Deployment (Step 3/4)**:
   - Deploys `/boot/kernel.elf` directly from Limine memory.
   - Installs `/bin/gwbasic`.
   - Writes documentation and sample programs (`/readme.txt`, `/hello.txt`, `/hello.bas`, `/fibonacci.bas`, `/etc/os-release`).
7. **Bootloader Installation (Step 4/4)**: Writes the Limine MBR bootsector into Sector 0 and stage 2 into Sector 8 of the physical drive.
8. **Hot-Mount**: Mounts the installed ext4 partition immediately as the root filesystem `/`.

#### Example
```bash
root@antigravity:/# install sda
=======================================================
           Antigravity OS Installation Wizard          
=======================================================
Preparing partition layout on sda...
  - Partition 1 (sda1): 64 MB FAT32 Bootloader Partition
  - Partition 2 (sda2): Linux ext4 Root Filesystem Partition
Partition: sda1 online (Start LBA: 2048, Size: 64 MB, Type: 0x0c [Bootable])
Partition: sda2 online (Start LBA: 133120, Size: 191 MB, Type: 0x83)
[OK] Partitions sda1 (64 MB) and sda2 (191 MB) created.

Step 1/4: Formatting sda1 as FAT32 Boot Partition...
  -> Limine BIOS stage 2/3 bootloader deployed to sda1.

Step 2/4: Formatting sda2 with ext4 Root Filesystem...
mkfs.ext4: Formatting sda2 (191 MB, 48896 blocks of 4096 bytes)...
mkfs.ext4: Format complete! Filesystem initialized with 48858 free blocks.

Step 3/4: Deploying Antigravity OS binaries & files to sda2...
  -> Created /bin, /boot, /etc
  -> Installing /boot/kernel.elf (406 KB)...
  -> Installing /bin/gwbasic (406 KB)...

Step 4/4: Installing Limine MBR bootloader on sda...
  -> Limine MBR bootloader written to sector 0 of sda.

Mounting installed system on '/'...
ext4: Filesystem mounted successfully! Block size: 4096 bytes, Block groups: 2
[OK] Root filesystem '/' switched to sda2.
=======================================================
         Installation Completed Successfully!          
=======================================================
```

---

## 2. Programming & Applications

### 2.1 `gwbasic` — Microsoft GW-BASIC (1983) Interpreter
A fully functional implementation of the classic Microsoft GW-BASIC interpreter.

#### Syntax
```bash
gwbasic [file.bas]
# or
/bin/gwbasic [file.bas]
```

#### Features Supported
- **Direct Interactive Mode**: When launched without arguments, enters the `GW-BASIC (1983)` prompt where commands can be executed immediately or numbered program lines entered.
- **Script File Execution**: When given a filename argument (e.g. `gwbasic /hello.bas`), loads the BASIC script from the ext4 filesystem, executes it, and returns cleanly to the shell.
- **Keywords**: `PRINT`, `LET`, `IF...THEN`, `FOR...TO...STEP...NEXT`, `GOTO`, `GOSUB...RETURN`, `REM`, `INPUT`, `CLS`, `LIST`, `RUN`, `END`.
- **Preinstalled Samples**:
  - `/hello.bas`: Verification script displaying greeting loops.
  - `/fibonacci.bas`: Calculates and displays the first 12 numbers in the Fibonacci sequence.

#### Example
```bash
root@antigravity:/# gwbasic /fibonacci.bas
0 1 1 2 3 5 8 13 21 34 55 89
```

---

### 2.2 `paratrooper` — Paratrooper (1982) Arcade Game
A faithful, native freestanding port of Orion Software's legendary IBM PC game **PARATROOPER** (1982, by Greg Kuperberg).

#### Syntax
```bash
paratrooper
# or
/bin/paratrooper
```

#### Operational Details
- **Authentic CGA Graphics**: Emulates 320x200 4-color CGA Mode 4 (Palette 1 intensified: Black, Cyan, Magenta, White) with 2x or 3x integer scaling dynamically centered on the linear framebuffer.
- **Embedded Binary Assets**: Contains the original 1982 sprite graphics, animation tables, title banner, score digits, and font data compiled directly into the kernel.
- **Real-Time PC Speaker Synthesis**: Direct hardware control of PIT Channel 2 (ports `0x42`, `0x43`, and `0x61`) producing authentic sound effects: rotor thump, anti-aircraft gun blasts, bomb whistles, explosions, and opening/ending tunes.
- **Game Mechanics**:
  - Two helicopter lanes with randomized spawn intervals and flight directions.
  - Parachutists jumping from helicopters with probabilistic chute deployment and free-fall physics.
  - Splat physics: parachutists falling without chutes crash onto open ground or crush landed troopers below.
  - Four-trooper base takeover: when four troopers successfully land on either side of the turret, they advance towards the base, climb onto one another, and detonate the gun.
  - High-speed jet waves dropping aimed bombs.
  - Progressive waves with increasing difficulty and persistent high-score tracking.

#### Controls
| Key | Action |
| :--- | :--- |
| `Space` / `Enter` | Start game / Skip intro music |
| `Left Arrow` / `4` / `A` | Start rotating turret counter-clockwise |
| `Right Arrow` / `6` / `D` | Start rotating turret clockwise |
| `Up Arrow` / `8` / `W` / `Space` / `5` | Stop turret rotation and fire |
| `I` | Display instructions screen |
| `S` (or `Ctrl+S`) | Toggle sound effects on / off |
| `Esc` / `Q` | Abandon game and return to OS shell |

---

## 3. Filesystem & Navigation Utilities

### 3.1 `cd` — Change Directory
Changes the current working directory of the shell environment.

#### Syntax
```bash
cd [path]
```

#### Operational Details
- Supports absolute paths (e.g. `cd /bin`) and relative paths (e.g. `cd bin`, `cd ..`).
- Supports `~` shorthand to navigate back to root `/`.
- Resolves `.` and `..` canonical path segments across arbitrary directory nesting.
- Dynamically updates the shell prompt to reflect the active working directory: `root@antigravity:<cwd># `.

#### Example
```bash
root@antigravity:/# cd bin
root@antigravity:/bin# ls
[DIR]  .
[DIR]  ..
[FILE] gwbasic
root@antigravity:/bin# cd ..
root@antigravity:/# 
```

---

### 3.2 `ls` — Directory Listing
Lists files and subdirectories stored on the mounted ext4 filesystem.

#### Syntax
```bash
ls [path]
```

#### Operational Details
- When invoked without arguments, lists contents of the current working directory (`.`).
- Resolves relative and absolute directory paths.

#### Example
```bash
root@antigravity:/# ls
[FILE] hello.bas
[FILE] fibonacci.bas
[FILE] readme.txt
[FILE] hello.txt
[DIR]  bin
[DIR]  boot
[DIR]  etc
```

---

### 3.3 `cat` — Display File Content
Reads and outputs the contents of a text file from the filesystem.

#### Syntax
```bash
cat <filepath>
```

#### Example
```bash
root@antigravity:/# cat /etc/os-release
NAME="Antigravity OS"
VERSION="1.0.0"
ID=antigravity
PRETTY_NAME="Antigravity OS 1.0.0 (x86_64)"
```

---

### 3.4 `cp` — Copy File
Copies an existing file to a new destination file or into a target directory on the ext4 filesystem.

#### Syntax
```bash
cp <source> <destination>
```

#### Operational Details
- **Directory Target**: If `<destination>` is an existing directory, the source file is copied inside that directory retaining its original filename.
- **New File Target**: If `<destination>` specifies a new file path or name, the content is copied to the specified location.
- **Ext4 Allocation**: Allocates new ext4 blocks and inodes, writes extent records (`0xF30A`), and automatically updates directory entries.
- Overwriting an existing destination filename updates the directory entry to point to the newly allocated inode.

#### Example
```bash
root@antigravity:/# cp readme.txt copy_readme.txt
root@antigravity:/# cp hello.txt /bin
root@antigravity:/# cd /bin
root@antigravity:/bin# ls
[DIR]  .
[DIR]  ..
[FILE] gwbasic
[FILE] hello.txt
root@antigravity:/bin# cat hello.txt
Hello from Antigravity OS!
```

---

## 4. Diagnostics & Hardware Control

### 4.1 `lspci` — Enumerate PCI Hardware
Scans all 256 PCI buses and 32 devices per bus, dumping hardware vendor IDs, device IDs, class codes, subclass codes, and base address registers (BARs).

#### Syntax
```bash
lspci
```

---

### 4.2 `meminfo` — Physical Memory Diagnostics
Queries the Physical Memory Manager (PMM) and reports total, used, and free RAM in megabytes.

#### Syntax
```bash
meminfo
```

#### Example
```bash
root@antigravity:/# meminfo
Memory Information:
  Total Physical RAM: 125 MB
  Used Memory:        6 MB
  Free Memory:        119 MB
```

---

### 4.3 `ps` — Process & Thread Viewer
Displays current running processes and thread IDs under the preemptive multitasking scheduler.

#### Syntax
```bash
ps
```

---

### 4.4 `threads` — Preemptive Worker Demonstration
Spawns two preemptible background worker threads (`Thread-A` and `Thread-B`) that run concurrently with the interactive shell, demonstrating hardware timer preemption and round-robin task switching.

#### Syntax
```bash
threads
```

---

### 4.5 `uname` — System & Uptime Information
Displays the kernel build release, CPU architecture, timer frequency, and system uptime in milliseconds and seconds.

#### Syntax
```bash
uname
```

---

### 4.6 `clear` — Clear Framebuffer
Clears the linear graphical framebuffer to solid black and resets the terminal cursor back to `(0, 0)`.

#### Syntax
```bash
clear
```

---

### 4.7 `help` — Shell Help Command
Lists all available commands and brief descriptions.

#### Syntax
```bash
help
```

---

### 4.8 `reboot` — Hardware Restart
Triggers a machine restart via the PS/2 keyboard controller pulse reset line (`outb(0x64, 0xFE)`).

#### Syntax
```bash
reboot
```
