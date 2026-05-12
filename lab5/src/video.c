#include "video.h"

#include "utils.h"

#include <stdint.h>

#define FB_WIDTH          1920U
#define FB_HEIGHT         1080U
#define FB_BPP            4U
#define FB_STRIDE         (FB_WIDTH * FB_BPP)
#define XRGB8888          875713112U
#define CACHE_BLOCK_SIZE  64UL

#ifdef QEMU
#define FB_BASE 0x87000000UL
#else
#define FB_BASE 0x7f700000UL
#endif

#ifdef QEMU
static uint16_t bswap16(uint16_t x) {
    return (uint16_t)(((x & 0x00ffU) << 8) |
                      ((x & 0xff00U) >> 8));
}

static uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffU) << 24) |
           ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8) |
           ((x & 0xff000000U) >> 24);
}

static uint64_t bswap64(uint64_t x) {
    return ((x & 0x00000000000000ffULL) << 56) |
           ((x & 0x000000000000ff00ULL) << 40) |
           ((x & 0x0000000000ff0000ULL) << 24) |
           ((x & 0x00000000ff000000ULL) << 8) |
           ((x & 0x000000ff00000000ULL) >> 8) |
           ((x & 0x0000ff0000000000ULL) >> 24) |
           ((x & 0x00ff000000000000ULL) >> 40) |
           ((x & 0xff00000000000000ULL) >> 56);
}

#define QEMU_PACKED __attribute__((packed))
#define FW_CFG_BASE    0x10100000UL
#define FW_CFG_DMA_CTL_ERROR   0x01U
#define FW_CFG_DMA_CTL_READ    0x02U
#define FW_CFG_DMA_CTL_SELECT  0x08U
#define FW_CFG_DMA_CTL_WRITE   0x10U
#define FW_CFG_FILE_DIR        0x19U

struct QEMU_PACKED ramfb_config {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

struct QEMU_PACKED fwcfg_file {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char name[56];
};

struct QEMU_PACKED fwcfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

static volatile uint64_t *const fw_cfg_dma = (volatile uint64_t *)(FW_CFG_BASE + 0x10);

static void fw_cfg_dma_transfer(void *address,
                                uint32_t length,
                                uint32_t control) {
    struct fwcfg_dma_access access = {
        .control = bswap32(control),
        .length = bswap32(length),
        .address = bswap64((uint64_t)(uintptr_t)address),
    };

    *fw_cfg_dma = bswap64((uint64_t)(uintptr_t)&access);
    while ((bswap32(access.control) & ~FW_CFG_DMA_CTL_ERROR) != 0)
        ;
}

static void fw_cfg_read_entry(void *buf, unsigned int entry, unsigned int len) {
    unsigned int control = (entry << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ;
    fw_cfg_dma_transfer(buf, len, control);
}

static void fw_cfg_write_entry(void *buf, unsigned int entry, unsigned int len) {
    unsigned int control = (entry << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE;
    fw_cfg_dma_transfer(buf, len, control);
}

static int fw_cfg_find_file(const char *name) {
    uint32_t count = 0;

    fw_cfg_read_entry(&count, FW_CFG_FILE_DIR, sizeof(count));
    count = bswap32(count);

    for (uint32_t i = 0; i < count; i++) {
        struct fwcfg_file file;

        fw_cfg_dma_transfer(&file, sizeof(file), FW_CFG_DMA_CTL_READ);
        if (str_ncmp(name, file.name, sizeof(file.name)) == 0)
            return (int)bswap16(file.select);
    }
    return -1;
}
#endif

// CPU 寫東西習慣先放 cache，不會馬上存到 DRAM。但螢幕控制器不看 cache，只看 DRAM
// 把 start 值搬到 a0 暫存器
// .word 0x0025200F 發出 cbo.flush a0 指令 (機器碼)
// 0x0025200F = cbo.flush (a0) 把 a0 所指的那條快取行寫回 DRAM 並使該行無效
#define cbo_flush(start)                \
    ({                                  \
        asm volatile("mv a0, %0\n\t"    \
                     ".word 0x0025200F" \
                     :                  \
                     : "r"(start)       \
                     : "memory", "a0"); \
    })

// *這個函式負責將 D-cache（資料快取）中的指定記憶體範圍強制同步回記憶體（flush）
void flush_dcache(void *addr, unsigned long len) {
    unsigned long start = (unsigned long)addr & ~(CACHE_BLOCK_SIZE - 1);
    unsigned long end = (unsigned long)addr + len;

    __sync_synchronize(); // 確認 CPU 所有寫入都完成再 FLUSH
    for (unsigned long line = start; line < end; line += CACHE_BLOCK_SIZE) {
        cbo_flush(line);
        __sync_synchronize(); // 確保 flush 完成
    }
}

void video_init(const void *fdt) {
    (void)fdt;

#ifdef QEMU
    int entry;
    struct ramfb_config cfg = {
        .addr = bswap64(FB_BASE),
        .fourcc = bswap32(XRGB8888),
        .flags = bswap32(0),
        .width = bswap32(FB_WIDTH),
        .height = bswap32(FB_HEIGHT),
        .stride = bswap32(FB_STRIDE),
    };

    entry = fw_cfg_find_file("etc/ramfb");
    if (entry >= 0)
        fw_cfg_write_entry(&cfg, (unsigned int)entry, sizeof(cfg));
#endif
}

void video_display(const unsigned int *bmp_image,
                   unsigned int width,
                   unsigned int height) {
    unsigned int *fb = (unsigned int *)(uintptr_t)FB_BASE;
    unsigned int start_x;
    unsigned int start_y;

    if (!bmp_image || width == 0 || height == 0)
        return;
    if (width > FB_WIDTH || height > FB_HEIGHT)
        return;

    start_x = (FB_WIDTH - width) / 2U;
    start_y = (FB_HEIGHT - height) / 2U;

    for (unsigned int y = 0; y < height; y++) {
        void *dst = fb + (start_y + y) * FB_WIDTH + start_x;
        const void *src = bmp_image + y * width;
        unsigned long line_bytes = (unsigned long)width * sizeof(unsigned int);

        memcpy(dst, src, line_bytes);
        flush_dcache(dst, line_bytes);
    }
}
