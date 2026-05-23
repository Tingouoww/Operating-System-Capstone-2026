#include "timer.h"
#include "uart.h"
#include "sbi.h"
#include "fdt.h"
#include "task.h"

#define MAX_TIMERS              64
#define PREEMPT_TICKS_DIVISOR   32

struct timer_entry {
    unsigned long deadline_ticks;
    void (*callback)(void *);
    void *arg;
    int   active;
};

static struct timer_entry timer_queue[MAX_TIMERS];
static unsigned long timer_freq         = 10000000;
static unsigned long timer_next_deadline = 0;
static unsigned long preempt_interval_ticks = 1;
static unsigned long next_preempt_deadline = 0;
static volatile int preempt_pending = 0;

static void reprogram_timer(void) {
    // Hardware timer tracks only one deadline, so pick the earliest active
    // software timer and ask OpenSBI to interrupt us at that time.

    unsigned long earliest = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_queue[i].active) continue;
        if (earliest == 0 || timer_queue[i].deadline_ticks < earliest)
            earliest = timer_queue[i].deadline_ticks;
    }
    if (next_preempt_deadline != 0 &&
        (earliest == 0 || next_preempt_deadline < earliest)) {
        earliest = next_preempt_deadline;
    }
    if (earliest == 0) return;
    timer_next_deadline = earliest;
    sbi_set_timer(earliest);
}

static void fire_expired_timers(unsigned long now) {
    // 執行所有已到期 timer，而且先設 active = 0，這樣 callback 裡如果又呼叫 add_timer()，不會卡在同一個舊 timer slot
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timer_queue[i].active && timer_queue[i].deadline_ticks <= now) {
            timer_queue[i].active = 0;
            if (timer_queue[i].callback)
                timer_queue[i].callback(timer_queue[i].arg);
        }
    }
}

int add_timer_ticks(void (*callback)(void *), void *arg, unsigned long ticks) {
    /* 把 callback 放進 timer_queue，設定它在 ticks 後到期。 */
    unsigned long now;
    unsigned long deadline;

    if (ticks == 0)
        ticks = 1;

    asm volatile("rdtime %0" : "=r"(now));
    deadline = now + ticks;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timer_queue[i].active) {
            timer_queue[i].deadline_ticks = deadline;
            timer_queue[i].callback       = callback;
            timer_queue[i].arg            = arg;
            timer_queue[i].active         = 1;
            if (timer_next_deadline == 0 || deadline < timer_next_deadline) {
                // 設最早到期的為下一個 interrupt
                timer_next_deadline = deadline;
                sbi_set_timer(deadline);
            }
            return 0;
        }
    }
    uart_puts("add_timer: queue full\n");
    return -1;
}

unsigned long timer_get_frequency(void) {
    return timer_freq;
}

unsigned long timer_us_to_ticks(unsigned long usec) {
    unsigned long long ticks;

    ticks = (unsigned long long)usec * (unsigned long long)timer_freq;
    ticks = (ticks + 1000000ULL - 1) / 1000000ULL;
    if (ticks == 0)
        ticks = 1;
    return (unsigned long)ticks;
}

int add_timer_us(void (*callback)(void *), void *arg, unsigned long usec) {
    return add_timer_ticks(callback, arg, timer_us_to_ticks(usec));
}

int add_timer(void (*callback)(void *), void *arg, unsigned long sec) {
    return add_timer_ticks(callback, arg, sec * timer_freq);
}

int timer_consume_preempt_pending(void) {
    int pending = preempt_pending;
    preempt_pending = 0;
    return pending;
}

static void run_expired_timers(void *arg){
    unsigned long now = (unsigned long) arg;
    fire_expired_timers(now);
    reprogram_timer();
}

/* timer interrupt 發生時真正處理 software timers 的函式 */
/*
    timer 到期
    -> CPU 產生 supervisor timer interrupt
    -> stvec / handle_exception
    -> do_trap()
    -> timer_handle_irq()
*/
void timer_handle_irq(void) {
    unsigned long cur;
    asm volatile("rdtime %0" : "=r"(cur));

    if (next_preempt_deadline != 0 && cur >= next_preempt_deadline) {
        do {
            next_preempt_deadline += preempt_interval_ticks;
        } while (next_preempt_deadline <= cur);
        preempt_pending = 1; // 需要強制切換
    }

    timer_next_deadline = 0;
    sbi_set_timer(-1UL); // 先把 timecmp 推到極大值，避免 timer interrupt 在 run_tasks 開中斷時持續觸發
    add_task(run_expired_timers, (void *)cur, 0); // priority = 0, 最低, 由 run_tasks() 執行
}

/* 讀 timebase-frequency: 代表 rdtime 每秒增加幾個 tick */
void timer_init(const void *fdt) {
    int len, off;
    const unsigned char *p;
    unsigned long now;

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
    preempt_interval_ticks = timer_freq / PREEMPT_TICKS_DIVISOR;
    if (preempt_interval_ticks == 0)
        preempt_interval_ticks = 1;
    asm volatile("rdtime %0" : "=r"(now));
    next_preempt_deadline = now + preempt_interval_ticks;
    timer_next_deadline = 0;
    reprogram_timer();
}
