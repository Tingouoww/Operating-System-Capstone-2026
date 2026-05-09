#include "syscall.h"
#include "task.h"
#include "mem_allocator.h"
#include "cpio.h"
#include "utils.h"
#include "uart.h"
#include "timer.h"
#include "video.h"

static void wake_sleeping_task(void *arg) {
    int pid = (int)(unsigned long)arg;
    struct task_struct *task = find_task_by_pid(pid);

    if (task && task->state == SLEEPING_THREAD)
        task->state = READY_THREAD;
}

long sys_getpid(void){
    return get_current()->pid;
} // 0

/* Read count bytes into buf. Return the number of bytes read. */
long sys_uart_read(char *buf, long count){
    for (long i = 0; i < count; i++)
        buf[i] = uart_getc();
    return count;
} // 1

/* Write count bytes from buf. Return the number of bytes written. */
long sys_uart_write(const char *buf, long count){
    unsigned long sstatus_save;

    if (!buf || count <= 0)
        return 0;

    // Keep each write() contiguous on the console so preemption
    // does not interleave bytes from different processes.
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus_save));
    for (long i = 0; i < count; i++)
        uart_putc(buf[i]);
    asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));
    return count;
} // 2

int sys_exec(struct pt_regs *regs, const char *path){
    // 找到新程式
    unsigned long new_entry = cpio_find_exec(path);
    if (!new_entry) return -1;

    struct task_struct *cur = get_current();
    unsigned long new_ustack = (unsigned long)buddy_alloc(0);
    unsigned long new_user_sp;

    if (!new_ustack) return -1;

    // 新 stack 準備好後再替換，避免 exec 失敗時把目前 process 弄壞
    if (cur->user_stack) buddy_free((void *)cur->user_stack);
    new_user_sp = new_ustack + PAGE_SIZE;
    cur->user_stack = new_ustack;
    cur->user_sp    = new_user_sp;
    cur->user_entry = new_entry;

    // 重建 user-visible trap frame，讓新程式從乾淨狀態開始
    memset(regs, 0, sizeof(*regs));
    regs->sepc = new_entry;
    regs->sp   = new_user_sp;
    regs->tp   = (unsigned long)cur;
    regs->sstatus = (1UL << 5);  // SPIE=1, SPP=0
    return 0;
} // 3

long sys_fork(struct pt_regs *regs){
    struct task_struct *cur = get_current();

    unsigned long ckernel = (unsigned long)buddy_alloc(0);
    unsigned long cuser = (unsigned long)buddy_alloc(0);
    struct task_struct *child;
    unsigned long child_ksp = ckernel + PAGE_SIZE;
    unsigned long child_usp = cuser + PAGE_SIZE;

    if (!ckernel || !cuser) {
        if (ckernel) buddy_free((void *)ckernel);
        if (cuser) buddy_free((void *)cuser);
        return -1;
    }

    // COPY USER STACK ( 保存user 程式的執行現場 )
    mem_cpy((void*)cuser, (void*)cur->user_stack, PAGE_SIZE);

    // 在 child kernel stack 上 copy trap frame
    struct pt_regs *child_regs = (struct pt_regs *)(child_ksp - TRAP_FRAME_SIZE);
    *child_regs = *regs; // copy 整個 trap frame

    // child 的 fork() return value 為 0
    child_regs->a0 = 0;

    // 修正 sp : 維持相對於 user stack top 的偏移
    unsigned long sp_offset = cur->user_sp - regs->sp;
    child_regs->sp = child_usp - sp_offset;
    
    // 建立 child task_struct
    child = allocate(sizeof(*child));
    if (!child) {
        buddy_free((void *)ckernel);
        buddy_free((void *)cuser);
        return -1;
    }
    memset(child, 0, sizeof(*child));
    child->pid = nr_threads++;
    child->state = READY_THREAD;
    child->stack      = ckernel;
    child->kernel_sp  = child_ksp;
    child->user_stack = cuser;
    child->user_sp    = child_usp;
    child->user_entry = cur->user_entry;
    child->parent_pid = cur->pid;
    child->wait_for_pid = -1;
    child_regs->tp = (unsigned long)child;

    extern void ret_from_exception(void);
    child->thread.ra = (unsigned long)ret_from_exception;
    child->thread.sp = (unsigned long)child_regs;

    child->next = run_queue->next;
    run_queue->next = child;

    return child->pid;  // parent 的返回值
} // 4

