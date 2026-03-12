#ifndef UART_H
#define UART_H

#define UART_BASE 0xD4017000UL
#define UART_LSR  ((unsigned char*)(UART_BASE + 0x14))
// #define UART_BASE 0x10000000UL
// #define UART_LSR  ((unsigned char*)(UART_BASE + 0x5))
#define UART_RBR  ((unsigned char*)(UART_BASE + 0x0))
#define UART_THR  ((unsigned char*)(UART_BASE + 0x0))

#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

char uart_getc(void);
void uart_putc(char c);
void uart_puts(const char* s);
void uart_hex(unsigned long h);

#endif
