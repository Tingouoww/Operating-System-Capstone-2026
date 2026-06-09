#ifndef TASK_H
#define TASK_H

#include "pt_regs.h"
#include "signal.h"

#define TRAP_FRAME_SIZE (35 * 8)  // 280 bytes
#define TASK_MAX_FD 16
// THREAD STATE
#define READY_THREAD 0
#define RUNNING_THREAD 1
#define ZOMBIE_THREAD 2
#define WAITING_THREAD 3
#define SLEEPING_THREAD 4

#define MAX_MMAP_AREAS 16

extern int nr_threads; // Thread Counter
extern struct task_struct* run_queue;

enum vma_type {
    VMA_ANONYMOUS = 0, // general data R/W , 不對應檔案
    VMA_TEXT, // code R/X
    VMA_STACK, // R/W
    VMA_SIGNAL_STACK, // R/W
    VMA_TRAMPOLINE, // R/W
};

struct vma{ // virtual memory area
    unsigned long start;
    unsigned long end;

    int prot; // READ, WRITE, EXEC
    int flags; // ANONYMOUS, POPULATE
    int used;

    int type; // vma_type
    unsigned long src; // VMA_TEXT: 程式碼來源 kernel VA
    unsigned long src_len; // VMA_TEXT: 程式碼長度
};

struct vnode;
struct file;

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
    struct vnode *root_dir;
    struct vnode *cwd;
    struct file *fd_table[TASK_MAX_FD];

    // ---- POSIX ----
    void (*signal_handler[MAX_SIGNALS])(int); // 函式指標陣列 (total 32 欄位)
    unsigned long signal_pending;
    int in_signal; // 是否在執行 signal handler
    struct saved_signal_context signal_context;

    // mmap
    struct vma vmas[MAX_MMAP_AREAS];
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
void task_init_fs_context(struct task_struct *task);
void task_clone_fs_context(struct task_struct *dst,
                           const struct task_struct *src);
void task_release_fs_context(struct task_struct *task);
int task_install_file(struct task_struct *task, struct file *file);
struct file *task_get_file(struct task_struct *task, int fd);
int task_close_fd(struct task_struct *task, int fd);

#endif
