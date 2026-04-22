#include "uart.h"
#include "fdt.h"

uintptr_t uart_base;

// ─── register offsets ───────────────────────────────────────────────────────

#ifdef QEMU
#define UART_RBR ((volatile unsigned char *)(uart_base + 0x0))
#define UART_THR ((volatile unsigned char *)(uart_base + 0x0))
#define UART_LSR ((volatile unsigned char *)(uart_base + 0x5))
#define UART_IER ((volatile unsigned char *)(uart_base + 0x1))
#define UART_IIR ((volatile unsigned char *)(uart_base + 0x2))
#define UART_FCR ((volatile unsigned char *)(uart_base + 0x2))
#else
/* PXA UART (ky,pxa-uart): reg-io-width=4 requires 32-bit word accesses */
#define UART_RBR ((volatile uint32_t *)(uart_base + 0x0))
#define UART_THR ((volatile uint32_t *)(uart_base + 0x0))
#define UART_LSR ((volatile uint32_t *)(uart_base + 0x14))
#define UART_IER ((volatile uint32_t *)(uart_base + 0x4))
#define UART_IIR ((volatile uint32_t *)(uart_base + 0x8))
#define UART_FCR ((volatile uint32_t *)(uart_base + 0x8))
#endif

#define LSR_DR   (1 << 0)   // Data Ready (RX byte available)
#define LSR_TDRQ (1 << 5)   // TX Data Request (THR empty)

// ─── PLIC configuration ──────────────────────────────────────────────────────

#ifdef QEMU
#define PLIC_BASE 0x0c000000UL
#define UART_IRQ  10
#else
#define PLIC_BASE 0xe0000000UL
#define UART_IRQ  42
#endif

/* PLIC 相關 register offset 要查 spec */
static unsigned int uart_plic_ctx = 1;  // S-mode context for current boot hart

static void plic_set_priority(unsigned int irq, unsigned int prio) {
    *(volatile unsigned int *)(PLIC_BASE + 4 * irq) = prio;
}

static void plic_enable_irq(unsigned int irq) {
    volatile unsigned int *en = (volatile unsigned int *)
        (PLIC_BASE + 0x2000 + 0x80 * uart_plic_ctx + 4 * (irq / 32));
    *en |= (1u << (irq % 32));
}

static void plic_set_threshold(unsigned int thr) {
    *(volatile unsigned int *)(PLIC_BASE + 0x200000 + 0x1000 * uart_plic_ctx) = thr;
}

static unsigned int plic_claim(void) {
    return *(volatile unsigned int *)(PLIC_BASE + 0x200004 + 0x1000 * uart_plic_ctx);
}

static void plic_complete(unsigned int irq) {
    *(volatile unsigned int *)(PLIC_BASE + 0x200004 + 0x1000 * uart_plic_ctx) = irq;
}

static unsigned int plic_get_enable_word(unsigned int irq) {
    return *(volatile unsigned int *)
        (PLIC_BASE + 0x2000 + 0x80 * uart_plic_ctx + 4 * (irq / 32));
}

static unsigned int plic_get_priority(unsigned int irq) {
    return *(volatile unsigned int *)(PLIC_BASE + 4 * irq);
}

static unsigned int plic_get_threshold(void) {
    return *(volatile unsigned int *)(PLIC_BASE + 0x200000 + 0x1000 * uart_plic_ctx);
}

// ─── ring buffers ────────────────────────────────────────────────────────────

#define RING_SIZE 256

/* circular queue */
typedef struct {
    volatile char data[RING_SIZE];
    volatile int  head;  // producer index
    volatile int  tail;  // consumer index
} ring_buf_t;

static ring_buf_t rx_ring;
static ring_buf_t tx_ring;
static int uart_async = 0;  // 0=polling, 1=interrupt-driven

static inline int  ring_empty(ring_buf_t *r) { return r->head == r->tail; }
static inline int  ring_full(ring_buf_t *r)  { return ((r->head + 1) % RING_SIZE) == r->tail; }

static inline void ring_push(ring_buf_t *r, char c) {
    r->data[r->head] = c;
    r->head = (r->head + 1) % RING_SIZE;
}

static inline char ring_pop(ring_buf_t *r) {
    char c = r->data[r->tail];
    r->tail = (r->tail + 1) % RING_SIZE;
    return c;
}

static unsigned int uart_plic_context_for_hart(unsigned long hartid) {
    return (unsigned int)(hartid * 2 + 1);
}

static char uart_normalize_rx_char(char c) {
    return c == '\r' ? '\n' : c;
}

static void uart_drain_rx_fifo(void) {
    while (*UART_LSR & LSR_DR) {
        char c = uart_normalize_rx_char((char)*UART_RBR);

        if (ring_full(&rx_ring))
            break;

        ring_push(&rx_ring, c);
    }
}

