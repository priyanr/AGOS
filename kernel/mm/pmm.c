#include <mm/pmm.h>
#include <kernel/string.h>
#include <kernel/printk.h>

static uint8_t *bitmap = NULL;
static size_t bitmap_size = 0;
static uint64_t max_pages = 0;
static uint64_t total_memory = 0;
static uint64_t free_memory = 0;
static uint64_t hhdm = 0;

static inline void bitmap_set(size_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(size_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(size_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    hhdm = hhdm_offset;
    uint64_t highest_addr = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_memory += entry->length;
        }
        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) {
            highest_addr = top;
        }
    }

    max_pages = highest_addr / PAGE_SIZE;
    bitmap_size = PAGE_ALIGN_UP(max_pages / 8);

    // Find a usable region to store the bitmap
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap = (uint8_t *)(entry->base + hhdm);
            entry->base += bitmap_size;
            entry->length -= bitmap_size;
            break;
        }
    }

    if (!bitmap) {
        printk("PMM ERROR: Could not allocate memory for physical memory bitmap!\n");
        return;
    }

    // Default all pages to used / reserved
    memset(bitmap, 0xFF, bitmap_size);

    // Mark usable areas as free
    free_memory = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_page = entry->base / PAGE_SIZE;
            uint64_t page_count = entry->length / PAGE_SIZE;
            for (uint64_t p = 0; p < page_count; p++) {
                bitmap_clear(start_page + p);
                free_memory += PAGE_SIZE;
            }
        }
    }

    printk("PMM: Initialized. Total RAM: %llu MB, Free: %llu MB, Managed Pages: %llu\n",
           total_memory / (1024 * 1024), free_memory / (1024 * 1024), max_pages);
}

paddr_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < max_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_memory -= PAGE_SIZE;
            return (paddr_t)(i * PAGE_SIZE);
        }
    }
    printk("PMM: Out of physical memory!\n");
    return 0;
}

paddr_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    size_t consecutive = 0;
    uint64_t start_idx = 0;

    for (uint64_t i = 0; i < max_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) {
                start_idx = i;
            }
            consecutive++;
            if (consecutive == count) {
                for (size_t c = 0; c < count; c++) {
                    bitmap_set(start_idx + c);
                }
                free_memory -= count * PAGE_SIZE;
                return (paddr_t)(start_idx * PAGE_SIZE);
            }
        } else {
            consecutive = 0;
        }
    }

    printk("PMM: Failed to allocate %llu contiguous pages!\n", (uint64_t)count);
    return 0;
}

void pmm_free_page(paddr_t addr) {
    if (addr == 0 || (addr % PAGE_SIZE) != 0) return;
    uint64_t page_idx = addr / PAGE_SIZE;
    if (page_idx < max_pages && bitmap_test(page_idx)) {
        bitmap_clear(page_idx);
        free_memory += PAGE_SIZE;
    }
}

void pmm_free_pages(paddr_t addr, size_t count) {
    if (addr == 0 || (addr % PAGE_SIZE) != 0) return;
    uint64_t start_idx = addr / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint64_t page_idx = start_idx + i;
        if (page_idx < max_pages && bitmap_test(page_idx)) {
            bitmap_clear(page_idx);
            free_memory += PAGE_SIZE;
        }
    }
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

uint64_t pmm_get_free_memory(void) {
    return free_memory;
}

uint64_t pmm_get_used_memory(void) {
    return total_memory - free_memory;
}
