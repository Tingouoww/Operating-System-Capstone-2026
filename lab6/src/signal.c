#include "signal.h"
#include "task.h"
#include "syscall.h"
#include "uart.h"
#include "mem_allocator.h"
#include "utils.h"
#include "vm.h"

void signal_init(void) {
    /*
     * Signal trampoline 改為每個 process 各自 map 一頁到固定的 user VA，
     * signal_init 目前不需要做全域配置。
     */
}

int signal_setup_user_pages(unsigned long *proc_pgd) {
    unsigned long trampoline_page;
    unsigned long signal_stack_page;
    unsigned int *trampoline_code;

    if (!proc_pgd)
        return -1;

    trampoline_page = (unsigned long)buddy_alloc(0);
    signal_stack_page = (unsigned long)buddy_alloc(0);
    if (!trampoline_page || !signal_stack_page) {
        if (trampoline_page)
            buddy_free((void *)trampoline_page);
        if (signal_stack_page)
            buddy_free((void *)signal_stack_page);
        return -1;
    }

    memset((void *)trampoline_page, 0, PAGE_SIZE);
    memset((void *)signal_stack_page, 0, PAGE_SIZE);

    trampoline_code = (unsigned int *)trampoline_page;
    trampoline_code[0] = 0x00b00893u;  // li a7, 11  (sigreturn syscall number)
    trampoline_code[1] = 0x00000073u;  // ecall

    map_pages(proc_pgd, USER_TRAMPOLINE_VA, PAGE_SIZE,
              VA_TO_PA(trampoline_page), PROT_USER_RX);
    map_pages(proc_pgd, USER_SIGNAL_STACK_VA, PAGE_SIZE,
              VA_TO_PA(signal_stack_page), PROT_USER_RW);
    return 0;
}

// 10
long sys_signal(int signum, void (*handler)(int)){
    if(signum <= 0 || signum >= MAX_SIGNALS) return -1;
    struct task_struct *cur = get_current();
    void (*old)(int) = cur->signal_handler[signum];
    cur->signal_handler[signum] = handler;
    return (long)old;
}

// 11
void sys_sigreturn(struct pt_regs *regs){
    uart_puts("[sigreturn] signal handler finished\n");

    struct task_struct *cur = get_current();

    // 還原原始 context，覆蓋整個 trap frame
    // 這樣 ret_from_exception 會用原本的 sepc/sp/regs 做 sret
    *regs = cur->signal_context.save_regs;
    cur->signal_context.signal_stack = 0;
    cur->in_signal = 0;
}

// 12
int sys_kill(int pid, int signum){
    if(signum <= 0 || signum >= MAX_SIGNALS) return -1;
    struct task_struct *t = find_task_by_pid(pid);
    if(!t) return -1;

    if(!t->signal_handler[signum] || t->signal_handler[signum] == SIG_DFL){
        sys_stop(pid);
    } else{
        t->signal_pending |= (1UL << signum);
    }
    return 0;
}

void do_signal(struct pt_regs *regs) {
    // 只對返回 user mode 的 trap 處理（SPP bit8 == 0）
    if (regs->sstatus & (1UL << 8)) return;

    struct task_struct *cur = get_current();
    if (!cur->signal_pending || cur->in_signal) return;

    int signum = 0;
    while (signum < MAX_SIGNALS && !(cur->signal_pending & (1UL << signum)))
        signum++;
    cur->signal_pending &= ~(1UL << signum);

    void (*handler)(int) = cur->signal_handler[signum];
    if (!handler || handler == SIG_DFL) return;

    // 保存原始 trap frame
    cur->signal_context.save_regs = *regs; // handler 跑完之後，sigreturn 會把這份備份還原

    // 修改 trap frame → sret 將跳去執行 handler（U-mode）
    regs->sepc = (unsigned long)handler;       // handler 進入點
    regs->sp   = USER_SIGNAL_STACK_VA + PAGE_SIZE;
    regs->ra   = USER_TRAMPOLINE_VA;           // handler return → trampoline → sigreturn
    regs->a0   = (unsigned long)signum;        // 傳入信號號碼

    cur->in_signal = 1;
}