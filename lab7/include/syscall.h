#ifndef SYSCALL_H
#define SYSCALL_H
#include "task.h"
#include "signal.h"

// ---- POSIX ----
long sys_signal(int signum, void (*handler)()); // 10
void sys_sigreturn(struct pt_regs *regs); // 11
int sys_kill(int pid, int signum); // 12
void do_signal(struct pt_regs *regs);
void signal_init(void);

long  sys_getpid(void); // 0
long  sys_uart_read(char *buf, long count); // 1
long  sys_uart_write(const char *buf, long count); // 2
int   sys_exec(struct pt_regs *regs, const char *path); // 3
long  sys_fork(struct pt_regs *regs); // 4
long  sys_waitpid(long pid); // 5
void  sys_exit(int status); // 6
int   sys_stop(long pid); // 7
void  sys_display(const unsigned int *bmp_image,
                  unsigned int width,
                  unsigned int height); // 8
int   sys_usleep(unsigned int usec); // 9

#endif
