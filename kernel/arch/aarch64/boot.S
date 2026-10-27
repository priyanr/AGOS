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
