#include "syscall.h"
#include "task.h"
#include "mem_allocator.h"
#include "cpio.h"
#include "utils.h"
#include "uart.h"
#include "timer.h"
#include "video.h"
#include "vm.h"
#include "mmap.h"

#define USER_PATH_MAX 256

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
    struct task_struct *cur = get_current();

    if (!buf || count <= 0 || !cur->pgd)
        return 0;

    for (long i = 0; i < count; i++) {
        char ch = uart_getc();

        if (copy_to_user_pgd(cur->pgd, buf + i, &ch, 1) < 0)
            return (i > 0) ? i : -1;
    }
    return count;
} // 1

/* Write count bytes from buf. Return the number of bytes written. */
long sys_uart_write(const char *buf, long count){
    struct task_struct *cur = get_current();
    unsigned long sstatus_save;
    long written = 0;
    char chunk_buf[128];

    if (!buf || count <= 0 || !cur->pgd)
        return 0;

    // Keep each write() contiguous on the console so preemption
    // does not interleave bytes from different processes.
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus_save)); // close interrupt, 保存目前 sstatus 進 sstatus_save

    while (written < count) {
        long chunk = count - written;

        if (chunk > (long)sizeof(chunk_buf))
            chunk = (long)sizeof(chunk_buf);
        if (copy_from_user_pgd(cur->pgd, chunk_buf, buf + written,
                               (unsigned long)chunk) < 0) {
            asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));
            return (written > 0) ? written : -1;
        }
        for (long i = 0; i < chunk; i++)
            uart_putc(chunk_buf[i]);
        written += chunk;
    }

    asm volatile("csrw sstatus, %0" :: "r"(sstatus_save)); // 恢復原本中斷狀態
    return written;
} // 2

int sys_exec(struct pt_regs *regs, const char *path){
    struct task_struct *cur = get_current();
    char kpath[USER_PATH_MAX]; // kernel buffer, kernel stack 上開的空間, 1 byte 1 byte
    unsigned long code_va;
    unsigned long code_size;

    if (!cur->pgd)
        return -1;
    if (copy_string_from_user_pgd(cur->pgd, kpath, path, sizeof(kpath)) < 0) // path（user VA）指向的字串，一個 byte 一個 byte 讀出來，寫進 kernel stack 上的 kpath
        return -1;

    code_va   = cpio_find_exec(kpath); // kernel 可取的 VA
    code_size = cpio_find_exec_size(kpath);
    if (!code_va || !code_size) return -1;

    unsigned long *new_pgd   = alloc_user_pgd();
    if (!new_pgd) {
        if (new_pgd)    free_user_pgd(new_pgd);
        return -1;
    }

    // 先切到新頁表再釋放舊的，確保 kernel 在切換過程中有效
    unsigned long *old_pgd = cur->pgd;
    cur->pgd = new_pgd;
    // 建立 4 個 VMA 對應 test, stack, signal_stack, trampoline，但不分配 page frames
    if (setup_user_exec_vmas(cur, code_va, code_size) < 0) {
        cur->pgd = old_pgd;
        free_user_pgd(new_pgd);
        return -1;
    }

    // 清除舊程式狀態
    cur->user_stack = USER_STACK_VA;
    cur->user_entry = USER_CODE_VA;
    cur->user_sp    = USER_STACK_VA + PAGE_SIZE;
    memset(cur->signal_handler, 0, sizeof(cur->signal_handler)); // 清除 signal handler (新的程式重新註冊)
    cur->signal_pending = 0;
    cur->in_signal = 0;
    memset(&cur->signal_context, 0, sizeof(cur->signal_context));

    asm volatile("csrw satp, %0\nsfence.vma zero, zero\n"
                 : : "r"(MAKE_SATP(VA_TO_PA((unsigned long)new_pgd))) : "memory");

    // 舊位址空間（含 code + stack 資料頁）由 free_user_pgd 一次釋放
    if (old_pgd) free_user_pgd(old_pgd);

    // 重建 trap frame 讓新程式從乾淨狀態開始
    memset(regs, 0, sizeof(*regs));
    regs->sepc    = USER_CODE_VA;
    regs->sp      = USER_STACK_VA + PAGE_SIZE;
    regs->tp      = (unsigned long)cur;
    regs->sstatus = (1UL << 5);  // SPIE=1, SPP=0
    return 0;
} // 3

