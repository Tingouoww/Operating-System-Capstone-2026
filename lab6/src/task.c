#include "task.h"
#include "uart.h"
#include "mem_allocator.h"
#include "utils.h"
#include "cpio.h"
#include "vm.h"
#include "mmap.h"

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

/* 獲取目前正在執行的 thread */
struct task_struct* get_current() {
    register struct task_struct* current asm("tp"); // tp = thread pointer, 指向 current task_struct
    return current;
}

extern void switch_to(struct task_struct* prev, struct task_struct* next);

/* 切換記憶體位址空間 */
static void switch_mm(struct task_struct *next) {
    unsigned long pgd_pa;
    if (next->pgd) {
        pgd_pa = VA_TO_PA((unsigned long)next->pgd);
    } else {
        // kernel thread：用全域 kernel PGD
        extern unsigned long pgd[];
        pgd_pa = VA_TO_PA((unsigned long)pgd);
    }
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        : : "r"(MAKE_SATP(pgd_pa)) : "memory"
    );
}

void schedule() {
    struct task_struct* current = get_current();
    struct task_struct* next = current->next;

    while(next->state == ZOMBIE_THREAD ||
          next->state == WAITING_THREAD ||
          next->state == SLEEPING_THREAD)
        next = next->next;

    if (next == current) return;
    
    if (current->state == RUNNING_THREAD) current->state = READY_THREAD;
    next->state = RUNNING_THREAD;
    switch_mm(next);           // 切換位址空間
    switch_to(current, next);
}

void idle_init(void){
    struct task_struct *idle_task = 
        (struct task_struct *) allocate(sizeof(struct task_struct));
    memset(idle_task, 0, sizeof(*idle_task)); // 先把整個結構清成 0，避免欄位有亂值
    idle_task->pid = nr_threads++;
    idle_task->state = RUNNING_THREAD;
    idle_task->next = idle_task; // 初始化只有一個 node 的環狀 linked list
    idle_task->parent_pid = -1; 
    idle_task->wait_for_pid = -1;
    run_queue = idle_task;
    
    /* 
    把 idle_task 的位址放進 RISC-V 的 tp 暫存器
    get_current() 會直接從 tp 取出目前 task
    */
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
                if (cur->pgd) { free_user_pgd(cur->pgd); cur->pgd = NULL; }
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
    stack_top &= ~0xFUL; // 16-byte 對齊, 清掉低 4 bits

    t->pid = nr_threads++;
    t->state = READY_THREAD;
    t->stack = stack_base;
    t->parent_pid = -1;
    t->wait_for_pid = -1;
    t->thread.sp = stack_top;
    t->thread.ra = (unsigned long)threadfn;
    t->next = run_queue->next; // 插入 run_queue 後面 (緊接在 idle 後面)
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
/*  user_exec 不會跳進 U-mode，
    它是建立一個新的 task_struct 排進 run_queue，
    然後讓 scheduler 在之後切換過去 */
int user_exec(const char *filename) {
    unsigned long code_va = cpio_find_exec(filename);
    unsigned long code_size = cpio_find_exec_size(filename);
    if(!code_va || !code_size) return -1;

    // 只配置 kernel stack 與 PGD，user pages 由 fault handler 按需補齊
    unsigned long kstack = (unsigned long)buddy_alloc(0);
    unsigned long *proc_pgd = alloc_user_pgd();
    if(!kstack || !proc_pgd) {
        if (kstack)   buddy_free((void *)kstack);
        if (proc_pgd) free_user_pgd(proc_pgd);
        return -1;
    }

    unsigned long kernel_sp = kstack + PAGE_SIZE; // 由頂端往下
    // 在 kernel stack 頂端建立 fake trap frame
    // 新建立的 process 沒有真的發生過 trap/syscall/interrupt
    // kernel stack 上面不存在 trap frame
    struct pt_regs *regs = (struct pt_regs *)(kernel_sp - TRAP_FRAME_SIZE); // 挪出空間給 pt_regs 使用, regs -> trap frame 起始位址
    memset(regs, 0, TRAP_FRAME_SIZE);
    regs->sepc    = USER_CODE_VA;
    regs->sp      = USER_STACK_VA + PAGE_SIZE;
    // sstatus: SPP=0 (U-mode), SPIE=1 (enable interrupt)
    regs->sstatus = (1UL << 5);  // SPIE=1, SPP=0

    // 分配並初始化 task_struct
    struct task_struct *t = allocate(sizeof(*t));
    if (!t) {
        buddy_free((void *)kstack);
        free_user_pgd(proc_pgd);
        return -1;
    }
    memset(t, 0, sizeof(*t));
    if (setup_user_exec_vmas(t, code_va, code_size) < 0) {
        buddy_free((void *)kstack);
        free_user_pgd(proc_pgd);
        free(t);
        return -1;
    }
    t->pid         = nr_threads++;
    t->state       = READY_THREAD;
    t->stack       = kstack;
    t->kernel_sp   = kernel_sp;
    t->user_stack  = USER_STACK_VA;
    t->user_sp     = USER_STACK_VA + PAGE_SIZE;
    t->user_entry  = USER_CODE_VA;
    t->pgd         = proc_pgd;
    t->parent_pid  = get_current()->pid;
    t->wait_for_pid = -1;

    regs->tp      = (unsigned long)t;
    /* 對新建的 user process 來說它以前從來沒有真的跑過，
    所以沒有一個現成的 kernel call stack 可以繼續。
    因此幫它造出第一個落點，而那個落點是 ret_from_exception。*/
    // 設定 thread 讓 switch_to 後跳到 ret_from_exception
    extern void ret_from_exception(void);
    t->thread.ra = (unsigned long)ret_from_exception;
    t->thread.sp = (unsigned long)regs;

    // 加入 run_queue
    t->next = run_queue->next;
    run_queue->next = t;
    return t->pid;
}
