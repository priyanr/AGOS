#include <kernel/types.h>
#include <kernel/printk.h>
#include <kernel/fb.h>
#include <hal/serial.h>
#include <hal/timer.h>
#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>
#include <drivers/bluetooth.h>
#include <drivers/gamepad.h>
#include <drivers/sound.h>
#include <hal/platform.h>
#include <drivers/block.h>
#include <drivers/usb.h>
#include <mm/pmm.h>
#include <mm/heap.h>

extern char _end[]; // linker.ld: end of the kernel image including BSS

// Called from exc_unhandled in boot.S for any exception other than an EL1 IRQ; never returns
void exception_report(uint64_t esr, uint64_t elr, uint64_t far) {
    char line[96];
    snprintf(line, sizeof(line), "!! CPU EXCEPTION  ESR=0x%08X  ELR=0x%08X  FAR=0x%08X",
             (unsigned int)esr, (unsigned int)elr, (unsigned int)far);
    printk("\n%s\n", line);
    fb_draw_string_at(0, 7, line, 0x00FFFFFF, 0x00B91C1C); // White on red, below the BLE/audio debug lines
}

// 16-byte aligned mailbox request buffer
static volatile uint32_t __attribute__((aligned(16))) GPU_SHARED mbox_buf[36];

// The firmware starts the ARM at its low idle clock (600 MHz on a Pi 3); ask for the maximum
static void rpi_set_max_arm_clock(void) {
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00030004; // GET_MAX_CLOCK_RATE
    mbox_buf[3] = 8;
    mbox_buf[4] = 0;
    mbox_buf[5] = 3;          // ARM clock
    mbox_buf[6] = 0;
    mbox_buf[7] = 0;
    if (!mbox_call(MBOX_CH_PROP, mbox_buf) || mbox_buf[6] == 0) return;
    uint32_t max_hz = mbox_buf[6];

    mbox_buf[0] = 9 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00038002; // SET_CLOCK_RATE
    mbox_buf[3] = 12;
    mbox_buf[4] = 0;
    mbox_buf[5] = 3;          // ARM clock
    mbox_buf[6] = max_hz;
    mbox_buf[7] = 0;          // Do not skip turbo
    mbox_buf[8] = 0;
    if (mbox_call(MBOX_CH_PROP, mbox_buf)) {
        printk("[CPU] ARM clock set to %d MHz\n", (int)(mbox_buf[6] / 1000000));
    }
}

static bool rpi_init_framebuffer(uint32_t width, uint32_t height) {
    mbox_buf[0] = 35 * 4;
    mbox_buf[1] = 0; // Request code

    mbox_buf[2] = MBOX_TAG_SET_PHYS_WH;
    mbox_buf[3] = 8;
    mbox_buf[4] = 8;
    mbox_buf[5] = width;
    mbox_buf[6] = height;

    mbox_buf[7] = MBOX_TAG_SET_VIRT_WH;
    mbox_buf[8] = 8;
    mbox_buf[9] = 8;
    mbox_buf[10] = width;
    mbox_buf[11] = height;

    mbox_buf[12] = MBOX_TAG_SET_DEPTH;
    mbox_buf[13] = 4;
    mbox_buf[14] = 4;
    mbox_buf[15] = 32; // 32 bpp

    mbox_buf[16] = MBOX_TAG_ALLOC_FB;
    mbox_buf[17] = 8;
    mbox_buf[18] = 8;
    mbox_buf[19] = 16; // 16-byte alignment
    mbox_buf[20] = 0;  // Will receive pointer

    mbox_buf[21] = MBOX_TAG_GET_PITCH;
    mbox_buf[22] = 4;
    mbox_buf[23] = 4;
    mbox_buf[24] = 0;  // Will receive pitch

    mbox_buf[25] = 0;  // End tag

    if (mbox_call(MBOX_CH_PROP, mbox_buf)) {
        uint64_t fb_ptr = mbox_buf[19] & 0x3FFFFFFF; // Bus address to Physical address
        uint64_t pitch = mbox_buf[24];
        if (fb_ptr != 0 && pitch != 0) {
            fb_init(fb_ptr, width, height, pitch, 32);
            return true;
        }
    }
    return false;
}

// Entry from boot.S: Pi HAL, memory and drivers, then the shared kernel start-up
void kmain_aarch64(void) {
    serial_init();
    timer_init(1000); // Counter-based ms clock plus the 1 kHz scheduler tick
    mmu_init();
    rpi_set_max_arm_clock();

    printk("\n===================================================\n");
    printk("   Antigravity OS - AArch64 (Raspberry Pi Edition)  \n");
    printk("   Target: ARMv8-A (Pi 3 / QEMU raspi3)            \n");
    printk("===================================================\n\n");

    printk("[BOOT] Initializing VideoCore Mailbox Framebuffer...\n");
    if (rpi_init_framebuffer(1024, 768)) {
        printk("[OK] Framebuffer allocated: 1024x768 (32bpp)\n");
        fb_clear(0x00000000);
        fb_write("Antigravity OS (Raspberry Pi 64-bit)\n\n");
    } else {
        printk("[WARN] Headless mode: Framebuffer allocation bypassed.\n");
    }

    // All RAM above the kernel image up to the GPU split is free (the boot stack is below
    // 0x80000, the uncached GPU_SHARED block is inside the image)
    static pmm_region_t ram;
    ram.base = PAGE_ALIGN_UP((uint64_t)(uintptr_t)_end);
    ram.length = rpi_arm_ram_end() - ram.base;
    pmm_init(&ram, 1, 0);
    heap_init();

    gamepad_init();
    printk("[INIT] Bluetooth (BCM43438) and Xbox controller...\n");
    bluetooth_init();
    bcm43438_init();

    printk("[INIT] HDMI audio...\n");
    sound_init();

    printk("[INIT] SD card...\n");
    sd_init();

    printk("[INIT] USB host and keyboard...\n");
    usb_init();

    kernel_start();
}
