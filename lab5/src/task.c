#include "task.h"
#include "uart.h"
#include "mem_allocator.h"
#include "string.h"

#define MAX_TASKS 64

// THREAD STATE
#define READY_THREAD 0
#define RUNNING_THREAD 1
#define ZOMBIE_THREAD 2

struct task_entry{
    task_callback_t callback;
    void *arg;
    int priority;
    int active;
};

int nr_threads = 0; // Thread Counter
struct task_struct* run_queue = 0;

static struct task_entry task_queue[MAX_TASKS];
static volatile int current_task_priority = -1; // -1 : 沒有 task 在跑

struct task_struct* get_current() {
    register struct task_struct* current asm("tp");
    return current;
}

extern void switch_to(struct task_struct* prev, struct task_struct* next);

void schedule() {
    struct task_struct* current = get_current();
    struct task_struct* next = current->next;

    while(next->state == ZOMBIE_THREAD) next = next->next;

    if (next == current) return;
    
    if (current->state != ZOMBIE_THREAD) current->state = READY_THREAD;
    next->state = RUNNING_THREAD;
    switch_to(current, next);
}

void idle_init(void){
    struct task_struct *idle_task = 
        (struct task_struct *) allocate(sizeof(struct task_struct));
    memset(idle_task, 0, sizeof(*idle_task));
    idle_task->pid = nr_threads++;
    idle_task->state = RUNNING_THREAD;
    idle_task->next = idle_task;
    run_queue = idle_task;
    
    asm volatile("mv tp, %0" :: "r"(idle_task) : "memory"); 
}

void idle() {
    while (1) {
        kill_zombies();
        schedule();
    }
}

/* 從 run_queue 開始走一圈, 遇到 ZOMBIE_THREAD 就釋放資源 */
void kill_zombies(){
    struct task_struct *prev = run_queue;
    struct task_struct *cur = run_queue->next;
    
    while(cur != run_queue){
        struct task_struct *next = cur->next;
        if(cur->state == ZOMBIE_THREAD){
            prev->next = next;
            buddy_free((void*)cur->stack);
            free(cur);
        }
        else{
            prev = cur;
        }
        cur = next;
    }
    
}

struct task_struct* thread_create(void (*threadfn)())
{ 
    struct task_struct* t = (struct task_struct*) allocate(sizeof(struct task_struct));
    if (!t) return NULL;

    unsigned long stack_base;
    unsigned long stack_top;

    stack_base = (unsigned long) buddy_alloc(0); // 配置一頁 kernel stack
    if(!stack_base){
        free(t);
        return NULL;
    }

    memset(t, 0, sizeof(*t));

    stack_top = stack_base + PAGE_SIZE;
    stack_top &= ~0xFUL; // 16-byte 對齊

    t->pid = nr_threads++;
    t->state = READY_THREAD;
    t->stack = stack_base;
    t->thread.sp = stack_top;
    t->thread.ra = (unsigned long)threadfn;
    t->next = run_queue->next;
    run_queue->next = t;

    return t;
};

void thread_exit(){
    get_current()->state = ZOMBIE_THREAD; // 把目前的 thread 標成 zombie 等待清理
    schedule(); 
}

/* --------- task ---------- */

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