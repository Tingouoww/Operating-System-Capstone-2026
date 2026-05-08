#ifndef TASK_H
#define TASK_H

#include "pt_regs.h"

#define TRAP_FRAME_SIZE (35 * 8)  // 280 bytes
// THREAD STATE
#define READY_THREAD 0
#define RUNNING_THREAD 1
#define ZOMBIE_THREAD 2
#define WAITING_THREAD 3

extern int nr_threads; // Thread Counter
extern struct task_struct* run_queue;

struct task_struct {
    struct thread_struct {
        unsigned long ra;
        unsigned long sp;
        unsigned long s[12];
    } thread;
    int pid;
    int state;
    unsigned long kernel_sp;
    unsigned long user_sp;
    unsigned long stack;
    unsigned long user_stack;
    unsigned long user_entry;
    int parent_pid; // 父 process pid（-1 表示無父）
    int wait_for_pid; // 正在等待哪個 child pid（-1 = 沒有等待）
    int exit_status; 
    struct task_struct *next;
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
