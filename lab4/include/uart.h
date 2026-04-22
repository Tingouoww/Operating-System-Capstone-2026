#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(const void *fdt);
void uart_interrupt_init(unsigned long hartid);
void uart_handle_external_irq(void);
void uart_debug_dump_state(void);
char uart_getc(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_hex(unsigned long h);
void uart_dec(unsigned long value);

#endif
