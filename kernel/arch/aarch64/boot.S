.section ".text.boot"
.global _start

_start:
    // Only allow primary CPU (core 0) to boot
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, 2f

1:  // Secondary cores hang
    wfe
    b       1b

2:
    // Check Current Exception Level
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    beq     el3_entry
    cmp     x0, #2
    beq     el2_entry
    b       el1_entry

el3_entry:
    // Don't trap FP/SIMD in EL3
    msr     cptr_el3, xzr
    // Configure EL3 and transition to EL2
    mov     x0, #(1 << 10)          // RW bit = AArch64 for EL2
    orr     x0, x0, #(1 << 0)       // NS bit = Non-Secure
    msr     scr_el3, x0
    mov     x0, #0x3c9              // SPSR: DAIF masked, EL2h
    msr     spsr_el3, x0
    adr     x0, el2_entry
    msr     elr_el3, x0
    eret

el2_entry:
    // Don't trap FP/SIMD in EL2
    msr     cptr_el2, xzr
    // Let EL1 use the physical generic timer (scheduler tick) and counter
    mov     x0, #3                  // CNTHCTL_EL2.EL1PCEN | EL1PCTEN
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    // Configure EL2 and transition to EL1
    mov     x0, #(1 << 31)          // HCR_EL2.RW = 1 (AArch64 for EL1)
    msr     hcr_el2, x0
    mov     x0, #0x3c5              // SPSR: DAIF masked, EL1h
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

el1_entry:
    // Enable FP/SIMD at EL1 (CPACR_EL1.FPEN = 0b11)
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    // Initialize Stack Pointer below kernel load address (0x80000)
    ldr     x0, =_start
    mov     sp, x0

    // Install exception vectors (IRQs stay masked until a driver unmasks them)
    ldr     x0, =vectors
    msr     vbar_el1, x0
    isb

    // Zero out BSS section
    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
    sub     x2, x2, x1
    cbz     x2, 4f

3:
    str     xzr, [x1], #8
    subs    x2, x2, #8
    bgt     3b

4:
    // Jump to high-level kernel entry point
    bl      kmain_aarch64

    // If kmain returns, halt core 0
halt_loop:
    wfe
    b       halt_loop

// EL1 vector table: 16 entries of 0x80 bytes. Only "current EL, SPx" IRQ is serviced;
// every other exception prints ESR/ELR/FAR and halts instead of hanging silently.
.section ".text"
.balign 2048
vectors:
    .rept 5                 // EL1t sync/irq/fiq/serror, EL1h sync
    .balign 0x80
    b       exc_unhandled
    .endr
    .balign 0x80            // EL1h IRQ
    b       irq_entry
    .rept 10                // EL1h fiq/serror, lower EL entries
    .balign 0x80
    b       exc_unhandled
    .endr

exc_unhandled:
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      exception_report
1:  wfe
    b       1b

// IRQ: save the registers a C call may clobber plus ELR/SPSR on the current thread's stack,
// then run irq_dispatch (timer tick, Bluetooth UART). The timer tick may switch threads from
// inside irq_dispatch; this frame stays on the old thread's stack until it is resumed.
irq_entry:
    sub     sp, sp, #192
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x29, [sp, #144]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x30, x0, [sp, #160]
    str     x1, [sp, #176]
    bl      irq_dispatch
    ldr     x1, [sp, #176]
    ldp     x30, x0, [sp, #160]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldp     x18, x29, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #192
    eret

// void hal_switch_context(uint64_t **old_sp, uint64_t *new_sp)
// Saves the callee-saved registers x19-x30 on the current stack (x30 = return address)
.global hal_switch_context
hal_switch_context:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

// First code a new thread runs (hal_thread_stack_init sets x30 here, x19 = entry, x20 = arg)
.global thread_startup
thread_startup:
    msr     daifclr, #2             // IRQs on
    mov     x0, x20
    blr     x19
    bl      thread_exit
1:  wfi
    b       1b
