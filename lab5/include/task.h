#ifndef TASK_H
#define TASK_H

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
    struct task_struct *next;
};

typedef void (*task_callback_t)(void *arg);
void add_task(task_callback_t callback, void *arg, int priority);
void run_tasks(void);

struct task_struct *get_current(void);
struct task_struct *thread_create(void (*threadfn)(void));
void thread_exit(void);
void kill_zombies(void);
void idle_init(void);
void idle(void);
void schedule(void);

#endif