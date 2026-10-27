#include <hal/idt.h>
#include <hal/gdt.h>
#include <hal/cpu.h>
#include <kernel/printk.h>
#include <kernel/string.h>

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

static idt_entry_t idt_entries[256];
static idt_ptr_t idt_pointer;
static isr_handler_t isr_handlers[256];

extern uint64_t isr_stub_table[256];
extern void hal_load_idt(uint64_t idt_ptr);

static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection Exception", "VMM Communication Exception",
    "Security Exception", "Reserved"
};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].offset_low = (base & 0xFFFF);
    idt_entries[num].selector = sel;
    idt_entries[num].ist = 0;
    idt_entries[num].type_attr = flags;
    idt_entries[num].offset_mid = (base >> 16) & 0xFFFF;
    idt_entries[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt_entries[num].reserved = 0;
}

void idt_register_handler(uint8_t vector, isr_handler_t handler) {
    isr_handlers[vector] = handler;
}

void isr_dispatch(interrupt_frame_t *frame) {
    if (isr_handlers[frame->int_no]) {
        isr_handlers[frame->int_no](frame);
        return;
    }

    if (frame->int_no < 32) {
        printk("\n!!! CPU EXCEPTION %u (%s) !!!\n", frame->int_no,
               frame->int_no < 32 ? exception_messages[frame->int_no] : "Unknown");
        printk("RIP: 0x%016llx  CS: 0x%04llx  RFLAGS: 0x%016llx\n",
               frame->rip, frame->cs, frame->rflags);
        printk("RSP: 0x%016llx  SS: 0x%04llx  Error:  0x%016llx\n",
               frame->rsp, frame->ss, frame->err_code);
        printk("RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx\n",
               frame->rax, frame->rbx, frame->rcx);
        printk("RDX: 0x%016llx  RSI: 0x%016llx  RDI: 0x%016llx\n",
               frame->rdx, frame->rsi, frame->rdi);
        printk("RBP: 0x%016llx  R8:  0x%016llx  R9:  0x%016llx\n",
               frame->rbp, frame->r8, frame->r9);
        printk("CR2: 0x%016llx  CR3: 0x%016llx\n", read_cr2(), read_cr3());
        printk("SYSTEM HALTED.\n");
        cli();
        while (1) {
            hlt();
        }
    }

    // Default EOI if it's PIC hardware interrupt
    if (frame->int_no >= 32 && frame->int_no <= 47) {
        if (frame->int_no >= 40) {
            outb(0xA0, 0x20); // Slave EOI
        }
        outb(0x20, 0x20);     // Master EOI
    }
}

void idt_init(void) {
    memset(idt_entries, 0, sizeof(idt_entries));
    memset(isr_handlers, 0, sizeof(isr_handlers));

    for (int i = 0; i < 256; i++) {
        uint8_t flags = 0x8E; // Present, Ring 0, 64-bit Interrupt Gate
        if (i == 0x80) {
            flags = 0xEE;     // User-mode callable for int 0x80 syscalls
        }
        idt_set_gate(i, isr_stub_table[i], GDT_KERNEL_CODE_SEG, flags);
    }

    idt_pointer.limit = sizeof(idt_entries) - 1;
    idt_pointer.base = (uint64_t)&idt_entries;

    hal_load_idt((uint64_t)&idt_pointer);
}
