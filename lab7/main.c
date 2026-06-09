#include "uart.h"
#include "utils.h"
#include "mem_allocator.h"
#include "shell.h"
#include "cpio.h"
#include "fdt.h"
#include "bootloader.h"
#include "sbi.h"
#include "timer.h"
#include "task.h"
#include "pt_regs.h"
#include "syscall.h"
#include "video.h"
#include "mmap.h"

static void shell_thread(void) {
    char buf[128];
    int  len = 0;
    char c;
    print_shell_prompt();
    while (1) {
        c = uart_getc();
        if (c == '\n') {
            uart_putc('\n');
            buf[len] = '\0';
            run_command(buf);
            len = 0;
            print_shell_prompt();
        } else if ((c == '\b' || c == '\x7f') && len > 0) {
            uart_puts("\b \b");
            len--;
        } else if (len < (int)sizeof(buf) - 1) {
            buf[len++] = c;
            uart_putc(c);
        }
    }
}

#define SCAUSE_IRQ_FLAG         (1UL << 63)
#define SCAUSE_SUPERVISOR_TIMER 5
#define SCAUSE_ECALL_U 8
#define SCAUSE_SUPERVISOR_EXT   9
#define SCAUSE_INST_PAGE_FAULT  12
#define SCAUSE_LOAD_PAGE_FAULT  13
#define SCAUSE_STORE_PAGE_FAULT 15

void do_trap(struct pt_regs *regs) {
    if (regs->scause & SCAUSE_IRQ_FLAG) {
        unsigned long irq = regs->scause & ~SCAUSE_IRQ_FLAG;
        if (irq == SCAUSE_SUPERVISOR_TIMER)
            timer_handle_irq();
        else if (irq == SCAUSE_SUPERVISOR_EXT)
            uart_handle_external_irq();
        run_tasks();
        if (timer_consume_preempt_pending())
            schedule();
        regs->tp = (unsigned long)get_current();
    }
    else if(regs->scause == SCAUSE_ECALL_U){
        regs->sepc += 4;  // 先跳過 ecall（parent 和 fork child 的 sepc 都正確）

        // 在 syscall 執行期間重新開啟 S-mode 中斷，保持 kernel preemptible (system call  需要時間執行)
        asm volatile("csrs sstatus, 0x2");

        switch (regs->a7) {
            case 0: regs->a0 = sys_getpid(); break;
            case 1: regs->a0 = sys_uart_read((char *)regs->a0, (long)regs->a1); break;
            case 2: regs->a0 = sys_uart_write((const char *)regs->a0, (long)regs->a1); break;
            case 3: regs->a0 = sys_exec(regs, (const char *)regs->a0); break;
            case 4: regs->a0 = sys_fork(regs); break;
            case 5: regs->a0 = sys_waitpid((long)regs->a0); break;
            case 6: sys_exit((int)regs->a0); break;  // 不返回
            case 7: regs->a0 = sys_stop((long)regs->a0); break;
            case 8: sys_display((const unsigned int *)regs->a0,
                                (unsigned int)regs->a1,
                                (unsigned int)regs->a2); break;
            case 9: regs->a0 = sys_usleep((unsigned int)regs->a0); break;
            case 10: regs->a0 = sys_signal((int)regs->a0, (void (*)(int))regs->a1); break;
            case 11: sys_sigreturn(regs); break;
            case 12: regs->a0 = sys_kill((int)regs->a0, (int)regs->a1); break;
            case 13: regs->a0 = sys_mmap((unsigned long)regs->a0, (unsigned long)regs->a1,
                                (int)regs->a2, (int)regs->a3); break;
            default: regs->a0 = -1; break;
        }

        regs->tp = (unsigned long)get_current();

        // syscall 結束後關中斷（do_trap 返回前），維持一致性
        asm volatile("csrci sstatus, 0x2");
    } 
    else if(regs->scause == SCAUSE_INST_PAGE_FAULT || regs->scause == SCAUSE_LOAD_PAGE_FAULT || regs->scause == SCAUSE_STORE_PAGE_FAULT){
        int fault_state = handle_user_page_fault(regs);
        if(fault_state < 0){
            uart_puts("[Segmentation fault]: Kill Process\n");
            sys_exit(-1);
        }
    }
    else {
        uart_puts("=== S-Mode trap ===\n");
        uart_puts("scause: "); uart_dec(regs->scause); uart_puts("\n");
        uart_puts("sepc: ");   uart_hex(regs->sepc);   uart_puts("\n");
        uart_puts("stval: ");  uart_dec(regs->stval);  uart_puts("\n");
        // if (regs->scause == 8)
        //     regs->sepc += 4;
        while(1);
    }

    do_signal(regs); // 返回 user 前檢查 signal
}


void start_kernel(unsigned long hartid, void *dtb) {
    uart_init(dtb);
    uart_puts("\nStarting kernel ...\n");
    mem_allocator_init(dtb);
    signal_init();
    video_init(dtb);
    shell_init(dtb);
    bootloader_init(hartid, dtb);
    uart_interrupt_init(hartid);

    asm volatile("csrs sie, %0" :: "r"(1UL << 5));  // STIE
    asm volatile("csrs sie, %0" :: "r"(1UL << 9));  // SEIE
    asm volatile("csrsi sstatus, 0x2");              // SIE

    timer_init(dtb);
    idle_init(); // 建立 idle_task (pid = 0)
    thread_create(shell_thread); // pid = 1
    idle(); // 進入主迴圈
}
