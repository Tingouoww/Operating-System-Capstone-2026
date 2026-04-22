#include "uart.h"
#include "fdt.h"

uintptr_t uart_base;

#ifndef UART_LSR
#ifdef QEMU
#define UART_LSR ((unsigned char *)(uart_base + 0x5))
#else
#define UART_LSR ((unsigned char *)(uart_base + 0x14))
#endif
#endif

#define UART_RBR ((unsigned char *)(uart_base + 0x0))
#define UART_THR ((unsigned char *)(uart_base + 0x0))

#define LSR_DR (1 << 0)
#define LSR_TDRQ (1 << 5)

/* Convert a 32-bit big-endian value to CPU endianness */
static inline uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

void uart_init(const void *fdt)
{
    int len;
    int offset;
    unsigned long addr;

    const void *reg;
    const uint32_t *cells;

#ifdef QEMU
    offset = fdt_path_offset(fdt, "/soc/uart");
    if (offset < 0)
    {
        offset = fdt_path_offset(fdt, "/soc/serial");
    }
#else
    offset = fdt_path_offset(fdt, "/soc/serial");
#endif

    if (offset < 0)
        return;

    reg = fdt_getprop(fdt, offset, "reg", &len);

    cells = (const uint32_t *)reg;
    if (len >= 16)
    {
        // >= 4 cells (2 address + 2 size)
        addr = ((unsigned long)bswap32(cells[0]) << 32) | bswap32(cells[1]);
    }
    else if (len >= 8)
    {
        addr = bswap32(cells[0]);
    }
    else
    {
        return;
    }

    if (addr != 0)
    {
        uart_base = addr;
    }
}

char uart_getc(void)
{
    while ((*UART_LSR & LSR_DR) == 0)
        ;
    char c = (char)*UART_RBR;
    return c == '\r' ? '\n' : c;
}

void uart_putc(char c)
{
    if (c == '\n')
        uart_putc('\r');

    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
    *UART_THR = c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h)
{
    uart_puts("0x");
    unsigned long n;
    for (int c = 60; c >= 0; c -= 4)
    {
        n = (h >> c) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc(n);
    }
}

void uart_dec(unsigned long value){
    char buf[32];
    int i = 0;

    if (value == 0)
    {
        uart_putc('0');
        return;
    }

    while (value > 0)
    {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (i > 0)
    {
        uart_putc(buf[--i]);
    }
}
