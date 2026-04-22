#include "uart.h"
#include "string.h"
#include "mem_allocator.h"
#include "shell.h"
#include "cpio.h"
#include "fdt.h"
#include "bootloader.h"
#include "sbi.h"
#include "timer.h"

#define INITRD_BASE        0xa0200000
#define STACK_SIZE         0x1000
#define TIMER_INTERVAL_SEC 2
#define MAX_TIMERS         16

static unsigned long initrd_base = INITRD_BASE;
static unsigned long timer_freq  = 10000000;
unsigned long        boot_seconds = 0;

struct timer_entry {
    unsigned long deadline_ticks;
    void (*callback)(void *);
    void *arg;
    int   active;
};

static struct timer_entry timer_queue[MAX_TIMERS];

// Absolute tick value currently programmed into the hardware timer.
// 0 means not programmed.
static unsigned long timer_next_deadline = 0;

static int local_hextoi(const char *s, int n) {
    int r = 0;
    while (n-- > 0) {
        r <<= 4;
        if (*s >= '0' && *s <= '9')      r += *s++ - '0';
        else if (*s >= 'A' && *s <= 'F') r += *s++ - 'A' + 10;
        else if (*s >= 'a' && *s <= 'f') r += *s++ - 'a' + 10;
        else s++;
    }
    return r;
}

static int local_align(int n, int byte) {
    return (n + byte - 1) & ~(byte - 1);
}

static int local_memcmp(const void *a, const void *b, int n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

static void reprogram_timer(void) {
    unsigned long earliest = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_queue[i].active) continue;
        if (earliest == 0 || timer_queue[i].deadline_ticks < earliest)
            earliest = timer_queue[i].deadline_ticks;
    }
    if (earliest == 0) return;
    timer_next_deadline = earliest;
    sbi_set_timer(earliest);
}

static void fire_expired_timers(unsigned long now) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timer_queue[i].active && timer_queue[i].deadline_ticks <= now) {
            timer_queue[i].active = 0;
            timer_queue[i].callback(timer_queue[i].arg);
        }
    }
}

void add_timer(void (*callback)(void *), void *arg, unsigned long sec) {
    unsigned long now;
    asm volatile("rdtime %0" : "=r"(now));
    unsigned long deadline = now + sec * timer_freq;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_queue[i].active) {
            timer_queue[i].deadline_ticks = deadline;
            timer_queue[i].callback       = callback;
            timer_queue[i].arg            = arg;
            timer_queue[i].active         = 1;
            // Reprogram hardware if this deadline is earlier than current.
            if (timer_next_deadline == 0 || deadline < timer_next_deadline) {
                timer_next_deadline = deadline;
                sbi_set_timer(deadline);
            }
            return;
        }
    }
    uart_puts("add_timer: queue full\n");
}

static void boot_tick_cb(void *arg) {
    (void)arg;
    boot_seconds += TIMER_INTERVAL_SEC;
    uart_puts("boot time: ");
    uart_dec(boot_seconds);
    uart_puts("\n");
    add_timer(boot_tick_cb, 0, TIMER_INTERVAL_SEC);
}

