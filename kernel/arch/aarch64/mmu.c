#include <hal/aarch64/arm.h>
#include <hal/aarch64/mailbox.h>
#include <kernel/printk.h>

// Identity-mapped MMU so the data and instruction caches can be used. With the MMU off every
// data access is Device-nGnRnE: no cache and no write combining, which made Paratrooper's
// 1024x768 frame upscale far slower than a frame tick.
//
// 4 KB granule, 32-bit VA (T0SZ = 32, lookup starts at level 1):
//   L1[0] -> L2 table of 2 MB blocks for 0..1 GB
//            ARM RAM                          Normal write-back (cached)
//            GPU_SHARED block (linker.ld)     Normal non-cacheable
//            GPU RAM (framebuffer), above ARM Normal non-cacheable (writes still combine)
//            0x3F000000.. peripherals         Device-nGnRE
//   L1[1] -> 1 GB Device block at 0x40000000 (ARM local peripherals)

#define MAIR_DEVICE     0 // Device-nGnRE
#define MAIR_NC         1 // Normal, non-cacheable
#define MAIR_WB         2 // Normal, write-back read/write-allocate

#define DESC_BLOCK      0x1ull
#define DESC_TABLE      0x3ull
#define DESC_AF         (1ull << 10)
#define DESC_INNER_SH   (3ull << 8)
#define DESC_PXN        (1ull << 53)
#define DESC_UXN        (1ull << 54)

#define BLOCK_2MB       0x200000ull
#define PERIPHERAL_BASE 0x3F000000ull

extern char __gpu_shared_start[];
extern char __gpu_shared_end[];

static uint64_t __attribute__((aligned(4096))) l1_table[512];
static uint64_t __attribute__((aligned(4096))) l2_table[512];
static volatile uint32_t __attribute__((aligned(16))) GPU_SHARED mmu_mbox[8];

static uint64_t block(uint64_t addr, uint64_t attr_index) {
    uint64_t desc = addr | DESC_AF | (attr_index << 2) | DESC_BLOCK;
    if (attr_index == MAIR_DEVICE) {
        desc |= DESC_PXN | DESC_UXN; // never execute from (or speculatively fetch) device memory
    } else {
        desc |= DESC_INNER_SH;
    }
    return desc;
}

// End of the RAM the firmware gives the ARM; the GPU owns the rest up to the peripherals
static uint64_t arm_ram_end(void) {
    mmu_mbox[0] = 8 * 4;
    mmu_mbox[1] = 0;
    mmu_mbox[2] = MBOX_TAG_GET_ARM_MEMORY;
    mmu_mbox[3] = 8;
    mmu_mbox[4] = 0;
    mmu_mbox[5] = 0;
    mmu_mbox[6] = 0;
    mmu_mbox[7] = 0;
    if (mbox_call(MBOX_CH_PROP, mmu_mbox) && mmu_mbox[6] != 0) {
        return (uint64_t)mmu_mbox[5] + mmu_mbox[6];
    }
    return 0x3C000000ull; // Default split: 64 MB GPU memory
}

void mmu_init(void) {
    uint64_t ram_end = arm_ram_end();
    uint64_t shared_start = (uint64_t)(uintptr_t)__gpu_shared_start;
    uint64_t shared_end = (uint64_t)(uintptr_t)__gpu_shared_end;

    for (uint64_t i = 0; i < 512; i++) {
        uint64_t addr = i * BLOCK_2MB;
        if (addr >= PERIPHERAL_BASE) {
            l2_table[i] = block(addr, MAIR_DEVICE);
        } else if (addr + BLOCK_2MB > ram_end || (addr >= shared_start && addr < shared_end)) {
            l2_table[i] = block(addr, MAIR_NC);
        } else {
            l2_table[i] = block(addr, MAIR_WB);
        }
    }
    for (int i = 0; i < 512; i++) l1_table[i] = 0;
    l1_table[0] = (uint64_t)(uintptr_t)l2_table | DESC_TABLE;
    l1_table[1] = (0x40000000ull | DESC_AF | (MAIR_DEVICE << 2) | DESC_PXN | DESC_UXN | DESC_BLOCK);

    uint64_t mair = (0x04ull << (8 * MAIR_DEVICE)) | (0x44ull << (8 * MAIR_NC)) | (0xFFull << (8 * MAIR_WB));
    uint64_t tcr = 32ull              // T0SZ: 32-bit VA
                 | (1ull << 8)        // IRGN0: write-back read/write-allocate
                 | (1ull << 10)       // ORGN0: write-back read/write-allocate
                 | (3ull << 12)       // SH0: inner shareable
                 | (0ull << 14)       // TG0: 4 KB granule
                 | (1ull << 23);      // EPD1: no TTBR1 walks
                                      // IPS = 0: 32-bit physical addresses

    __asm__ volatile(
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "dsb sy\n"
        "isb\n"
        "tlbi vmalle1\n"
        "ic iallu\n"
        "dsb sy\n"
        "isb\n"
        :
        : "r"(mair), "r"(tcr), "r"((uint64_t)(uintptr_t)l1_table)
        : "memory");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 0)    // M: MMU on
           | (1ull << 2)    // C: data cache
           | (1ull << 12);  // I: instruction cache
    sctlr &= ~(1ull << 1);  // A: no alignment faults on normal memory
    __asm__ volatile("msr sctlr_el1, %0\nisb" : : "r"(sctlr) : "memory");

    printk("[MMU] Caches on. ARM RAM end 0x%08X, GPU-shared block 0x%08X-0x%08X\n",
           (unsigned int)ram_end, (unsigned int)shared_start, (unsigned int)shared_end);
}
