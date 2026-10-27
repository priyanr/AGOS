#ifndef HAL_GDT_H
#define HAL_GDT_H

#include <kernel/types.h>

#define GDT_KERNEL_CODE_SEG 0x08
#define GDT_KERNEL_DATA_SEG 0x10
#define GDT_USER_DATA_SEG   (0x18 | 3)
#define GDT_USER_CODE_SEG   (0x20 | 3)
#define GDT_TSS_SEG         0x28

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss_t;

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif // HAL_GDT_H
