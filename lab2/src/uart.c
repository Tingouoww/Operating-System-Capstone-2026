#include "uart.h"
#include "fdt.h"

uintptr_t uart_base;

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

void uart_init(const void *fdt) {
    int offset;
    int len = 0;
    const void *reg;
    const uint32_t *cells;
    unsigned long addr;

#ifdef QEMU
    offset = fdt_path_offset(fdt, "/soc/uart");
    if (offset < 0) {
        offset = fdt_path_offset(fdt, "/soc/serial");
    }
#else
    offset = fdt_path_offset(fdt, "/soc/serial");
#endif

    if (offset < 0) {
        uart_puts("uart_init: node not found\n");
        return;
    }

    reg = fdt_getprop(fdt, offset, "reg", &len);
    if (!reg) {
        uart_puts("uart_init: reg prop not found\n");
        return;
    }

    cells = (const uint32_t *)reg;
    if (len >= 16) {
        addr = ((unsigned long)bswap32(cells[0]) << 32) | bswap32(cells[1]);
    } else if (len >= 8) {
        addr = bswap32(cells[0]);
    } else {
        return;
    }

    if (addr != 0) {
        uart_base = addr;
    }
}

char uart_getc(void) {
    while ((*UART_LSR & LSR_DR) == 0)
        ;
    char c = (char)*UART_RBR;
    return c == '\r' ? '\n' : c;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
    *UART_THR = c;
}

void uart_puts(const char* s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    unsigned long n;
    for (int c = 60; c >= 0; c -= 4) {
        n = (h >> c) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc(n);
    }
}
