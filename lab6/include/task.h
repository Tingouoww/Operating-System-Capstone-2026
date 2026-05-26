#ifndef TASK_H
#define TASK_H

#include "pt_regs.h"
#include "signal.h"

#define TRAP_FRAME_SIZE (35 * 8)  // 280 bytes
// THREAD STATE
#define READY_THREAD 0
#define RUNNING_THREAD 1
#define ZOMBIE_THREAD 2
#define WAITING_THREAD 3
#define SLEEPING_THREAD 4

extern int nr_threads; // Thread Counter
extern struct task_struct* run_queue;

struct task_struct {
    struct thread_struct {
        unsigned long ra; // function callback address
        unsigned long sp; // stack pointer, kernel 執行時的 stack 指標, 目前或切換時要恢復的 stack pointer 值
        unsigned long s[12]; // callee-saved regs
    } thread;
    int pid;
    int state;
    unsigned long kernel_sp;
    unsigned long user_sp;
    unsigned long stack; // kernel stack base address
    unsigned long user_stack; // user stack base address(VA)
    unsigned long user_entry; // user program 開始執行的入口位址(VA)
    unsigned long *pgd; // process 的 PGD(VA), null 表示 kernel thread
    int parent_pid; // 父 process pid（-1 表示無父）
    int wait_for_pid; // 正在等待哪個 child pid（-1 = 沒有等待）
    int exit_status;  // 子行程結束時把結果交給父行程 (但目前其實沒用到, spec 說lab不用)
    struct task_struct *next; // schedule use

    // ---- POSIX ----
    void (*signal_handler[MAX_SIGNALS])(int); // 函式指標陣列 (total 32 欄位)
    unsigned long signal_pending;
    int in_signal; // 是否在執行 signal handler
    struct saved_signal_context signal_context;
};

typedef void (*task_callback_t)(void *arg);
void add_task(task_callback_t callback, void *arg, int priority);
void run_tasks(void);// 父 process pid（-1 表示無父）

struct task_struct *get_current(void);
struct task_struct *thread_create(void (*threadfn)(void));
void thread_exit(void);
void kill_zombies(void);
void idle_init(void);
void idle(void);
void schedule(void);

struct task_struct *find_task_by_pid(int pid);
int user_exec(const char *filename); // 建立 user process task，回傳 pid 或 -1

#endif
