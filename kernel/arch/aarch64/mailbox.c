#include <hal/aarch64/mailbox.h>
#include <hal/aarch64/arm.h>

#define MBOX_BASE       0x3F00B880
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20

#define MBOX_FULL       0x80000000
#define MBOX_EMPTY      0x40000000

bool mbox_call(uint8_t channel, volatile uint32_t *mbox) {
    // 28-bit address (16-byte aligned) + 4-bit channel
    uint32_t addr = (uint32_t)((uintptr_t)mbox & ~0xF) | (channel & 0xF);

    int timeout = 50000;
    while ((mmio_read32(MBOX_BASE + MBOX_STATUS) & MBOX_FULL) && --timeout > 0) {
        arm_delay(10);
    }
    if (timeout <= 0) return false;

    // Write address to Mailbox
    mmio_write32(MBOX_BASE + MBOX_WRITE, addr);

    // Wait for response with timeout
    timeout = 100000;
    while (--timeout > 0) {
        while ((mmio_read32(MBOX_BASE + MBOX_STATUS) & MBOX_EMPTY) && --timeout > 0) {
            arm_delay(10);
        }
        if (timeout <= 0) return false;

        uint32_t resp = mmio_read32(MBOX_BASE + MBOX_READ);
        if ((resp & 0xF) == channel && (resp & ~0xF) == ((uintptr_t)mbox & ~0xF)) {
            // Buffer status word: 0x80000000 = success
            return mbox[1] == 0x80000000;
        }
    }
    return false;
}
