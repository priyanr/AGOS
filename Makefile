CC = gcc
LD = ld

CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector \
         -fno-stack-check -fno-lto -fno-pie -fno-pic -mno-80387 -mno-mmx \
         -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel \
         -Iinclude -I. -O2 -g

LDFLAGS = -m elf_x86_64 -nostdlib -static -no-pie -z max-page-size=0x1000 -T linker.ld

C_SOURCES = $(shell find kernel -name "*.c")
ASM_SOURCES = $(shell find kernel -name "*.S")

C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.S=.o)
OBJECTS = $(C_OBJECTS) $(ASM_OBJECTS)

ISO_IMAGE = antigravity-os.iso
DISK_IMAGE = disk.img

all: kernel.elf $(ISO_IMAGE) $(DISK_IMAGE)

kernel.elf: $(OBJECTS) linker.ld
	@echo " [LD] $@"
	@$(LD) $(LDFLAGS) $(OBJECTS) -o $@

%.o: %.c
	@echo " [CC] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@echo " [AS] $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(ISO_IMAGE): kernel.elf limine.conf
	@echo " [ISO] Creating bootable hybrid ISO..."
	@rm -rf iso_root
	@mkdir -p iso_root/boot
	@mkdir -p iso_root/boot/limine
	@mkdir -p iso_root/EFI/BOOT
	@cp kernel.elf iso_root/boot/
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
	@echo " [OK] $(ISO_IMAGE) built successfully."

$(DISK_IMAGE):
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
	@printf "cd bin\nwrite kernel.elf gwbasic\nwrite kernel.elf paratrooper\n" | debugfs -w $(DISK_IMAGE) > /dev/null 2>&1
	@rm -f /tmp/readme.txt /tmp/hello.txt /tmp/hello.bas /tmp/fibonacci.bas

run: $(ISO_IMAGE) $(DISK_IMAGE)
	qemu-system-x86_64 -boot d -cdrom $(ISO_IMAGE) -drive file=$(DISK_IMAGE),format=raw,index=0,media=disk -serial stdio -m 512M

clean:
	@rm -rf $(OBJECTS) kernel.elf $(ISO_IMAGE) $(DISK_IMAGE) iso_root
