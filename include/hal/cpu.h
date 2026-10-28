#ifndef HAL_CPU_H
#define HAL_CPU_H

#include <kernel/types.h>

#if defined(__aarch64__)

#define HAL_ARCH_NAME "aarch64"

static inline void cli(void) {
    __asm__ volatile ("msr daifset, #2" ::: "memory");
}

static inline void sti(void) {
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

static inline void hlt(void) {
    __asm__ volatile ("wfi");
}

// Spin-wait hint for busy loops
static inline void cpu_relax(void) {
    __asm__ volatile ("yield");
}

// Mask IRQs and return the previous mask state for irq_restore()
static inline uint64_t irq_save(void) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif; msr daifset, #2" : "=r"(daif) : : "memory");
    return daif;
}

static inline void irq_restore(uint64_t daif) {
    __asm__ volatile ("msr daif, %0" : : "r"(daif) : "memory");
}

static inline uint64_t rdtsc(void) {
    uint64_t val;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

#else // x86_64

#define HAL_ARCH_NAME "x86_64"

static inline void cpu_relax(void) {
    __asm__ volatile ("pause");
}

static inline uint64_t irq_save(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) : : "memory");
    return rflags;
}

static inline void irq_restore(uint64_t rflags) {
    if (rflags & 0x200) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline void cli(void) {
    __asm__ volatile ("cli" ::: "memory");
}

static inline void sti(void) {
    __asm__ volatile ("sti" ::: "memory");
}

static inline void hlt(void) {
    __asm__ volatile ("hlt");
}

static inline uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

static inline void write_cr0(uint64_t val) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(val) : "memory");
}

static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

static inline uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(val));
    return val;
}

static inline void write_cr4(uint64_t val) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(val) : "memory");
}

static inline void invlpg(uint64_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static inline uint64_t rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

#endif // __aarch64__

#endif // HAL_CPU_H