static unsigned long get_timer_freq(const void *fdt) {
    int len, off;
    const unsigned char *p;

    /*
     * The standard DT layout stores timebase-frequency on /cpus.
     * Some trees may duplicate it on cpu@0, so keep that as a fallback.
     */
    off = fdt_path_offset(fdt, "/cpus");
    if (off >= 0) {
        p = fdt_getprop(fdt, off, "timebase-frequency", &len);
        if (p && len >= 4) {
            return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
                   ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
        }
    }

    off = fdt_path_offset(fdt, "/cpus/cpu@0");
    if (off < 0) return 10000000;

    p = fdt_getprop(fdt, off, "timebase-frequency", &len);
    if (!p || len < 4) return 10000000;

    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

int exec(const char *filename) {
    char *p = (char *)initrd_base;
    while (local_memcmp(p + sizeof(struct cpio_newc_header), "TRAILER!!!", 10)) {
        struct cpio_newc_header *hdr = (struct cpio_newc_header *)p;
        int namesize = local_hextoi(hdr->c_namesize, 8);
        int filesize = local_hextoi(hdr->c_filesize, 8);
        int headsize = local_align(sizeof(struct cpio_newc_header) + namesize, 4);
        int datasize = local_align(filesize, 4);
        if (!local_memcmp(p + sizeof(struct cpio_newc_header), filename, namesize)) {
            unsigned long kernel_sp;
            unsigned long user_sp    = (unsigned long)buddy_alloc(0) + STACK_SIZE;
            unsigned long user_entry = (unsigned long)(p + headsize);
            unsigned long sstatus;

            asm volatile("mv %0, sp"        : "=r"(kernel_sp));
            asm volatile("csrr %0, sstatus" : "=r"(sstatus));

            sstatus &= ~(1UL << 8);  // SPP=0: sret returns to U-mode
            sstatus |=  (1UL << 5);  // SPIE=1: enable interrupts in U-mode

            asm volatile(
                "csrw sscratch, %0\n"
                "mv sp, %1\n"
                "csrw sepc, %2\n"
                "csrw sstatus, %3\n"
                "sret\n"
                :
                : "r"(kernel_sp), "r"(user_sp), "r"(user_entry), "r"(sstatus)
                : "memory");

            __builtin_unreachable();
        }
        p += headsize + datasize;
    }
    return -1;
}

struct pt_regs {
    unsigned long ra;
    unsigned long sp;
    unsigned long gp;
    unsigned long tp;
    unsigned long t0;
    unsigned long t1;
    unsigned long t2;
    unsigned long s0;
    unsigned long s1;
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    unsigned long a3;
    unsigned long a4;
    unsigned long a5;
    unsigned long a6;
    unsigned long a7;
    unsigned long s2;
    unsigned long s3;
    unsigned long s4;
    unsigned long s5;
    unsigned long s6;
    unsigned long s7;
    unsigned long s8;
    unsigned long s9;
    unsigned long s10;
    unsigned long s11;
    unsigned long t3;
    unsigned long t4;
    unsigned long t5;
    unsigned long t6;
    unsigned long sepc;
    unsigned long sstatus;
    unsigned long scause;
    unsigned long stval;
};

#define SCAUSE_IRQ_FLAG         (1UL << 63)
#define SCAUSE_SUPERVISOR_TIMER 5
#define SCAUSE_SUPERVISOR_EXT   9

void do_trap(struct pt_regs *regs) {
    // SCAUSE_IRQ_FLAG : 1 表示為 interrupt
    if (regs->scause & SCAUSE_IRQ_FLAG) {
        unsigned long irq = regs->scause & ~SCAUSE_IRQ_FLAG; // 取出中斷號碼

        if (irq == SCAUSE_SUPERVISOR_TIMER) {
            unsigned long cur;
            asm volatile("rdtime %0" : "=r"(cur));
            timer_next_deadline = 0;
            fire_expired_timers(cur);
            reprogram_timer();
        } else if (irq == SCAUSE_SUPERVISOR_EXT) {
            uart_handle_external_irq();
        }
        // Interrupts: do NOT advance sepc
    } else {
        // Exception (ecall from U-mode, etc.)
        uart_puts("=== S-Mode trap ===\n");
        uart_puts("scause: ");
        uart_dec(regs->scause);
        uart_puts("\n");
        uart_puts("sepc: ");
        uart_hex(regs->sepc);
        uart_puts("\n");
        uart_puts("stval: ");
        uart_dec(regs->stval);
        uart_puts("\n");
        if (regs->scause == 8)  // U-mode ecall: skip past ecall instruction
            regs->sepc += 4;
    }
}

void start_kernel(unsigned long hartid, void *dtb) {
    char buf[128];
    int len = 0;
    char c;
    uart_init(dtb);
    uart_puts("\nStarting kernel ...\n");
    mem_allocator_init(dtb);
    shell_init(dtb);
    bootloader_init(hartid, dtb);
    initrd_base = fdt_get_initrd_start_or_default(dtb, INITRD_BASE);

    // uart_interrupt_init(hartid);

    timer_freq = get_timer_freq(dtb);

    asm volatile("csrs sie, %0" :: "r"(1UL << 5));   // STIE: enable timer interrupt
    asm volatile("csrs sie, %0" :: "r"(1UL << 9));   // SEIE: enable external interrupts
    asm volatile("csrsi sstatus, 0x2");               // SIE: global interrupt enable

    add_timer(boot_tick_cb, 0, TIMER_INTERVAL_SEC);

    print_shell_prompt();
    uart_puts("boot time: 0\n");
    while (1) {
        c = uart_getc();
        if (c == '\n') {
            uart_putc('\n');
            buf[len] = '\0';
            run_command(buf);
            len = 0;
            print_shell_prompt();
        } else if ((c == '\b' || c == '\x7f') && len > 0) {
            uart_puts("\b \b");
            len--;
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
            uart_putc(c);
        }
    }
}
