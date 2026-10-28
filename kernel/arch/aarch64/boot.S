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

// UART0 (Bluetooth) RX IRQ: drain the PL011 FIFO into bt_rx_ring (4096 bytes).
// Reading DR until the FIFO is empty clears the RX and RX-timeout interrupts.
irq_entry:
    stp     x0, x1, [sp, #-48]!
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    ldr     x0, =0x3F201000         // UART0 base
    ldr     x1, =bt_rx_ring
    ldr     x2, =bt_rx_head
    ldr     w3, [x2]
    mov     w5, #0                  // bytes taken this IRQ
2:  ldr     w4, [x0, #0x18]         // FR
    tbnz    w4, #4, 3f              // RXFE: FIFO empty
    ldr     w4, [x0]                // DR
    strb    w4, [x1, w3, uxtw]
    add     w3, w3, #1
    and     w3, w3, #4095
    add     w5, w5, #1
    b       2b
3:  str     w3, [x2]
    ldr     x2, =bt_irq_count       // diagnostics: IRQ entries and bytes received
    ldr     w4, [x2]
    add     w4, w4, #1
    str     w4, [x2]
    ldr     x2, =bt_irq_bytes
    ldr     w4, [x2]
    add     w4, w4, w5
    str     w4, [x2]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp], #48
    eret
