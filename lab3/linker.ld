OUTPUT_ARCH(riscv) /* Target architecture: RISC-V */
ENTRY(_start) /* Program entry point */
SECTIONS
{
    . = 0x00200000;
    _start = .;
    .text : { *(.text.boot) *(.text) } /* Boot code first */
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    __image_end = .;
    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss)
        __bss_stop = .;
    } /* Uninitialized global variables */
    . = ALIGN(16);   /* 16-byte aligned stack base */
    . = . + 0x4000;  /* Reserve 16 KiB for stack */
    _end = .;        /* Initial stack pointer */
}
