#include "bootloader.h"
#include "uart.h"

#define BOOT_MAGIC 0x544f4f42U
#define MAX_KERNEL_SIZE (16UL * 1024UL * 1024UL)

static unsigned long boot_hartid;
static void *boot_dtb;

/*
    定義一種函式指標型別
    kernel_entry_t 指向一個 function
*/
typedef void (*kernel_entry_t)(unsigned long hartid, void *dtb);

static inline uint32_t bswap32(uint32_t x)
{
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static uint32_t uart_get_u32_le(void)
{
    uint32_t value = 0;
    /* 從 UART 連續讀 4 個 byte，組成一個 little-endian 的 uint32_t */

    // uart_getc 會等待資料(等待UART資料流送 bytes)
    value |= (uint32_t)(unsigned char)uart_getc();
    value |= (uint32_t)(unsigned char)uart_getc() << 8;
    value |= (uint32_t)(unsigned char)uart_getc() << 16;
    value |= (uint32_t)(unsigned char)uart_getc() << 24;

    return value;
}

void bootloader_init(unsigned long hartid, void *dtb)
{
    boot_hartid = hartid;
    boot_dtb = dtb;
}

void bootloader_load(void)
{
    uint32_t magic;
    uint32_t size;
    unsigned char *load_addr;
    kernel_entry_t kernel_entry;

    uart_puts("Waiting for kernel payload header...\n");
    uart_puts("Send an image linked for ");
    uart_hex(KERNEL_LOAD_ADDR);
    uart_puts("\n");

    magic = uart_get_u32_le();
    if (magic != BOOT_MAGIC)
    {
        uart_puts("Invalid image header.\n");
        return;
    }

    size = uart_get_u32_le();
    if (size == 0 || size > MAX_KERNEL_SIZE)
    {
        uart_puts("Invalid image size.\n");
        return;
    }

    uart_puts("Receiving kernel, size = ");
    uart_hex(size);
    uart_puts("\n");

    load_addr = (unsigned char *)KERNEL_LOAD_ADDR;
    for (uint32_t i = 0; i < size; i++)
    {
        load_addr[i] = (unsigned char)uart_getc();
    }

    uart_puts("Kernel received. Jumping to ");
    uart_hex((unsigned long)load_addr);
    uart_puts("\n");

    asm volatile("fence iorw, iorw" ::: "memory"); // 確保前面的讀寫先完成，不會因亂序到跳轉之後
    asm volatile("fence.i");
    /*
        kernel entry 是 function pointer
        把記憶體位址(load_addr)當成(可呼叫的 kernel 入口函式)來呼叫
        (函式指標被呼叫本身就代表跳到該位址執行)
    */
    kernel_entry = (kernel_entry_t)load_addr;
    kernel_entry(boot_hartid, boot_dtb);
}