// ─── uart_init ───────────────────────────────────────────────────────────────

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffU) << 24) | ((x & 0x0000ff00U) << 8) |
           ((x & 0x00ff0000U) >> 8)  | ((x & 0xff000000U) >> 24);
}

void uart_init(const void *fdt) {
    int len, offset;
    unsigned long addr;
    const void *reg;
    const uint32_t *cells;

#ifdef QEMU
    offset = fdt_path_offset(fdt, "/soc/uart");
    if (offset < 0)
        offset = fdt_path_offset(fdt, "/soc/serial");
#else
    offset = fdt_path_offset(fdt, "/soc/serial");
#endif
    if (offset < 0) return;

    reg   = fdt_getprop(fdt, offset, "reg", &len);
    cells = (const uint32_t *)reg;

    if (len >= 16)
        addr = ((unsigned long)bswap32(cells[0]) << 32) | bswap32(cells[1]);
    else if (len >= 8)
        addr = bswap32(cells[0]);
    else
        return;

    if (addr != 0)
        uart_base = addr;
}

// ─── uart_interrupt_init ─────────────────────────────────────────────────────
/* 把 UART 從原本的 polling 模式，切換成 中斷驅動 + ring buffer */
void uart_interrupt_init(unsigned long hartid) {
    // head -> 寫入位置
    // tail -> 清出位置
    // head == tail 表示 buffer 為空
    rx_ring.head = rx_ring.tail = 0; // 接收緩衝區清空
    tx_ring.head = tx_ring.tail = 0; // 傳送緩衝區清空
    uart_plic_ctx = uart_plic_context_for_hart(hartid);

    // Enable FIFO, reset RX/TX FIFO, RX trigger = 1 byte
    *UART_FCR = (1 << 0) | (1 << 1) | (1 << 2);

    // Enable UART RX interrupt (bit 0 = RBFI) 接收端有資料時觸發中斷
    *UART_IER |= 0x1;

    // Configure PLIC: priority > 0, source enabled, threshold = 0
    plic_set_priority(UART_IRQ, 1);
    plic_enable_irq(UART_IRQ);
    plic_set_threshold(0);

    uart_async = 1;
}

void uart_debug_dump_state(void) {
    unsigned long sie = 0;
    unsigned long sstatus = 0;

    asm volatile("csrr %0, sie" : "=r"(sie));
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));

    uart_puts("[uart] base=");
    uart_hex(uart_base);
    uart_puts(" irq=");
    uart_dec(UART_IRQ);
    uart_puts("\n");

    uart_puts("[uart] ier=");
    uart_hex(*UART_IER);
    uart_puts(" iir=");
    uart_hex(*UART_IIR);
    uart_puts(" lsr=");
    uart_hex(*UART_LSR);
    uart_puts("\n");

    uart_puts("[plic] prio=");
    uart_dec(plic_get_priority(UART_IRQ));
    uart_puts(" ctx=");
    uart_dec(uart_plic_ctx);
    uart_puts(" enable_word=");
    uart_hex(plic_get_enable_word(UART_IRQ));
    uart_puts(" threshold=");
    uart_dec(plic_get_threshold());
    uart_puts("\n");

    uart_puts("[csr] sie=");
    uart_hex(sie);
    uart_puts(" sstatus=");
    uart_hex(sstatus);
    uart_puts("\n");
}

// ─── uart_handle_external_irq ────────────────────────────────────────────────

void uart_handle_external_irq(void) {
    unsigned int irq = plic_claim();

    if (irq == UART_IRQ)
        uart_drain_rx_fifo();

    if (irq)
        plic_complete(irq);
}

// ─── uart_getc ───────────────────────────────────────────────────────────────

char uart_getc(void) {
    if (!uart_async) {
        while ((*UART_LSR & LSR_DR) == 0) ;
        char c = (char)*UART_RBR;
        return c == '\r' ? '\n' : c;
    }

    while (ring_empty(&rx_ring)) ;

    return ring_pop(&rx_ring);
}

// ─── uart_putc ───────────────────────────────────────────────────────────────

void uart_putc(char c) {
    if (c == '\n') uart_putc('\r');

    unsigned int timeout = 100000;
    while ((*UART_LSR & LSR_TDRQ) == 0 && --timeout) ;
    *UART_THR = c;
}

// ─── uart_puts ───────────────────────────────────────────────────────────────

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

// ─── uart_hex / uart_dec ─────────────────────────────────────────────────────

void uart_hex(unsigned long h) {
    uart_puts("0x");
    for (int c = 60; c >= 0; c -= 4) {
        unsigned long n = (h >> c) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc((char)n);
    }
}

void uart_dec(unsigned long value) {
    char buf[32];
    int i = 0;

    if (value == 0) { uart_putc('0'); return; }
    while (value > 0) { buf[i++] = (char)('0' + (value % 10)); value /= 10; }
    while (i > 0) uart_putc(buf[--i]);
}
