#ifndef MM_PMM_H
#define MM_PMM_H

#include <kernel/types.h>

// A range of RAM the kernel may allocate from (the boot code builds the list: Limine memory
// map on x86, the firmware's ARM memory split on the Pi)
typedef struct {
    paddr_t base;
    uint64_t length;
} pmm_region_t;

// The bitmap is carved from the first region large enough; regions may be modified
void pmm_init(pmm_region_t *regions, size_t count, uint64_t hhdm_offset);
paddr_t pmm_alloc_page(void);
paddr_t pmm_alloc_pages(size_t count);
void pmm_free_page(paddr_t addr);
void pmm_free_pages(paddr_t addr, size_t count);

uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_used_memory(void);

#endif // MM_PMM_H
