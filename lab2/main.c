#include "bootloader.h"
#include "shell.h"
#include "uart.h"

void start_kernel(unsigned long hartid, void *dtb) {
    uart_init(dtb);
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
