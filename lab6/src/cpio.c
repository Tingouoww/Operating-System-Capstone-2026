#include "utils.h"
#include "cpio.h"
#include "uart.h"

static const void *initrd_start;
static const void *initrd_end;

/**
 * @brief Convert a hexadecimal string to integer
 *
 * @param s hexadecimal string
 * @param n length of the string
 * @return integer value
 */
static int hextoi(const char *s, int n)
{
    int r = 0;
    while (n-- > 0)
    {
        r = r << 4;
        if (*s >= '0' && *s <= '9')
            r += *s++ - '0';
        else if (*s >= 'A' && *s <= 'F')
            r += *s++ - 'A' + 10;
        else if (*s >= 'a' && *s <= 'f')
            r += *s++ - 'a' + 10;
        else
            s++;
    }
    return r;
}

/**
 * @brief Align a number to the nearest multiple of a given number
 *
 * @param n number
 * @param byte alignment
 * @return aligned number
 */
static int align(int n, int byte)
{
    return (n + byte - 1) & ~(byte - 1);
}

static void uart_put_uint(unsigned int value)
{
    char buf[16];
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

void initrd_init(void *start, void *end)
{
    initrd_start = start;
    initrd_end = end;
}

void initrd_list(const void *rd)
{ // rd: ramdisk/initrd
    if (!rd)
        rd = initrd_start;
    if (!rd)
        return;
    struct cpio_newc_header *cpio = (struct cpio_newc_header *)rd;
    unsigned int count = 0;

    while ((const void *)cpio < initrd_end && str_ncmp(cpio->c_magic, "070701", 6) == 0)
    {
        // magic = "070701" -> 表示 New ASCII Format
        int filesize = hextoi(cpio->c_filesize, sizeof(cpio->c_filesize));
        int namesize = hextoi(cpio->c_namesize, sizeof(cpio->c_namesize));
        const char *name = (const char *)(cpio + 1);
        const char *base = (const char *)cpio;

        if (str_cmp(name, "TRAILER!!!") == 0)
        {
            break;
        }

        count++;
        cpio = (struct cpio_newc_header *)(base + align((int)((name - base) + namesize), 4) +
                                           align(filesize, 4));
    }

    uart_puts("Total ");
    uart_put_uint(count);
    uart_puts(" files.\n");

    cpio = (struct cpio_newc_header *)rd;

    while ((const void *)cpio < initrd_end && str_ncmp(cpio->c_magic, "070701", 6) == 0)
    {
        int filesize = hextoi(cpio->c_filesize, sizeof(cpio->c_filesize));
        int namesize = hextoi(cpio->c_namesize, sizeof(cpio->c_namesize));
        const char *name = (const char *)(cpio + 1);
        const char *base = (const char *)cpio;

        if (str_cmp(name, "TRAILER!!!") == 0)
        {
            break;
        }

        uart_put_uint((unsigned int)filesize);
        uart_puts(" ");
        uart_puts(name);
        uart_puts("\n");

        cpio = (struct cpio_newc_header *)(base + align((int)((name - base) + namesize), 4) +
                                           align(filesize, 4));
    }
}

unsigned long cpio_find_exec(const char *filename)
{
    if (!initrd_start || !filename)
        return 0;

    struct cpio_newc_header *cpio = (struct cpio_newc_header *)initrd_start;

    while ((const void *)cpio < initrd_end && str_ncmp(cpio->c_magic, "070701", 6) == 0) {
        int filesize = hextoi(cpio->c_filesize, sizeof(cpio->c_filesize));
        int namesize = hextoi(cpio->c_namesize, sizeof(cpio->c_namesize));
        const char *name = (const char *)(cpio + 1);
        const char *base = (const char *)cpio;

        if (str_cmp(name, "TRAILER!!!") == 0)
            break;

        int data_offset = align((int)((name - base) + namesize), 4);

        if (str_cmp(name, filename) == 0 && filesize > 0)
            return (unsigned long)(base + data_offset);

        cpio = (struct cpio_newc_header *)(base + data_offset + align(filesize, 4));
    }

    return 0;
}

void initrd_cat(const void *rd, const char *filename)
{
    int i;

    if (!rd)
        rd = initrd_start;
    if (!rd || !filename)
        return;

    struct cpio_newc_header *cpio = (struct cpio_newc_header *)rd;

    while ((const void *)cpio < initrd_end && str_ncmp(cpio->c_magic, "070701", 6) == 0)
    {
        int filesize = hextoi(cpio->c_filesize, sizeof(cpio->c_filesize));
        int namesize = hextoi(cpio->c_namesize, sizeof(cpio->c_namesize));
        const char *name = (const char *)(cpio + 1);
        const char *base = (const char *)cpio;

        if (str_cmp(name, "TRAILER!!!") == 0)
            break;

        if (str_cmp(name, filename) == 0)
        {
            const char *data = base + align((int)((name - base) + namesize), 4);
            for (i = 0; i < filesize; i++)
            {
                uart_putc(data[i]);
            }
            uart_putc('\n');
            return;
        }

        cpio = (struct cpio_newc_header *)(base + align((int)((name - base) + namesize), 4) +
                                           align(filesize, 4));
    }

    uart_puts("initrd_cat: ");
    uart_puts(filename);
    uart_puts(": No such file\n");
}
