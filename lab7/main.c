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
#include "vfs.h"
#include "tmpfs.h"
#include "ramfs.h"

static void vfs_fail(const char* msg);

static void init_initrd(const void *dtb) {
    unsigned long initrd_start = 0;
    unsigned long initrd_end = 0;

    if (fdt_get_initrd_range(dtb, &initrd_start, &initrd_end) < 0)
        vfs_fail("[initrd] failed to get initrd range\n");
    if (initrd_start == 0 || initrd_end <= initrd_start)
        vfs_fail("[initrd] initrd range not found\n");

    initrd_init((void *)PA_TO_VA(initrd_start), (void *)PA_TO_VA(initrd_end));
}

static void init_rootfs(void) {
    struct filesystem* tmpfs = tmpfs_get_filesystem();
    struct filesystem* ramfs = ramfs_get_filesystem();

    if (register_filesystem(tmpfs) != 0) {
        uart_puts("[vfs] failed to register tmpfs\n");
        while (1);
    }
    if (register_filesystem(ramfs) != 0) {
        uart_puts("[vfs] failed to register ramfs\n");
        while (1);
    }

    if (vfs_mount("/", "tmpfs") != 0) {
        uart_puts("[vfs] failed to mount rootfs\n");
        while (1);
    }
    if (vfs_mkdir("/ramfs") != 0) {
        uart_puts("[vfs] failed to create /ramfs\n");
        while (1);
    }
    if (vfs_mount("/ramfs", "ramfs") != 0) {
        uart_puts("[vfs] failed to mount /ramfs\n");
        while (1);
    }
}

static void vfs_fail(const char* msg) {
    uart_puts(msg);
    while (1);
}

static void vfs_basic1_smoke_test(void) {
    struct file* file = NULL;
    char buf[16] = {0};

    if (vfs_open("/hello.txt", O_CREAT, &file) != 0)
        vfs_fail("[vfs] open(create) failed\n");
    if (vfs_write(file, "hello", 5) != 5)
        vfs_fail("[vfs] write failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close after write failed\n");

    if (vfs_open("/hello.txt", 0, &file) != 0)
        vfs_fail("[vfs] open(read) failed\n");
    if (vfs_read(file, buf, 5) != 5)
        vfs_fail("[vfs] read failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close after read failed\n");

    if (str_cmp(buf, "hello") != 0)
        vfs_fail("[vfs] verify failed\n");

    uart_puts("[vfs] basic exercise 1 smoke test passed: ");
    uart_puts(buf);
    uart_puts("\n");
}

