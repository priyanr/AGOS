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
#include <shell/paratrooper.h>

// Stubs for freestanding AArch64 HAL
void sched_yield(void) {
    __asm__ volatile("yield");
}

bool keyboard_has_char(void) {
    return false;
}

char keyboard_getchar(void) {
    return 0;
}

// 16-byte aligned mailbox request buffer
static volatile uint32_t __attribute__((aligned(16))) mbox_buf[36];

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

void kmain_aarch64(void) {
    serial_init();
    timer_init(1000);

    printk("\n===================================================\n");
    printk("   Antigravity OS - AArch64 (Raspberry Pi Edition)  \n");
    printk("   Target: ARMv8-A (Pi 3 / Pi 4 / QEMU raspi3b)    \n");
    printk("===================================================\n\n");

    printk("[BOOT] Initializing VideoCore Mailbox Framebuffer...\n");
    bool has_fb = rpi_init_framebuffer(1024, 768);
    if (has_fb) {
        printk("[OK] Framebuffer allocated: 1024x768 (32bpp)\n");
        fb_clear(0x000F172A);
        fb_write("Antigravity OS (Raspberry Pi 64-bit)\n");
        fb_write("Unified Gamepad & BLE Bluetooth Subsystem Active.\n\n");
    } else {
        printk("[WARN] Headless mode: Framebuffer allocation bypassed.\n");
    }

    printk("[INIT] Initializing unified Gamepad HAL...\n");
    gamepad_init();

    printk("[INIT] Initializing Bluetooth HCI & Xbox BLE driver...\n");
    bluetooth_init();
    bcm43438_init();

    printk("[INIT] Initializing Sound Driver (VCHIQ HDMI Audio)...\n");
    sound_init();

    printk("\n[SYSTEM] AArch64 Kernel ready. Launching Paratrooper...\n");
    if (has_fb) {
        cmd_paratrooper(0, NULL);
    }

    while (1) {
        bcm43438_poll();
        if (serial_received()) {
            char c = serial_getchar();
            serial_putchar(c);
        }
        arm_delay(1000);
    }
}
