#include "signal.h"
#include "task.h"
#include "syscall.h"
#include "uart.h"
#include "mem_allocator.h"

unsigned long trampoline_uaddr;

void signal_init(void) {
    // 分配一頁記憶體當 trampoline
    unsigned long page = (unsigned long)buddy_alloc(0);  // order 0 = 4KB
    if (!page) {
        uart_puts("signal_init: failed to alloc trampoline\n");
        return;
    }

    // 寫入兩條 RISC-V 指令
    unsigned int *t = (unsigned int *)page;
    t[0] = 0x00b00893u;  // li a7, 11  (sigreturn syscall number)
    t[1] = 0x00000073u;  // ecall

    // 記住這個位址，之後 do_signal 把 handler 的 ra 設成它
    trampoline_uaddr = page;

    /*
    trampoline_page 就是 kernel 幫 user 準備好的回家的位址，
    讓 handler 結束後能自動呼叫 sigreturn 回到 kernel。
    */
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

    if(cur->signal_context.signal_stack){
        buddy_free((void*)cur->signal_context.signal_stack);
        cur->signal_context.signal_stack = 0;
    }

    // 還原原始 context，覆蓋整個 trap frame
    // 這樣 ret_from_exception 會用原本的 sepc/sp/regs 做 sret
    *regs = cur->signal_context.save_regs;
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

    // 分配 signal stack
    unsigned long sig_stack = (unsigned long)buddy_alloc(0); // WHY
    if (!sig_stack) return;
    cur->signal_context.signal_stack = sig_stack;

    // 修改 trap frame → sret 將跳去執行 handler（U-mode）
    regs->sepc = (unsigned long)handler;       // handler 進入點
    regs->sp   = sig_stack + PAGE_SIZE;        // 獨立的 signal stack
    regs->ra   = trampoline_uaddr;             // handler return → trampoline → sigreturn
    regs->a0   = (unsigned long)signum;        // 傳入信號號碼

    cur->in_signal = 1;
}

// WHY 做 SIGNAL , 為甚麼不在 TRAP 裡面做
// 