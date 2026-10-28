#ifndef MM_VMM_H
#define MM_VMM_H

#include <kernel/types.h>

// Address spaces as the scheduler sees them. x86_64: 4-level paging with the Limine higher-half
// direct map (kernel/arch/x86_64/vmm.c). AArch64: one identity map shared by every thread
// (kernel/arch/aarch64/mmu.c).

#if defined(__aarch64__)

typedef struct {
    paddr_t ttbr0; // Translation table base; all threads share the kernel identity map
} address_space_t;

#else // x86_64

#include <hal/idt.h>

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

typedef uint64_t *page_table_t;

typedef struct {
    paddr_t pml4_phys;
    page_table_t pml4_virt;
} address_space_t;

void vmm_init(uint64_t hhdm_offset);
bool vmm_map_page(page_table_t pml4, vaddr_t vaddr, paddr_t paddr, uint64_t flags);
void vmm_unmap_page(page_table_t pml4, vaddr_t vaddr);
paddr_t vmm_virt_to_phys(page_table_t pml4, vaddr_t vaddr);
void vmm_destroy_address_space(address_space_t *as);
void vmm_page_fault_handler(interrupt_frame_t *frame);

#endif // __aarch64__

address_space_t vmm_create_address_space(void);
void vmm_switch_address_space(address_space_t *as);
address_space_t *vmm_get_kernel_address_space(void);

uint64_t vmm_get_hhdm(void);
void *vmm_phys_to_virt(paddr_t phys);

#endif // MM_VMM_H
