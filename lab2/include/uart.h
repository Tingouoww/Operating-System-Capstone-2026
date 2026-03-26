#ifndef UART_H
#define UART_H

#include <stdint.h>

extern uintptr_t uart_base;

#define UART_LSR  ((unsigned char*)(uart_base + 0x14))
#define UART_RBR  ((unsigned char*)(uart_base + 0x0))
#define UART_THR  ((unsigned char*)(uart_base + 0x0))

#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

void uart_init(const void *fdt);
char uart_getc(void);
void uart_putc(char c);
void uart_puts(const char* s);
void uart_hex(unsigned long h);

#endif
