#include <hal/platform.h>
#include <hal/cpu.h>
#include <hal/gdt.h>

extern void thread_startup(void); // entry.S: sti, entry(arg), thread_exit()

uint64_t *hal_thread_stack_init(uint64_t *stack_top, void (*entry)(void *), void *arg) {
    // hal_switch_context pops r15, r14, r13, r12, rbp, rbx, rflags and executes ret, which
    // jumps to thread_startup with entry in r15 and arg in r14
    *(--stack_top) = (uint64_t)thread_startup;  // Return address for ret in hal_switch_context
    *(--stack_top) = 0x202;                     // rflags (IF=1)
    *(--stack_top) = 0;                         // rbx
    *(--stack_top) = 0;                         // rbp
    *(--stack_top) = 0;                         // r12
    *(--stack_top) = 0;                         // r13
    *(--stack_top) = (uint64_t)arg;             // r14
    *(--stack_top) = (uint64_t)entry;           // r15
    return stack_top;
}

void hal_set_kernel_stack(uint64_t stack_top) {
    tss_set_rsp0(stack_top);
}

void hal_poll_input(void) {
    // PS/2 keyboard and serial are interrupt driven / polled by their own read functions
}

void hal_reboot(void) {
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE); // Pulse reset line through the keyboard controller
    hlt();
}