/*
fork() 的核心目標
讓 child process 從 parent 的 fork() 呼叫點繼續執行，
但 child 的回傳值為 0，parent 的回傳值為 child 的 pid。
*/
long sys_fork(struct pt_regs *regs){
    struct task_struct *cur = get_current();

    unsigned long ckernel    = (unsigned long)buddy_alloc(0); // kernel stack
    unsigned long *child_pgd = alloc_user_pgd();
    struct task_struct *child = allocate(sizeof(*child)); // task_struct

    if (!ckernel || !child_pgd || !child) {
        if (ckernel)   buddy_free((void *)ckernel);
        if (child_pgd) free_user_pgd(child_pgd);
        if (child)     free(child);
        return -1;
    }

    // CoW 複製 page 
    if (fork_user_pages_cow(cur->pgd, child_pgd) < 0) {
        buddy_free((void *)ckernel);
        free_user_pgd(child_pgd);
        free(child);
        return -1;
    }

    // 在 child kernel stack 上建立 trap frame
    unsigned long child_ksp = ckernel + PAGE_SIZE;
    struct pt_regs *child_regs = (struct pt_regs *)(child_ksp - TRAP_FRAME_SIZE);
    *child_regs = *regs;
    child_regs->a0 = 0;  // child fork() 回傳 0

    // sp 維持相對 user stack top 的偏移不變
    unsigned long sp_offset = (USER_STACK_VA + PAGE_SIZE) - regs->sp;
    child_regs->sp = USER_STACK_VA + PAGE_SIZE - sp_offset;

    memset(child, 0, sizeof(*child));
    // child 繼承 parent 的 signal handlers
    for (int i = 0; i < MAX_SIGNALS; i++)
        child->signal_handler[i] = cur->signal_handler[i];
    mem_cpy(child->vmas, cur->vmas, sizeof(cur->vmas));
    child->pid        = nr_threads++;
    child->state      = READY_THREAD;
    child->stack      = ckernel;
    child->kernel_sp  = child_ksp;
    child->user_stack = USER_STACK_VA;
    child->user_sp    = USER_STACK_VA + PAGE_SIZE;
    child->user_entry = cur->user_entry;
    child->pgd        = child_pgd;
    child->parent_pid = cur->pid;
    child->wait_for_pid = -1;
    child_regs->tp    = (unsigned long)child;

    extern void ret_from_exception(void);
    child->thread.ra = (unsigned long)ret_from_exception;
    child->thread.sp = (unsigned long)child_regs;

    child->next = run_queue->next;
    run_queue->next = child;
    return child->pid;  // parent 的返回值
} // 4

/* 讓 parent 等待 child 結束，結束後回收 child 資源 */
long sys_waitpid(long pid){
    struct task_struct *cur = get_current();

    // 關中斷保護 check-then-set
    unsigned long sstatus_save;
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus_save)); // 清除 SIE

    struct task_struct *child = find_task_by_pid((int)pid);
    if (!child || child->state == ZOMBIE_THREAD) {
        if (child) {
            // 從 run queue 移除 child
            struct task_struct *t = run_queue;
            while (t->next != run_queue && t->next != child) t = t->next;
            if (t->next == child) t->next = child->next;
            // 釋放 user 位址空間（含所有資料頁）
            if (child->pgd) { free_user_pgd(child->pgd); child->pgd = NULL; }
            buddy_free((void *)child->stack);
            free(child);
        }
        asm volatile("csrw sstatus, %0" :: "r"(sstatus_save));
        return pid;
    }

    // child 還在跑，設 WAITING 並讓出 CPU
    cur->wait_for_pid = (int)pid;
    cur->state = WAITING_THREAD;
    asm volatile("csrw sstatus, %0" :: "r"(sstatus_save)); // 還原中斷
    schedule();
    
    // schedule return 的恢復點在 context_switch 反回的地方
    // 行程被 scheduler 重新選中, cpu 控制權交回來
    // context_switch 切走時儲存的 ra（return address）指向 schedule() 返回後的下一條指令

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

    // 釋放 user 位址空間（含 code + stack 所有資料頁）；kernel stack 在 kill_zombies 清理
    if (cur->pgd) { free_user_pgd(cur->pgd); cur->pgd = NULL; }
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
    if (pid == 0 || pid == 1) return -1;
    if ((int)pid == get_current()->pid) return -1;
    struct task_struct *t = find_task_by_pid((int)pid);
    if (!t) return -1;
    if (t->pgd) { free_user_pgd(t->pgd); t->pgd = NULL; }
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
    struct task_struct *cur = get_current();

    if (!bmp_image || width == 0 || height == 0)
        return;

    if (cur->pgd)
        (void)video_display_user(bmp_image, width, height, cur->pgd);
    else
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
