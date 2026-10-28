ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text.boot : {
        KEEP(*(.text.boot))
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(16);
    __bss_start = .;

    /* Buffers the GPU / DMA read directly (mailbox messages, audio DMA). mmu.c maps this
       2 MB block uncached so no cache maintenance is needed; nothing else may share it. */
    . = ALIGN(0x200000);
    __gpu_shared_start = .;
    .gpu_shared (NOLOAD) : {
        *(.bss.gpu_shared)
    }
    . = ALIGN(0x200000);
    __gpu_shared_end = .;

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(16);
    __bss_end = .;

    _end = .;
}
