#include "uart.h"
#include "string.h"
#include "mem_allocator.h"
#include "shell.h"
#include "cpio.h"
#include "fdt.h"
#include "bootloader.h"
#include "sbi.h"
#include "timer.h"
#include "task.h"

#define INITRD_BASE 0xa0200000
#define STACK_SIZE  0x1000

static unsigned long initrd_base = INITRD_BASE;

static int local_hextoi(const char *s, int n) {
    int r = 0;
    while (n-- > 0) {
        r <<= 4;
        if (*s >= '0' && *s <= '9')      r += *s++ - '0';
        else if (*s >= 'A' && *s <= 'F') r += *s++ - 'A' + 10;
        else if (*s >= 'a' && *s <= 'f') r += *s++ - 'a' + 10;
        else s++;
    }
    return r;
}

/* 把 n 向上對齊到 byte 的倍數 */
static int local_align(int n, int byte) {
    return (n + byte - 1) & ~(byte - 1);
}

static int local_memcmp(const void *a, const void *b, int n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0; // compare 一樣會回傳 0
}

int exec(const char *filename) {
    char *p = (char *)initrd_base;
    while (local_memcmp(p + sizeof(struct cpio_newc_header), "TRAILER!!!", 10)) {
        // [cpio header][filename][padding][file data][padding]
        struct cpio_newc_header *hdr = (struct cpio_newc_header *)p;
        int namesize = local_hextoi(hdr->c_namesize, 8);
        int filesize = local_hextoi(hdr->c_filesize, 8);
        int headsize = local_align(sizeof(struct cpio_newc_header) + namesize, 4);
        int datasize = local_align(filesize, 4);
        // memory compare 比較 兩塊記憶體的 n bytes 是否相同
        if (!local_memcmp(p + sizeof(struct cpio_newc_header), filename, namesize)) {
            unsigned long kernel_sp; // 存目前 kernel 正在用的 stack pointer。之後 trap 從 U-mode 回來時，要恢復使用 kernel stack。
            // 配置一頁 user stack，保存目前 kernel stack 到 sscratch
            unsigned long user_sp    = (unsigned long)buddy_alloc(0) + STACK_SIZE; // stack 通常往低位址長，所以 stack pointer 要設在頁的最上面
            unsigned long user_entry = (unsigned long)(p + headsize);
            unsigned long sstatus; // 記錄 S-mode trap , interrupt, privilege return 有關狀態 (739)

            asm volatile("mv %0, sp"        : "=r"(kernel_sp));
            asm volatile("csrr %0, sstatus" : "=r"(sstatus));
            
            sstatus &= ~(1UL << 8);  // SPP=0: sret returns to U-mode
            sstatus |=  (1UL << 5);  // SPIE=1: enable interrupts in U-mode (決定進入 U-mode 後是否開啟中斷)

            // 進入 user mode 前先放 sscratch = kernel_sp, sp = user_sp
            // 避免 kernel handler 直接用 user_sp 存 reg
            asm volatile(
                "csrw sscratch, %0\n"
                "mv sp, %1\n"
                "csrw sepc, %2\n" // sret 會跳到 sepc
                "csrw sstatus, %3\n" // 修改過後的 sstatus 放回 sstatus
                "sret\n" // 從 S-mode return 到指定 sepc 位址
                :
                : "r"(kernel_sp), "r"(user_sp), "r"(user_entry), "r"(sstatus)
                : "memory");

            __builtin_unreachable(); // 這裡不該被執行
        }
        p += headsize + datasize; // 如果目前 entry 不是目標檔案就跳到下一個 CPIO entry
    }
    return -1;
}

struct pt_regs {
    unsigned long ra, sp, gp, tp;
    unsigned long t0, t1, t2;
    unsigned long s0, s1;
    unsigned long a0, a1, a2, a3, a4, a5, a6, a7;
    unsigned long s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    unsigned long t3, t4, t5, t6;
    unsigned long sepc, sstatus, scause, stval;
};

// 746-747
#define SCAUSE_IRQ_FLAG         (1UL << 63)
#define SCAUSE_SUPERVISOR_TIMER 5
#define SCAUSE_SUPERVISOR_EXT   9

void do_trap(struct pt_regs *regs) {
    int should_run_tasks = 0;

    // scause -> 判斷 trap 類型是 interrupt 還是 exception
    // 第 63 bit (746 - 747)
    if (regs->scause & SCAUSE_IRQ_FLAG) {
        // async
        unsigned long irq = regs->scause & ~SCAUSE_IRQ_FLAG; // 把 scause 最高 bit 清掉才知道是哪種 interrupt
        if (irq == SCAUSE_SUPERVISOR_TIMER) {
            timer_handle_irq();
            should_run_tasks = 1;
        } else if (irq == SCAUSE_SUPERVISOR_EXT) {
            uart_handle_external_irq();
        }
    } else {
        // sync
        uart_puts("=== S-Mode trap ===\n");
        uart_puts("scause: "); uart_dec(regs->scause); uart_puts("\n"); // 8
        uart_puts("sepc: ");   uart_hex(regs->sepc);   uart_puts("\n"); // trap 發生時的 PC
        uart_puts("stval: ");  uart_dec(regs->stval);  uart_puts("\n"); // trap value
        if (regs->scause == 8)
            regs->sepc += 4; // ecall 是 4-byte instruction
    }

    if (should_run_tasks)
        run_tasks();
}

static void test_task_cb(void *arg) {
    uart_puts("[Task] Executing Priority ");
    uart_puts((const char *)arg);
    uart_puts("\n");
}

void start_kernel(unsigned long hartid, void *dtb) {
    char buf[128];
    int  len = 0;
    char c;

    uart_init(dtb);
    uart_puts("\nStarting kernel ...\n");
    uart_puts("[boot] hartid="); uart_dec(hartid); uart_puts("\n");
    mem_allocator_init(dtb);
    shell_init(dtb);
    bootloader_init(hartid, dtb);
    initrd_base = fdt_get_initrd_start_or_default(dtb, INITRD_BASE);
    uart_interrupt_init(hartid);

    // 743-744
    asm volatile("csrs sie, %0" :: "r"(1UL << 5));  // STIE
    asm volatile("csrs sie, %0" :: "r"(1UL << 9));  // SEIE
    asm volatile("csrsi sstatus, 0x2");              // SIE 打開 S-mode 的整體 interrupt 總開關

    //uart_debug_dump_state();
    
    timer_init(dtb);

    add_task(test_task_cb, "1", 1);
    add_task(test_task_cb, "3", 3);
    add_task(test_task_cb, "2", 2);
    run_tasks();

    print_shell_prompt();
    uart_puts("boot time: 0\n");
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
