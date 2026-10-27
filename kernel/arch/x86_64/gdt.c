#include <hal/gdt.h>
#include <kernel/string.h>

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} gdt_tss_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

static struct {
    gdt_entry_t null_desc;
    gdt_entry_t kernel_code;
    gdt_entry_t kernel_data;
    gdt_entry_t user_data;
    gdt_entry_t user_code;
    gdt_tss_entry_t tss;
} __attribute__((packed)) gdt_table;

static tss_t main_tss;
static gdt_ptr_t gdt_pointer;

extern void hal_load_gdt(uint64_t gdt_ptr, uint16_t code_seg, uint16_t data_seg);
extern void hal_load_tss(uint16_t tss_seg);

static void set_gdt_entry(gdt_entry_t *entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    entry->base_low = (base & 0xFFFF);
    entry->base_middle = (base >> 16) & 0xFF;
    entry->base_high = (base >> 24) & 0xFF;

    entry->limit_low = (limit & 0xFFFF);
    entry->granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    entry->access = access;
}

static void set_tss_entry(gdt_tss_entry_t *entry, uint64_t base, uint32_t limit) {
    entry->limit_low = (limit & 0xFFFF);
    entry->base_low = (base & 0xFFFF);
    entry->base_middle = (base >> 16) & 0xFF;
    entry->access = 0x89; // Present, 64-bit TSS (Available)
    entry->granularity = ((limit >> 16) & 0x0F);
    entry->base_high = (base >> 24) & 0xFF;
    entry->base_upper = (base >> 32) & 0xFFFFFFFF;
    entry->reserved = 0;
}

void tss_set_rsp0(uint64_t rsp0) {
    main_tss.rsp0 = rsp0;
}

void gdt_init(void) {
    memset(&gdt_table, 0, sizeof(gdt_table));
    memset(&main_tss, 0, sizeof(main_tss));

    main_tss.iopb_offset = sizeof(main_tss);

    // 0x00: Null descriptor
    set_gdt_entry(&gdt_table.null_desc, 0, 0, 0, 0);

    // 0x08: Kernel 64-bit Code (Conforming=0, Readable=1, LongMode=1)
    set_gdt_entry(&gdt_table.kernel_code, 0, 0xFFFFF, 0x9A, 0xA0);

    // 0x10: Kernel Data (Writable=1)
    set_gdt_entry(&gdt_table.kernel_data, 0, 0xFFFFF, 0x92, 0xC0);

    // 0x18: User Data (DPL=3, Writable=1)
    set_gdt_entry(&gdt_table.user_data, 0, 0xFFFFF, 0xF2, 0xC0);

    // 0x20: User 64-bit Code (DPL=3, Conforming=0, Readable=1, LongMode=1)
    set_gdt_entry(&gdt_table.user_code, 0, 0xFFFFF, 0xFA, 0xA0);

    // 0x28: TSS (16 bytes in x86_64)
    set_tss_entry(&gdt_table.tss, (uint64_t)&main_tss, sizeof(main_tss) - 1);

    gdt_pointer.limit = sizeof(gdt_table) - 1;
    gdt_pointer.base = (uint64_t)&gdt_table;

    hal_load_gdt((uint64_t)&gdt_pointer, GDT_KERNEL_CODE_SEG, GDT_KERNEL_DATA_SEG);
    hal_load_tss(GDT_TSS_SEG);
}
