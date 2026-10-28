#ifndef HAL_PLATFORM_H
#define HAL_PLATFORM_H

#include <kernel/types.h>

// Machine-specific services the shared kernel calls. Each architecture implements these in
// kernel/arch/<arch>/platform.c (context switch and thread startup in its assembly file).

// Shared kernel start-up (kernel/init/main.c), called by the architecture entry point once the
// HAL, memory and device drivers are up. Mounts the root filesystem, starts the scheduler and
// the shell. Never returns.
void kernel_start(void);

// Restart the machine
void hal_reboot(void);

// Service input devices that are polled rather than interrupt driven (Pi: USB keyboard and
// Bluetooth controller). Called from every input wait loop; does nothing on x86.
void hal_poll_input(void);

// Build the first stack frame of a new thread so hal_switch_context() starts it in
// entry(arg) with interrupts enabled. Returns the new stack pointer.
uint64_t *hal_thread_stack_init(uint64_t *stack_top, void (*entry)(void *), void *arg);

// Save callee-saved registers on the current stack, store the stack pointer in *old_sp,
// switch to new_sp and restore the registers saved there
void hal_switch_context(uint64_t **old_sp, uint64_t *new_sp);

// Stack the CPU switches to on an interrupt from the given thread (x86 TSS RSP0; no-op on ARM,
// where interrupts use the current stack)
void hal_set_kernel_stack(uint64_t stack_top);

#endif // HAL_PLATFORM_H
