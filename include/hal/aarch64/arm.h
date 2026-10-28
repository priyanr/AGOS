#ifndef HAL_AARCH64_ARM_H
#define HAL_AARCH64_ARM_H

#include <kernel/types.h>

// Place a buffer the GPU or a DMA engine reads/writes directly (mailbox messages, DMA control
// blocks and data) in the uncached block set up by mmu.c, so CPU caches never hide it
#define GPU_SHARED __attribute__((section(".bss.gpu_shared")))

// Identity-mapped MMU with caches on (mmu.c): RAM write-back, GPU memory and GPU_SHARED
// uncached, peripherals Device. Call once, early, before the framebuffer is set up.
void mmu_init(void);

static inline void mmio_write32(uintptr_t reg, uint32_t val) {
    *(volatile uint32_t *)reg = val;
}

static inline uint32_t mmio_read32(uintptr_t reg) {
    return *(volatile uint32_t *)reg;
}

static inline void arm_delay(int32_t count) {
    __asm__ volatile(
        "1: subs %w0, %w0, #1; bne 1b"
        : "+r"(count) : : "cc"
    );
}

static inline void enable_interrupts(void) {
    __asm__ volatile("msr daifclr, #2");
}

static inline void disable_interrupts(void) {
    __asm__ volatile("msr daifset, #2");
}

static inline uint64_t get_cntpct(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

static inline uint32_t get_cntfrq(void) {
    uint32_t val;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

#endif // HAL_AARCH64_ARM_H
