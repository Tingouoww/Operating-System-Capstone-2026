#include "task.h"
#include "uart.h"
#include "mem_allocator.h"
#include "utils.h"
#include "cpio.h"

#define MAX_TASKS 64

struct task_entry{
    task_callback_t callback;
    void *arg;
    int priority;
    int active;
};

int nr_threads = 0;
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

    while(next->state == ZOMBIE_THREAD || next->state == WAITING_THREAD)
        next = next->next;

    if (next == current) return;
    
    if (current->state == RUNNING_THREAD) current->state = READY_THREAD;
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
    idle_task->parent_pid = -1; 
    idle_task->wait_for_pid = -1;
    run_queue = idle_task;
    
    asm volatile("mv tp, %0" :: "r"(idle_task) : "memory"); 
}

void idle() {
    while (1) {
        kill_zombies();
        schedule();
    }
}

struct task_struct *find_task_by_pid(int pid) {
    struct task_struct *t = run_queue;
    do {
        if (t->pid == pid) return t;
        t = t->next;
    } while (t != run_queue);
    return NULL;
}

/* 從 run_queue 開始走一圈, 遇到 ZOMBIE_THREAD 就釋放資源 */
void kill_zombies(){
    struct task_struct *prev = run_queue;
    struct task_struct *cur = run_queue->next;
    
    while(cur != run_queue){
        struct task_struct *next = cur->next;
        if (cur->state == ZOMBIE_THREAD) {
            int parent_waiting = 0;
            if (cur->parent_pid >= 0) {
                struct task_struct *parent = find_task_by_pid(cur->parent_pid);
                if (parent && parent->state == WAITING_THREAD &&
                    parent->wait_for_pid == cur->pid)
                    parent_waiting = 1;
            }
            if (!parent_waiting) {
                prev->next = next;
                if (cur->user_stack) buddy_free((void *)cur->user_stack);
                buddy_free((void *)cur->stack);
                free(cur);
                // prev 不移動
            } else {
                prev = cur;  // 留著讓 sys_waitpid 清理
            }
        } else {
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
    t->parent_pid = -1;
    t->wait_for_pid = -1;
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

/* ------------------------- user exec --------------------------*/

int user_exec(const char *filename) {
    // CPIO 掃描找 user_entry
    unsigned long user_entry = cpio_find_exec(filename);
    if(!user_entry) return -1;

    // 分配 kernel stack 和 user stack
    unsigned long kstack = (unsigned long)buddy_alloc(0);
    unsigned long ustack = (unsigned long)buddy_alloc(0);
    if(!kstack || !ustack) {
        if (kstack) buddy_free((void *)kstack);
        if (ustack) buddy_free((void *)ustack);
        return -1;
    }

    unsigned long kernel_sp = kstack + PAGE_SIZE;
    unsigned long user_sp   = ustack + PAGE_SIZE;

    // 在 kernel stack 頂端建立 fake trap frame
    struct pt_regs *regs = (struct pt_regs *)(kernel_sp - TRAP_FRAME_SIZE);
    memset(regs, 0, TRAP_FRAME_SIZE);
    regs->sepc    = user_entry;
    regs->sp      = user_sp;
    // sstatus: SPP=0 (U-mode), SPIE=1 (enable interrupt)
    regs->sstatus = (1UL << 5);  // SPIE=1, SPP=0

    // 分配並初始化 task_struct
    struct task_struct *t = allocate(sizeof(*t));
    if (!t) {
        buddy_free((void *)kstack);
        buddy_free((void *)ustack);
        return -1;
    }
    memset(t, 0, sizeof(*t));
    t->pid         = nr_threads++;
    t->state       = READY_THREAD;
    t->stack       = kstack;
    t->kernel_sp   = kernel_sp;
    t->user_stack  = ustack;
    t->user_sp     = user_sp;
    t->user_entry  = user_entry;
    t->parent_pid  = get_current()->pid;
    t->wait_for_pid = -1;

    regs->tp      = (unsigned long)t;

    // 設定 thread 讓 switch_to 後跳到 ret_from_exception
    extern void ret_from_exception(void);
    t->thread.ra = (unsigned long)ret_from_exception;
    t->thread.sp = (unsigned long)regs;

    // 加入 run_queue
    t->next = run_queue->next;
    run_queue->next = t;
    return t->pid;
}
