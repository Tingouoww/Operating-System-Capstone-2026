#include "bootloader.h"
#include "fdt.h"
#include "uart.h"

#define BOOT_MAGIC 0x544f4f42U
#define MAX_KERNEL_SIZE (16UL * 1024UL * 1024UL)

#ifdef QEMU
#define BOOTLOADER_RELOC_BASE 0x80A00000UL
#else
#define BOOTLOADER_RELOC_BASE 0x20000000UL
#endif

/* 
    定義一種函式指標型別
    kernel_entry_t 指向一個 function
*/
typedef void (*kernel_entry_t)(unsigned long hartid, void *dtb);

static unsigned long boot_hartid;
static void *boot_dtb;
static int boot_ranges_checked;
static int boot_ranges_safe;

extern char _start;
extern char __image_end;

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

/* Return 1 when [target_start, target_end) does NOT overlap [block_start, block_end). */
static int range_check(unsigned long target_start, unsigned long target_end,
                       unsigned long block_start, unsigned long block_end) {
    return (target_end <= block_start) || (block_end <= target_start);
}

static void print_range(const char *name, unsigned long start, unsigned long end) {
    uart_puts(name);
    uart_puts(": [");
    uart_hex(start);
    uart_puts(", ");
    uart_hex(end);
    uart_puts(")\n");
}

static int get_dtb_range(const void *dtb, unsigned long *start, unsigned long *end) {
    const struct fdt_header *hdr;
    uint32_t magic;
    unsigned long size;

    if (!dtb || !start || !end) {
        return -1;
    }

    hdr = (const struct fdt_header *)dtb;
    magic = bswap32(hdr->magic);
    if (magic != 0xd00dfeedU) {
        return -1;
    }

    size = (unsigned long)bswap32(hdr->totalsize);
    if (size == 0) {
        return -1;
    }

    *start = (unsigned long)dtb;
    *end = *start + size;
    return 0;
}

static int bootloader_reloc_range_is_safe(void) {
    unsigned long image_size = (unsigned long)(&__image_end - &_start);
    unsigned long reloc_start = BOOTLOADER_RELOC_BASE;
    unsigned long reloc_end = reloc_start + image_size;
    unsigned long mem_base = 0;
    unsigned long mem_size = 0;
    unsigned long mem_end = 0;
    unsigned long dtb_start = 0;
    unsigned long dtb_end = 0;
    unsigned long initrd_start = 0;
    unsigned long initrd_end = 0;
    int has_mem = 0;
    int has_dtb = 0;
    int has_initrd = 0;

    if (image_size == 0) {
        uart_puts("Relocation safety check failed: image size is zero.\n");
        return 0;
    }

    has_mem = (fdt_get_memory_range(boot_dtb, &mem_base, &mem_size) == 0);
    if (has_mem) {
        mem_end = mem_base + mem_size;
    }

    has_dtb = (get_dtb_range(boot_dtb, &dtb_start, &dtb_end) == 0);
    has_initrd = (fdt_get_initrd_range(boot_dtb, &initrd_start, &initrd_end) == 0);

    uart_puts("Relocation safety ranges:\n");
    print_range("  reloc", reloc_start, reloc_end);
    if (has_mem) print_range("  memory", mem_base, mem_end);
    if (has_dtb) print_range("  dtb", dtb_start, dtb_end);
    if (has_initrd) print_range("  initrd", initrd_start, initrd_end);

    if (has_mem && (reloc_start < mem_base || reloc_end > mem_end)) {
        uart_puts("Relocation safety check failed: outside /memory.\n");
        return 0;
    }
    if (has_dtb && !range_check(reloc_start, reloc_end, dtb_start, dtb_end)) {
        uart_puts("Relocation safety check failed: overlap with dtb.\n");
        return 0;
    }
    if (has_initrd && !range_check(reloc_start, reloc_end, initrd_start, initrd_end)) {
        uart_puts("Relocation safety check failed: overlap with initrd.\n");
        return 0;
    }

    uart_puts("Relocation safety check passed.\n");
    return 1;
}

static int kernel_load_range_is_safe(unsigned long size) {
    unsigned long load_start = KERNEL_LOAD_ADDR;
    unsigned long load_end = load_start + size;
    unsigned long image_size = (unsigned long)(&__image_end - &_start);
    unsigned long reloc_start = BOOTLOADER_RELOC_BASE;
    unsigned long reloc_end = reloc_start + image_size;
    unsigned long dtb_start = 0;
    unsigned long dtb_end = 0;
    unsigned long initrd_start = 0;
    unsigned long initrd_end = 0;
    int has_dtb = 0;
    int has_initrd = 0;

    has_dtb = (get_dtb_range(boot_dtb, &dtb_start, &dtb_end) == 0);
    has_initrd = (fdt_get_initrd_range(boot_dtb, &initrd_start, &initrd_end) == 0);

    if (load_end < load_start) {
        uart_puts("Kernel load safety check failed: address overflow.\n");
        return 0;
    }

    if (!range_check(load_start, load_end, reloc_start, reloc_end)) {
        uart_puts("Kernel load safety check failed: overlap with relocated bootloader.\n");
        return 0;
    }
    if (has_dtb && !range_check(load_start, load_end, dtb_start, dtb_end)) {
        uart_puts("Kernel load safety check failed: overlap with dtb.\n");
        return 0;
    }
    if (has_initrd && !range_check(load_start, load_end, initrd_start, initrd_end)) {
        uart_puts("Kernel load safety check failed: overlap with initrd.\n");
        return 0;
    }

    return 1;
}

static uint32_t uart_get_u32_le(void) {
    uint32_t value = 0;
    /* 從 UART 連續讀 4 個 byte，組成一個 little-endian 的 uint32_t */

    // uart_getc 會等待資料(等待UART資料流送 bytes)
    value |= (uint32_t)(unsigned char)uart_getc();
    value |= (uint32_t)(unsigned char)uart_getc() << 8;
    value |= (uint32_t)(unsigned char)uart_getc() << 16;
    value |= (uint32_t)(unsigned char)uart_getc() << 24;

    return value;
}

void bootloader_init(unsigned long hartid, void *dtb) {
    boot_hartid = hartid;
    boot_dtb = dtb;
    boot_ranges_checked = 1;
    boot_ranges_safe = bootloader_reloc_range_is_safe();
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
    uart_puts("\n");

    if (!boot_ranges_checked || !boot_ranges_safe) {
        uart_puts("Bootloader safety check not passed, reject load.\n");
        return;
    }

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

    if (!kernel_load_range_is_safe((unsigned long)size)) {
        uart_puts("Kernel target range is unsafe.\n");
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

    // asm volatile("fence iorw, iorw" ::: "memory"); // 確保前面的讀寫先完成，不會因亂序到跳轉之後
    // asm volatile("fence.i" ::: "memory"); // 讓 CPU 取指令時看到最新寫入的內容，避免吃到舊快取資料
    asm volatile("fence.i");
    /* 
        kernel entry 是 function pointer
        把記憶體位址(load_addr)當成(可呼叫的 kernel 入口函式)來呼叫
        (函式指標被呼叫本身就代表跳到該位址執行)
    */
    kernel_entry = (kernel_entry_t)load_addr;
    kernel_entry(boot_hartid, boot_dtb);
}