long sys_waitpid(long pid){
    struct task_struct *cur = get_current();

    // 關中斷保護 check-then-set
    unsigned long sstatus_save;
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus_save));

    struct task_struct *child = find_task_by_pid((int)pid);
    if (!child || child->state == ZOMBIE_THREAD) {
        if (child) {
            struct task_struct *t = run_queue;
            while (t->next != run_queue && t->next != child) t = t->next;
            if (t->next == child) t->next = child->next;
            if (child->user_stack) buddy_free((void *)child->user_stack);
            buddy_free((void *)child->stack);
            free(child);
        }
        asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));
        return pid;
    }

    // child 還在跑，設 WAITING 並讓出 CPU
    cur->wait_for_pid = (int)pid;
    cur->state = WAITING_THREAD;
    asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));
    schedule();

    // 被 sys_exit 喚醒後，清理 child
    child = find_task_by_pid((int)pid);
    if (child) {
        // 從 run_queue 移除並釋放 kernel stack + task_struct
        // (user_stack 已在 sys_exit 中釋放)
        buddy_free((void *)child->stack);
        // 從鏈結串列移除 child
        struct task_struct *t = run_queue;
        while (t->next != run_queue && t->next != child) t = t->next;
        if (t->next == child) t->next = child->next;
        free(child);
    }
    return pid;
} // 5

void sys_exit(int status){
    struct task_struct *cur = get_current();

    // 釋放 user stack（kernel stack 在 kill_zombies 清理）
    if (cur->user_stack) buddy_free((void *)cur->user_stack);
    cur->user_stack = 0;
    cur->exit_status = status;
    cur->state = ZOMBIE_THREAD;

    // 喚醒等待我的 parent
    if (cur->parent_pid >= 0) {
        struct task_struct *parent = find_task_by_pid(cur->parent_pid);
        if (parent && parent->state == WAITING_THREAD &&
            parent->wait_for_pid == cur->pid) {
            parent->state = READY_THREAD;
            parent->wait_for_pid = -1;
        }
    }
    schedule();
    // 不返回（state=ZOMBIE，不再被 schedule 選到）
} // 6

int sys_stop(long pid){
    struct task_struct *t = find_task_by_pid((int)pid);
    if (!t) return -1;
    if (t->user_stack) buddy_free((void *)t->user_stack);
    t->user_stack = 0;
    t->state = ZOMBIE_THREAD;
    // 喚醒可能等待此 pid 的 parent
    if (t->parent_pid >= 0) {
        struct task_struct *parent = find_task_by_pid(t->parent_pid);
        if (parent && parent->state == WAITING_THREAD &&
            parent->wait_for_pid == (int)pid) {
            parent->state = READY_THREAD;
            parent->wait_for_pid = -1;
        }
    }
    return 0;
} // 7

void sys_display(const unsigned int *bmp_image,
                 unsigned int width,
                 unsigned int height) {
    if (!bmp_image || width == 0 || height == 0)
        return;

    video_display(bmp_image, width, height);
} // 8

int sys_usleep(unsigned int usec){
    struct task_struct *cur = get_current();
    unsigned long sstatus_save;

    if (usec == 0)
        return 0;

    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus_save));
    cur->state = SLEEPING_THREAD;
    if (add_timer_us(wake_sleeping_task, (void *)(unsigned long)cur->pid, usec) < 0) {
        cur->state = RUNNING_THREAD;
        asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));
        return -1;
    }
    asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));

    schedule();
    return 0;
} // 9
