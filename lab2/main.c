#include "bootloader.h"
#include "shell.h"
#include "uart.h"

extern char _start;

void start_kernel(unsigned long hartid, void *dtb) { 
    /* 
        - hartid (hardware thread ID): the identifier of each hardware thread (core/thread) in RISC-V.
        - void *dtb : Pointer to the DTB base address (passed in a1 by SBI).
    */
    uart_init(dtb);
    uart_puts("[kernel] _start @ ");
    uart_hex((unsigned long)&_start);
    uart_puts("\n");
    uart_puts("[kernel] start_kernel @ ");
    uart_hex((unsigned long)&start_kernel);
    uart_puts("\n");
    shell_init(dtb);
    bootloader_init(hartid, dtb);

    print_shell_prompt();
    char buf[64];
    int len = 0;
    while (1) {
        char c = uart_getc();

        if (c == '\n') {
            uart_puts("\n");
            buf[len] = '\0';
            len = 0;
            run_command(buf);
            print_shell_prompt();
            continue;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                uart_puts("\b \b");
            }
            continue;
        }

        if (len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
            uart_putc(c);
        }
    }
}
