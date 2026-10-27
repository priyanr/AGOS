.code64
.altmacro

.global hal_load_gdt
.global hal_load_tss
.global hal_load_idt
.global hal_switch_context
.global isr_stub_table
.global thread_startup

.extern isr_dispatch
.extern thread_exit

.section .text

# void hal_load_gdt(uint64_t gdt_ptr, uint16_t code_seg, uint16_t data_seg)
hal_load_gdt:
    lgdt (%rdi)
    mov %dx, %ds
    mov %dx, %es
    mov %dx, %ss
    mov %dx, %fs
    mov %dx, %gs

    pushq %rsi                  # Push new CS
    lea .reload_cs(%rip), %rax
    pushq %rax                  # Push new RIP
    lretq
.reload_cs:
    ret

# void hal_load_tss(uint16_t tss_seg)
hal_load_tss:
    ltr %di
    ret

# void hal_load_idt(uint64_t idt_ptr)
hal_load_idt:
    lidt (%rdi)
    ret

# void hal_switch_context(uint64_t **old_rsp, uint64_t *new_rsp)
hal_switch_context:
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)
    movq %rsi, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popfq
    ret

# Thread startup trampoline
thread_startup:
    sti
    movq %r14, %rdi             # First argument: arg
    call *%r15                 # Call entry(arg)
    call thread_exit            # If entry returns, exit thread
1:
    hlt
    jmp 1b

.macro ISR_NOERR index
isr_stub_\index:
    pushq $0
    pushq $\index
    jmp isr_common_stub
.endm

.macro ISR_ERR index
isr_stub_\index:
    pushq $\index
    jmp isr_common_stub
.endm

# CPU Exceptions 0-31
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

.macro DEFINE_USER_ISR idx
    ISR_NOERR \idx
.endm

.set i, 32
.rept 224
    DEFINE_USER_ISR %i
    .set i, i + 1
.endr

isr_common_stub:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %rdi
    cld
    call isr_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp
    iretq

.macro DEFINE_TABLE_ENTRY idx
    .quad isr_stub_\idx
.endm

.section .data
.align 8
isr_stub_table:
.set k, 0
.rept 256
    DEFINE_TABLE_ENTRY %k
    .set k, k + 1
.endr
