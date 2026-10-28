#include <hal/aarch64/mailbox.h>
#include <hal/aarch64/arm.h>
#include <hal/timer.h>

#define MBOX_BASE       0x3F00B880
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20

#define MBOX_FULL       0x80000000
#define MBOX_EMPTY      0x40000000

// Timeouts use the millisecond timer, not delay loops: loop speed changes a lot once the
// caches are on (mmu.c). mbox must be a GPU_SHARED (uncached) buffer.
#define MBOX_TIMEOUT_MS 1000

bool mbox_call(uint8_t channel, volatile uint32_t *mbox) {
    // 28-bit address (16-byte aligned) + 4-bit channel
    uint32_t addr = (uint32_t)((uintptr_t)mbox & ~0xF) | (channel & 0xF);

    uint64_t start = timer_get_ticks();
    while (mmio_read32(MBOX_BASE + MBOX_STATUS) & MBOX_FULL) {
        if (timer_get_ticks() - start > MBOX_TIMEOUT_MS) return false;
    }

    // The request must be in memory before the GPU is told where it is
    __asm__ volatile("dsb sy" ::: "memory");
    mmio_write32(MBOX_BASE + MBOX_WRITE, addr);

    // Wait for our response
    start = timer_get_ticks();
    while (timer_get_ticks() - start <= MBOX_TIMEOUT_MS) {
        if (mmio_read32(MBOX_BASE + MBOX_STATUS) & MBOX_EMPTY) continue;

        uint32_t resp = mmio_read32(MBOX_BASE + MBOX_READ);
        if ((resp & 0xF) == channel && (resp & ~0xF) == ((uintptr_t)mbox & ~0xF)) {
            __asm__ volatile("dsb sy" ::: "memory");
            // Buffer status word: 0x80000000 = success
            return mbox[1] == 0x80000000;
        }
    }
    return false;
}
