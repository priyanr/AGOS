#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <kernel/string.h>
#include <kernel/printk.h>

#define HEAP_INITIAL_PAGES 1024 // 4 MB initial heap
#define HEAP_MAGIC 0xCAFEBABE

typedef struct heap_block {
    uint32_t magic;
    bool is_free;
    size_t size;
    struct heap_block *next;
    struct heap_block *prev;
} heap_block_t;

static heap_block_t *heap_start = NULL;

void heap_init(void) {
    paddr_t phys = pmm_alloc_pages(HEAP_INITIAL_PAGES);
    if (!phys) {
        printk("HEAP: Failed to allocate initial physical pages!\n");
        return;
    }

    heap_start = (heap_block_t *)vmm_phys_to_virt(phys);
    heap_start->magic = HEAP_MAGIC;
    heap_start->is_free = true;
    heap_start->size = (HEAP_INITIAL_PAGES * PAGE_SIZE) - sizeof(heap_block_t);
    heap_start->next = NULL;
    heap_start->prev = NULL;

    printk("HEAP: Initialized at Virt 0x%016llx with %d KB\n",
           (uint64_t)heap_start, (HEAP_INITIAL_PAGES * 4));
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    // 16-byte alignment
    size = ALIGN_UP(size, 16);

    heap_block_t *curr = heap_start;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            printk("HEAP CORRUPTION detected in block 0x%016llx!\n", (uint64_t)curr);
            return NULL;
        }

        if (curr->is_free && curr->size >= size) {
            // Can we split this block?
            if (curr->size >= size + sizeof(heap_block_t) + 16) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)curr + sizeof(heap_block_t) + size);
                new_block->magic = HEAP_MAGIC;
                new_block->is_free = true;
                new_block->size = curr->size - size - sizeof(heap_block_t);
                new_block->next = curr->next;
                new_block->prev = curr;

                if (curr->next) {
                    curr->next->prev = new_block;
                }
                curr->next = new_block;
                curr->size = size;
            }

            curr->is_free = false;
            return (void *)((uint8_t *)curr + sizeof(heap_block_t));
        }

        curr = curr->next;
    }

    printk("HEAP: Out of heap memory! (Requested %llu bytes)\n", (uint64_t)size);
    return NULL;
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void *kcalloc(size_t num, size_t size) {
    return kzalloc(num * size);
}

void kfree(void *ptr) {
    if (!ptr) return;

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC) {
        printk("HEAP: kfree called on invalid pointer 0x%016llx!\n", (uint64_t)ptr);
        return;
    }

    block->is_free = true;

    // Coalesce with next block if free
    if (block->next && block->next->is_free) {
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    // Coalesce with previous block if free
    if (block->prev && block->prev->is_free) {
        block->prev->size += sizeof(heap_block_t) + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}
