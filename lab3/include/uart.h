#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(const void *fdt);
char uart_getc(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_hex(unsigned long h);

#endif
