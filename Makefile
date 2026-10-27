ARCH ?= x86_64

OUTPUT_DIR = output/$(ARCH)

# Common directories and tools
MKDIR_P = mkdir -p

ifeq ($(ARCH),x86_64)
    CC = gcc
    LD = ld
    OBJCOPY = objcopy

    CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector \
             -fno-stack-check -fno-lto -fno-pie -fno-pic -mno-80387 -mno-mmx \
             -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel \
             -Iinclude -I. -O2 -g

    LDFLAGS = -m elf_x86_64 -nostdlib -static -no-pie -z max-page-size=0x1000 -T linker.ld

    # Exclude other architectures (aarch64) from x86_64 build
    C_SOURCES = $(shell find kernel -name "*.c" ! -path "kernel/arch/aarch64/*")
    ASM_SOURCES = kernel/arch/x86_64/entry.S

    C_OBJECTS = $(C_SOURCES:.c=.o)
    ASM_OBJECTS = $(ASM_SOURCES:.S=.o)
    OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

    KERNEL_BIN = $(OUTPUT_DIR)/kernel.elf
    ISO_IMAGE = $(OUTPUT_DIR)/antigravity-os.iso
    DISK_IMAGE = $(OUTPUT_DIR)/disk.img

    all: $(KERNEL_BIN) $(ISO_IMAGE) $(DISK_IMAGE)

    $(KERNEL_BIN): $(OBJECTS) linker.ld
		@$(MKDIR_P) $(OUTPUT_DIR)
		@echo " [LD] $@"
		@$(LD) $(LDFLAGS) $(OBJECTS) -o $@

    $(ISO_IMAGE): $(KERNEL_BIN) limine.conf
		@$(MKDIR_P) $(OUTPUT_DIR)
		@echo " [ISO] Creating hybrid bootable ISO for x86_64..."
		@rm -rf iso_root
		@mkdir -p iso_root/boot iso_root/boot/limine iso_root/EFI/BOOT
		@cp $(KERNEL_BIN) iso_root/boot/kernel.elf
		@cp limine.conf iso_root/boot/limine/
		@cp limine-bin/limine-bios.sys iso_root/boot/limine/
		@cp limine-bin/limine-bios-cd.bin iso_root/boot/limine/
		@cp limine-bin/limine-uefi-cd.bin iso_root/boot/limine/
		@cp limine-bin/BOOTX64.EFI iso_root/EFI/BOOT/
		@cp limine-bin/BOOTIA32.EFI iso_root/EFI/BOOT/
		@xorriso -as mkisofs -R -r -J \
			-b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
			-hfsplus -apm-block-size 2048 \
			--efi-boot boot/limine/limine-uefi-cd.bin \
			-efi-boot-part --efi-boot-image --protective-msdos-label \
			iso_root -o $(ISO_IMAGE) > /dev/null 2>&1
		@./limine-bin/limine bios-install $(ISO_IMAGE) > /dev/null 2>&1
		@echo " [OK] $(ISO_IMAGE) created."

    $(DISK_IMAGE):
		@$(MKDIR_P) $(OUTPUT_DIR)
		@echo " [EXT4] Creating test ext4 disk image..."
		@dd if=/dev/zero of=$(DISK_IMAGE) bs=1M count=32 status=none
		@/usr/sbin/mkfs.ext4 -F -O ^metadata_csum $(DISK_IMAGE) > /dev/null 2>&1
		@echo "Welcome to Antigravity OS ext4 filesystem!" > /tmp/readme.txt
		@echo "This text was read directly from disk via the custom ext4 driver." > /tmp/hello.txt
		@printf "10 REM GW-BASIC TEST\n20 PRINT \"HELLO FROM GW-BASIC (1983)!\"\n30 FOR I = 1 TO 5\n40 PRINT \"COUNT: \"; I\n50 NEXT I\n60 PRINT \"SUCCESS!\"\n" > /tmp/hello.bas
		@printf "10 REM FIBONACCI\n20 A = 0 : B = 1\n30 PRINT A; B;\n40 FOR I = 3 TO 12\n50 C = A + B : PRINT C;\n60 A = B : B = C\n70 NEXT I\n80 PRINT \"\"\n" > /tmp/fibonacci.bas
		@debugfs -w -R "write /tmp/readme.txt readme.txt" $(DISK_IMAGE) > /dev/null 2>&1
		@debugfs -w -R "write /tmp/hello.txt hello.txt" $(DISK_IMAGE) > /dev/null 2>&1
		@debugfs -w -R "write /tmp/hello.bas hello.bas" $(DISK_IMAGE) > /dev/null 2>&1
		@debugfs -w -R "write /tmp/fibonacci.bas fibonacci.bas" $(DISK_IMAGE) > /dev/null 2>&1
		@debugfs -w -R "mkdir bin" $(DISK_IMAGE) > /dev/null 2>&1
		@printf "cd bin\nwrite $(KERNEL_BIN) gwbasic\nwrite $(KERNEL_BIN) paratrooper\n" | debugfs -w $(DISK_IMAGE) > /dev/null 2>&1
		@rm -f /tmp/readme.txt /tmp/hello.txt /tmp/hello.bas /tmp/fibonacci.bas
		@echo " [OK] $(DISK_IMAGE) created."

    run: $(ISO_IMAGE) $(DISK_IMAGE)
		qemu-system-x86_64 -boot d -cdrom $(ISO_IMAGE) -drive file=$(DISK_IMAGE),format=raw,index=0,media=disk -serial stdio -m 512M

