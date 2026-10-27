#ifndef MM_PMM_H
#define MM_PMM_H

#include <kernel/types.h>
#include <limine.h>

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);
paddr_t pmm_alloc_page(void);
paddr_t pmm_alloc_pages(size_t count);
void pmm_free_page(paddr_t addr);
void pmm_free_pages(paddr_t addr, size_t count);

uint64_t pmm_get_total_memory(void);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_used_memory(void);

#endif // MM_PMM_H
