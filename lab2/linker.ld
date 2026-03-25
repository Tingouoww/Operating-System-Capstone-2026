OUTPUT_ARCH(riscv) /* 指定目標架構為 RISC-V */
ENTRY(_start) /* 定義程式的入口點 */
SECTIONS /* 定義輸出檔案的結構 */
{
    . = 0x00200000; /* Location Counter */
    _start = .; /* 將 _start 符號定義在當前位址 */
    .text : { *(.text.boot) *(.text) } /* boot 最先執行 */
    .rodata : { *(.rodata) } /*read only data*/
    .data : { *(.data) } /*initialized global variables*/
    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss)
        __bss_stop = .;
    } /* uninitialized global variables */
    . = ALIGN(16); /* 把 location counter 對齊到 *16 bytes /
    . = . + 0x4000; /* 預留 0x4000 bytes 給 stack */
    _end = .; /* 將 _end 定義在預留空間的末端。這個位址通常被拿來當作 Stack Pointer (sp) 的初始值 */
}
