#include "sbi.h"
#include "uart.h"
#include "shell.h"
#include "bootloader.h"
#include "cpio.h"
#include "fdt.h"
#include "mem_allocator.h"
#include "mem_allocator_test.h"
#include "string.h"
#include "timer.h"

#include <stdint.h>

#define MSG_POOL_COUNT 16
#define MSG_POOL_LEN   64

static char msg_pool[MSG_POOL_COUNT][MSG_POOL_LEN];
static int  msg_pool_idx = 0;

static void settimeout_cb(void *arg) {
    uart_puts((const char *)arg);
    uart_puts("\n");
}

static int simple_atoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

extern int exec(const char *filename);

static const void *fdt_base;
static const void *initrd_base;

static inline uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
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
    //uart_puts("  test_buddy_merge - run a dedicated buddy merge test.\n");
    uart_puts("  exec <file> - execute user program from initramfs in U-mode.\n");
    uart_puts("  settimeout <sec> <msg> - show text after x sec.\n");
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
    else if (check_command(cmd, "test_buddy_merge"))
    {
        run_buddy_merge_test();
    }
    else if (cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'e' && cmd[3] == 'c' &&
             (cmd[4] == '\0' || cmd[4] == ' '))
    {
        const char *filename = skip_spaces(cmd + 4);

        if (*filename == '\0')
        {
            uart_puts("Usage: exec <filename>\n");
            return;
        }

        if (exec(filename) < 0)
        {
            uart_puts("exec: file not found\n");
        }
        // exec() never returns on success (sret jumps to U-mode)
    }
    else if (cmd[0] == 's' && cmd[1] == 'e' && cmd[2] == 't' &&
             cmd[3] == 't' && cmd[4] == 'i' && cmd[5] == 'm' &&
             cmd[6] == 'e' && cmd[7] == 'o' && cmd[8] == 'u' &&
             cmd[9] == 't' && (cmd[10] == '\0' || cmd[10] == ' '))
    {
        const char *p = skip_spaces(cmd + 10);
        if (*p == '\0') {
            uart_puts("Usage: settimeout <seconds> <message>\n");
            return;
        }
        int sec = simple_atoi(p);
        while (*p && *p != ' ') p++;
        p = skip_spaces(p);

        char *slot = msg_pool[msg_pool_idx % MSG_POOL_COUNT];
        msg_pool_idx++;
        int i = 0;
        while (*p && i < MSG_POOL_LEN - 1)
            slot[i++] = *p++;
        slot[i] = '\0';

        add_timer(settimeout_cb, slot, (unsigned long)sec);
    }
    else
    {
        uart_puts("Unknown command: ");
        uart_puts(cmd);
        uart_puts("\n");
        uart_puts("Use help to get commands.\n");
    }
}
