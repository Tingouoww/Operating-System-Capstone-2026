#include "task.h"
#include "uart.h"

#define MAX_TASKS 16

struct task_entry {
    task_callback_t callback;
    void           *arg;
    int             priority;
    int             active;
};

static struct task_entry task_queue[MAX_TASKS];
static volatile int task_runner_active;

void add_task(task_callback_t callback, void *arg, int priority) {
    unsigned long sstatus;
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus));

    int added = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!task_queue[i].active) {
            task_queue[i].callback = callback;
            task_queue[i].arg      = arg;
            task_queue[i].priority = priority;
            task_queue[i].active   = 1;
            added = 1;
            break;
        }
    }

    asm volatile("csrw sstatus, %0" :: "r"(sstatus));

    if (!added)
        uart_puts("add_task: queue full\n");
}

void run_tasks(void) {
    unsigned long entry_sstatus;

    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(entry_sstatus));
    if (task_runner_active) {
        asm volatile("csrw sstatus, %0" :: "r"(entry_sstatus));
        return;
    }
    task_runner_active = 1;
    asm volatile("csrw sstatus, %0" :: "r"(entry_sstatus));

    while (1) {
        unsigned long sstatus;
        asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus));

        int best = -1;
        for (int i = 0; i < MAX_TASKS; i++) {
            if (!task_queue[i].active) continue;
            if (best < 0 || task_queue[i].priority > task_queue[best].priority)
                best = i;
        }

        if (best < 0) {
            task_runner_active = 0;
            asm volatile("csrw sstatus, %0" :: "r"(sstatus));
            break;
        }

        task_callback_t cb  = task_queue[best].callback;
        void           *arg = task_queue[best].arg;
        task_queue[best].active = 0;

        /* Re-enable interrupts so higher-priority IRQs can preempt the task. */
        asm volatile("csrs sstatus, 0x2");

        cb(arg);

        /* Loop back to re-check the queue — any task enqueued during cb() is
           picked up here, in priority order. */
    }
}
