#include "bootloader.h"
#include "cpio.h"
#include "fdt.h"
#include "shell.h"
#include "uart.h"

#include <stdint.h>

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static inline uintptr_t read_reg_base(const uint32_t *reg, int len) {
    if (!reg) {
        return 0;
    }

    if (len >= 16) {
        return (uintptr_t)(((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]));
    }

    if (len >= 8) {
        return (uintptr_t)(((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]));
    }

    if (len >= 4) {
        return (uintptr_t)bswap32(reg[0]);
    }

    return 0;
}

static void init_fdt_from_dtb(const void *dtb) {
    int offset;
    int len;
    const uint32_t *reg;
    uintptr_t uart_base_value;

    offset = fdt_path_offset(dtb, "/soc/serial");
    if (offset < 0) {
        offset = fdt_path_offset(dtb, "/soc/uart");
    }

    if (offset < 0) {
        while (1) {
        }
    }

    reg = (const uint32_t *)fdt_getprop(dtb, offset, "reg", &len);
    uart_base_value = read_reg_base(reg, len);

    if (uart_base_value == 0) {
        while (1) {
        }
    }

    uart_init(uart_base_value);
}

static void init_initrd_from_dtb(const void *dtb) {
    int chosen_offset;
    int start_len;
    int end_len;
    const uint32_t *start_prop;
    const uint32_t *end_prop;
    uintptr_t initrd_start;
    uintptr_t initrd_end;

    chosen_offset = fdt_path_offset(dtb, "/chosen");
    if (chosen_offset < 0) {
        return;
    }

    start_prop = (const uint32_t *)fdt_getprop(dtb, chosen_offset, "linux,initrd-start", &start_len);
    end_prop = (const uint32_t *)fdt_getprop(dtb, chosen_offset, "linux,initrd-end", &end_len);

    initrd_start = read_reg_base(start_prop, start_len);
    initrd_end = read_reg_base(end_prop, end_len);

    if (initrd_start != 0 && initrd_end > initrd_start) {
        initrd_init((void *)initrd_start, (void *)initrd_end);
    }
}

void start_kernel(unsigned long hartid, void *dtb) {
    char buf[64];
    int len = 0;

    init_fdt_from_dtb(dtb);
    bootloader_init(hartid, dtb);
    init_initrd_from_dtb(dtb);

    print_shell_prompt();

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
