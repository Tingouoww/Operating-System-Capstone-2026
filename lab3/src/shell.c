#include "sbi.h"
#include "uart.h"
#include "shell.h"
#include "bootloader.h"
#include "cpio.h"
#include "fdt.h"
#include "mem_allocator_test.h"

#include <stdint.h>

static const void *fdt_base;
static const void *initrd_base;

static inline uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static unsigned long read_be_addr(const void *prop, int len) {
    const uint32_t *cells = (const uint32_t *)prop;

    if (!prop) {
        return 0;
    }

    if (len >= 8) {
        return ((unsigned long)bswap32(cells[0]) << 32) | (unsigned long)bswap32(cells[1]);
    }

    if (len >= 4) {
        return (unsigned long)bswap32(cells[0]);
    }

    return 0;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ')
    {
        s++;
    }
    return s;
}

void shell_init(const void *fdt)
{
    unsigned long initrd_start = 0;
    unsigned long initrd_end = 0;

    fdt_base = fdt;
    initrd_base = 0;

    if (fdt_get_initrd_range(fdt, &initrd_start, &initrd_end) < 0)
    {
        uart_puts("Shell initialization failed. It can't get initrd_range.\n");
    }

    if (initrd_start != 0)
    {
        initrd_base = (const void *)initrd_start;
    }

    if (initrd_start != 0 && initrd_end > initrd_start)
    {
        initrd_init((void *)initrd_start, (void *)initrd_end);
    }
    else
    {
        uart_puts("initrd range not found in /chosen\n");
    }
}

// void shell_init(const void *fdt) {
//     int offset;
//     unsigned long initrd_start = 0;
//     unsigned long initrd_end = 0;
//     int start_len = 0;
//     int end_len = 0;
//     const void *start_prop;
//     const void *end_prop;

//     fdt_base = fdt;
//     initrd_base = 0;

//     // Read the initrd base address from the DTB /chosen node.
//     offset = fdt_path_offset(fdt, "/chosen");
//     if (offset < 0) {
//         uart_puts("failed to find /chosen\n");
//         return;
//     }

//     start_prop = fdt_getprop(fdt, offset, "linux,initrd-start", &start_len);
//     end_prop = fdt_getprop(fdt, offset, "linux,initrd-end", &end_len);
//     initrd_start = read_be_addr(start_prop, start_len);
//     initrd_end = read_be_addr(end_prop, end_len);

//     if (initrd_start != 0) {
//         initrd_base = (const void *)initrd_start;
//     }

//     if (initrd_start != 0 && initrd_end > initrd_start) {
//         initrd_init((void *)initrd_start, (void *)initrd_end);
//     } else {
//         uart_puts("initrd range not found in /chosen\n");
//     }
// }

void print_shell_prompt()
{
    uart_puts("opi-rv2> ");
}

int check_command(const char *input_str, const char *cmd_str)
{
    while (*input_str && *cmd_str)
    {
        if (*input_str == *cmd_str)
        {
            input_str++;
            cmd_str++;
        }
        else
            return 0;
    }
    return (*input_str == *cmd_str);
}

void print_help()
{
    uart_puts("Available commands:\n");
    uart_puts("  help   - show all commands.\n");
    uart_puts("  hello  - print Hello world.\n");
    uart_puts("  info   - print system info.\n");
    uart_puts("  ls   - list files in initramfs.\n");
    uart_puts("  cat <file> - print file content from initramfs.\n");
    //uart_puts("  load   - receive kernel_payload.bin over UART and jump to it.\n");
    uart_puts("  test_alloc - run memory allocator test.\n");
}

void print_hello()
{
    uart_puts("Hello world.\n");
}

void print_info()
{
    uart_puts("System information:\n");
    uart_puts("  OpenSBI specification version: ");
    uart_hex(sbi_get_spec_version());
    uart_puts("\n");

    uart_puts("  implementation ID: ");
    uart_hex(sbi_get_impl_id());
    uart_puts("\n");

    uart_puts("  implementation version: ");
    uart_hex(sbi_get_impl_version());
    uart_puts("\n");
}

void run_command(const char *cmd)
{
    if (cmd[0] == '\0')
        return;

    if (check_command(cmd, "hello"))
    {
        print_hello();
    }
    else if (check_command(cmd, "help"))
    {
        print_help();
    }
    else if (check_command(cmd, "info"))
    {
        print_info();
    }
    else if (check_command(cmd, "ls"))
    {
        initrd_list(NULL);
    }
    else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' &&
             (cmd[3] == '\0' || cmd[3] == ' '))
    {
        const char *filename = skip_spaces(cmd + 3);

        if (*filename == '\0')
        {
            uart_puts("Usage: cat <filename>\n");
            return;
        }

        initrd_cat(NULL, filename);
    }
    // else if (check_command(cmd, "load"))
    // {
    //     bootloader_load();
    // }
    else if (check_command(cmd, "test_alloc"))
    {
        run_mem_allocator_test();
    }
    else
    {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
        uart_puts("Use help to get commands.\n");
    }
}
