#include "bootloader.h"
#include "uart.h"

#define BOOT_MAGIC 0x544f4f42U
#define MAX_KERNEL_SIZE (16UL * 1024UL * 1024UL)

typedef void (*kernel_entry_t)(unsigned long hartid, void *dtb);

static unsigned long boot_hartid;
static void *boot_dtb;

static uint32_t uart_get_u32_le(void) {
    uint32_t value = 0;

    value |= (uint32_t)(unsigned char)uart_getc();
    value |= (uint32_t)(unsigned char)uart_getc() << 8;
    value |= (uint32_t)(unsigned char)uart_getc() << 16;
    value |= (uint32_t)(unsigned char)uart_getc() << 24;

    return value;
}

void bootloader_init(unsigned long hartid, void *dtb) {
    boot_hartid = hartid;
    boot_dtb = dtb;
}

void bootloader_load(void) {
    uint32_t magic;
    uint32_t size;
    unsigned char *load_addr;
    uint32_t i;
    kernel_entry_t kernel_entry;

    uart_puts("Waiting for kernel payload header...\n");
    uart_puts("Send an image linked for ");
    uart_hex(KERNEL_LOAD_ADDR);
    uart_puts(" (for QEMU use kernel_payload.bin).\n");

    magic = uart_get_u32_le();
    if (magic != BOOT_MAGIC) {
        uart_puts("Invalid image header.\n");
        return;
    }

    size = uart_get_u32_le();
    if (size == 0 || size > MAX_KERNEL_SIZE) {
        uart_puts("Invalid image size.\n");
        return;
    }

    uart_puts("Receiving kernel, size = ");
    uart_hex(size);
    uart_puts("\n");

    load_addr = (unsigned char *)KERNEL_LOAD_ADDR;
    for (i = 0; i < size; i++) {
        load_addr[i] = (unsigned char)uart_getc();
    }

    uart_puts("Kernel received. Jumping to ");
    uart_hex(KERNEL_LOAD_ADDR);
    uart_puts("\n");

    asm volatile("fence iorw, iorw" ::: "memory");
    asm volatile("fence.i" ::: "memory");

    kernel_entry = (kernel_entry_t)load_addr;
    kernel_entry(boot_hartid, boot_dtb);
}