static void vfs_basic2_smoke_test(void) {
    struct file* file = NULL;
    struct vnode* vnode = NULL;
    char buf[16] = {0};

    if (vfs_mkdir("/dir") != 0)
        vfs_fail("[vfs] mkdir /dir failed\n");
    if (vfs_mkdir("/dir/sub") != 0)
        vfs_fail("[vfs] mkdir /dir/sub failed\n");
    if (vfs_mkdir("/dir") == 0)
        vfs_fail("[vfs] duplicate mkdir should fail\n");

    if (vfs_open("/dir/sub/note.txt", O_CREAT, &file) != 0)
        vfs_fail("[vfs] create nested file failed\n");
    if (vfs_write(file, "basic2", 6) != 6)
        vfs_fail("[vfs] write nested file failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close nested file failed\n");

    if (vfs_lookup("/dir/sub/note.txt", &vnode) != 0 || vnode == NULL)
        vfs_fail("[vfs] multi-level lookup failed\n");
    if (vfs_open("/dir/sub/note.txt", 0, &file) != 0)
        vfs_fail("[vfs] reopen nested file failed\n");
    if (vfs_read(file, buf, 6) != 6)
        vfs_fail("[vfs] read nested file failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close nested read failed\n");
    if (str_cmp(buf, "basic2") != 0)
        vfs_fail("[vfs] nested content verify failed\n");

    if (vfs_mkdir("/mnt") != 0)
        vfs_fail("[vfs] mkdir /mnt failed\n");
    if (vfs_open("/mnt/hidden.txt", O_CREAT, &file) != 0)
        vfs_fail("[vfs] create pre-mount file failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close pre-mount file failed\n");

    if (vfs_mount("/mnt", "tmpfs") != 0)
        vfs_fail("[vfs] mount /mnt failed\n");
    if (vfs_lookup("/mnt", &vnode) != 0 || vnode == NULL)
        vfs_fail("[vfs] lookup mounted root failed\n");
    if (vfs_mount("/mnt", "tmpfs") == 0)
        vfs_fail("[vfs] duplicate mount should fail\n");
    if (vfs_open("/mnt/hidden.txt", 0, &file) == 0)
        vfs_fail("[vfs] covered file should not be visible after mount\n");

    if (vfs_open("/mnt/visible.txt", O_CREAT, &file) != 0)
        vfs_fail("[vfs] create mounted file failed\n");
    if (vfs_write(file, "mount", 5) != 5)
        vfs_fail("[vfs] write mounted file failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close mounted file failed\n");

    memset(buf, 0, sizeof(buf));
    if (vfs_open("/mnt/visible.txt", 0, &file) != 0)
        vfs_fail("[vfs] reopen mounted file failed\n");
    if (vfs_read(file, buf, 5) != 5)
        vfs_fail("[vfs] read mounted file failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close mounted read failed\n");
    if (str_cmp(buf, "mount") != 0)
        vfs_fail("[vfs] mounted content verify failed\n");

    if (vfs_mount("/hello.txt", "tmpfs") == 0)
        vfs_fail("[vfs] mount on file should fail\n");
    if (vfs_mount("/nope", "tmpfs") == 0)
        vfs_fail("[vfs] mount on missing path should fail\n");

    uart_puts("[vfs] basic exercise 2 smoke test passed\n");
}

static void vfs_basic4_smoke_test(void) {
    struct file* file = NULL;
    struct vnode* vnode = NULL;
    char buf[8] = {0};

    if (vfs_lookup("/ramfs", &vnode) != 0 || vnode == NULL || !vfs_is_dir(vnode))
        vfs_fail("[vfs] lookup /ramfs failed\n");
    if (vfs_open("/ramfs/osctest.bin", 0, &file) != 0)
        vfs_fail("[vfs] open /ramfs/osctest.bin failed\n");
    if (vfs_read(file, buf, sizeof(buf)) <= 0)
        vfs_fail("[vfs] read /ramfs/osctest.bin failed\n");
    if (vfs_close(file) != 0)
        vfs_fail("[vfs] close /ramfs/osctest.bin failed\n");

    if (vfs_open("/ramfs/nope", 0, &file) == 0)
        vfs_fail("[vfs] missing ramfs file should fail\n");
    if (vfs_mkdir("/ramfs/x") == 0)
        vfs_fail("[vfs] mkdir on ramfs should fail\n");
    if (vfs_mount("/ramfs/osctest.bin", "tmpfs") == 0)
        vfs_fail("[vfs] mount on ramfs file should fail\n");
    if (vfs_open("/ramfs/created.txt", O_CREAT, &file) == 0)
        vfs_fail("[vfs] create on ramfs should fail\n");

    uart_puts("[vfs] basic exercise 4 smoke test passed\n");
}

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
            case 14: regs->a0 = sys_open((const char *)regs->a0, (int)regs->a1); break;
            case 15: regs->a0 = sys_close((int)regs->a0); break;
            case 16: regs->a0 = sys_read((int)regs->a0, (void *)regs->a1,
                                         (unsigned long)regs->a2); break;
            case 17: regs->a0 = sys_write((int)regs->a0, (const void *)regs->a1,
                                          (unsigned long)regs->a2); break;
            case 18: regs->a0 = sys_mkdir((const char *)regs->a0,
                                          (unsigned int)regs->a1); break;
            case 19: regs->a0 = sys_mount((const char *)regs->a0,
                                          (const char *)regs->a1,
                                          (const char *)regs->a2,
                                          (unsigned long)regs->a3,
                                          (const void *)regs->a4); break;
            case 20: regs->a0 = sys_chdir((const char *)regs->a0); break;
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
    init_initrd(dtb);
    init_rootfs();
    vfs_basic1_smoke_test();
    vfs_basic2_smoke_test();
    vfs_basic4_smoke_test();
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
