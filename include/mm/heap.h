#ifndef MM_HEAP_H
#define MM_HEAP_H

#include <kernel/types.h>

void heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t num, size_t size);
void kfree(void *ptr);

#endif // MM_HEAP_H
