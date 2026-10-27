#ifndef HAL_IDT_H
#define HAL_IDT_H

#include <kernel/types.h>

typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame_t;

typedef void (*isr_handler_t)(interrupt_frame_t *frame);

void idt_init(void);
void idt_register_handler(uint8_t vector, isr_handler_t handler);
void isr_dispatch(interrupt_frame_t *frame);

#endif // HAL_IDT_H