else ifeq ($(ARCH),aarch64)
    CROSS_COMPILE ?= aarch64-linux-gnu-
    CC = $(CROSS_COMPILE)gcc
    LD = $(CROSS_COMPILE)ld
    OBJCOPY = $(CROSS_COMPILE)objcopy

    CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector \
             -fno-stack-check -fno-lto -fno-pie -fno-pic \
             -Iinclude -I. -O2 -g

    LDFLAGS = -nostdlib -static -no-pie -T kernel/arch/aarch64/linker.ld

    AARCH64_C_SOURCES = kernel/arch/aarch64/kmain.c \
                        kernel/arch/aarch64/serial.c \
                        kernel/arch/aarch64/mailbox.c \
                        kernel/arch/aarch64/timer.c \
                        kernel/arch/aarch64/sound_vchiq.c \
                        kernel/drivers/bluetooth/hci.c \
                        kernel/drivers/bluetooth/bcm43438.c \
                        kernel/drivers/bluetooth/bcm43430a1_hcd.c \
                        kernel/drivers/bluetooth/xbox_ble.c \
                        kernel/drivers/input/gamepad.c \
                        kernel/shell/paratrooper.c \
                        kernel/shell/paratrooper_data.c \
                        kernel/lib/printk.c \
                        kernel/lib/string.c \
                        kernel/lib/fb.c \
                        kernel/lib/font.c

    AARCH64_ASM_SOURCES = kernel/arch/aarch64/boot.S

    AARCH64_C_OBJECTS = $(AARCH64_C_SOURCES:.c=.arm.o)
    AARCH64_ASM_OBJECTS = $(AARCH64_ASM_SOURCES:.S=.arm.o)
    OBJECTS = $(AARCH64_C_OBJECTS) $(AARCH64_ASM_OBJECTS)

    KERNEL_ELF = $(OUTPUT_DIR)/kernel8.elf
    KERNEL_IMG = $(OUTPUT_DIR)/kernel8.img
    SD_IMAGE   = $(OUTPUT_DIR)/antigravity-rpi.img

    all: $(KERNEL_ELF) $(KERNEL_IMG) $(SD_IMAGE)

    $(KERNEL_ELF): $(OBJECTS) kernel/arch/aarch64/linker.ld
		@$(MKDIR_P) $(OUTPUT_DIR)
		@echo " [LD] $@"
		@$(LD) $(LDFLAGS) $(OBJECTS) -o $@

    $(KERNEL_IMG): $(KERNEL_ELF)
		@echo " [OBJCOPY] $@"
		@$(OBJCOPY) -O binary $< $@

    $(SD_IMAGE): $(KERNEL_IMG)
		@echo " [SD] Generating bootable Raspberry Pi disk image..."
		@dd if=/dev/zero of=$@ bs=1M count=64 status=none
		@parted -s $@ mklabel msdos
		@parted -s $@ mkpart primary fat32 2048s 100%
		@parted -s $@ set 1 boot on
		@mformat -i $@@@1048576 -F ::
		@printf "arm_64bit=1\nkernel=kernel8.img\nhdmi_force_hotplug=1\nhdmi_drive=2\nconfig_hdmi_boost=4\ndisable_overscan=1\nenable_uart=1\nuart_2ndstage=1\ncore_freq=250\ninit_uart_clock=48000000\n" > $(OUTPUT_DIR)/config.txt
		@mcopy -i $@@@1048576 $(KERNEL_IMG) ::kernel8.img
		@mcopy -i $@@@1048576 $(OUTPUT_DIR)/config.txt ::config.txt
		@if [ -d firmware-rpi ]; then \
			mcopy -i $@@@1048576 firmware-rpi/* :: ; \
			cp -f firmware-rpi/* $(OUTPUT_DIR)/ ; \
		fi
		@echo " [OK] $@ created."

    run: $(KERNEL_IMG)
		qemu-system-aarch64 -M raspi3 -kernel $(KERNEL_IMG) -serial null -serial stdio

    run-sd: $(SD_IMAGE)
		qemu-system-aarch64 -M raspi3 -sd $(SD_IMAGE) -serial null -serial stdio

else
    $(error Unsupported architecture '$(ARCH)'. Supported: x86_64, aarch64)
endif

%.arm.o: %.c
	@echo " [CC-ARM] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

%.arm.o: %.S
	@echo " [AS-ARM] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	@echo " [CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@echo " [AS] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(shell find kernel -name "*.o") $(shell find kernel -name "*.arm.o") iso_root output