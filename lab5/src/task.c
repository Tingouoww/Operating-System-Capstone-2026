#include "task.h"
#include "uart.h"

#define MAX_TASKS 64

struct task_entry{
    task_callback_t callback;
    void *arg;
    int priority;
    int active;
};

static struct task_entry task_queue[MAX_TASKS];
static volatile int current_task_priority = -1; // -1 : 沒有 task 在跑

static int find_best_task(void){
    int best = -1;
    for(int i = 0; i < MAX_TASKS; i++){
        if(!task_queue[i].active) continue;
        if(best < 0 || task_queue[i].priority > task_queue[best].priority){
           best = i;
        }
    }
    return best;
}

void add_task(task_callback_t callback, void *arg, int priority){
    // 關中斷
    unsigned long sstatus;
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus));

    //填入 task_queue
    int added = 0;
    for(int i = 0; i < MAX_TASKS; i++){
        if(!task_queue[i].active){
            task_queue[i].callback = callback;
            task_queue[i].arg = arg;
            task_queue[i].priority = priority;
            task_queue[i].active = 1;
            added = 1;
            break;
        }
    }

    // 還原中斷
    asm volatile("csrw sstatus, %0" :: "r"(sstatus));

    // 未能填入的除錯
    if(!added){
        uart_puts("add_task: queue full\n");
    }
}

void run_tasks(void){
    while(1){
        // close interrupt
        unsigned long sstatus;
        asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus));

        int best = find_best_task();
        if(best < 0 || task_queue[best].priority <= current_task_priority){
            asm volatile("csrw sstatus, %0" :: "r"(sstatus));
            break;
        }
        
        // 取出 best task
        task_callback_t cb = task_queue[best].callback;
        void *arg = task_queue[best].arg;
        int priority = task_queue[best].priority;
        task_queue[best].active = 0;

        int prev_priority = current_task_priority;
        current_task_priority = priority;
        
        // open interrupt
        asm volatile("csrs sstatus, 0x2");
        cb(arg);

        // close interrupt
        asm volatile("csrrci zero, sstatus, 0x2"); // 寫進 x0 -> 不需要保留原本讀出的值
        current_task_priority = prev_priority;
    }
}