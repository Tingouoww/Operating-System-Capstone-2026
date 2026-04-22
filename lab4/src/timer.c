#include "timer.h"
#include "uart.h"
#include "sbi.h"
#include "fdt.h"

#define TIMER_INTERVAL_SEC 2
#define MAX_TIMERS         16

struct timer_entry {
    unsigned long deadline_ticks;
    void (*callback)(void *);
    void *arg;
    int   active;
};

static struct timer_entry timer_queue[MAX_TIMERS];
static unsigned long timer_freq         = 10000000;
static unsigned long timer_next_deadline = 0;

unsigned long boot_seconds = 0;

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
            if (timer_next_deadline == 0 || deadline < timer_next_deadline) {
                timer_next_deadline = deadline;
                sbi_set_timer(deadline);
            }
            return;
        }
    }
    uart_puts("add_timer: queue full\n");
}

void timer_handle_irq(void) {
    unsigned long cur;
    asm volatile("rdtime %0" : "=r"(cur));
    timer_next_deadline = 0;
    fire_expired_timers(cur);
    reprogram_timer();
}

static void boot_tick_cb(void *arg) {
    (void)arg;
    boot_seconds += TIMER_INTERVAL_SEC;
    uart_puts("boot time: ");
    uart_dec(boot_seconds);
    uart_puts("\n");
    add_timer(boot_tick_cb, 0, TIMER_INTERVAL_SEC);
}

void timer_init(const void *fdt) {
    int len, off;
    const unsigned char *p;

    off = fdt_path_offset(fdt, "/cpus");
    if (off >= 0) {
        p = fdt_getprop(fdt, off, "timebase-frequency", &len);
        if (p && len >= 4) {
            timer_freq = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
                         ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
            goto done;
        }
    }

    off = fdt_path_offset(fdt, "/cpus/cpu@0");
    if (off >= 0) {
        p = fdt_getprop(fdt, off, "timebase-frequency", &len);
        if (p && len >= 4) {
            timer_freq = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
                         ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
        }
    }

done:
    add_timer(boot_tick_cb, 0, TIMER_INTERVAL_SEC);
}
