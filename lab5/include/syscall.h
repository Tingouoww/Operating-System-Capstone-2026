#ifndef SYSCALL_H
#define SYSCALL_H
#include "task.h"

long   sys_getpid(void); // 0
long  sys_uart_read(char *buf, long count); // 1
long  sys_uart_write(const char *buf, long count); // 2
int   sys_exec(struct pt_regs *regs, const char *path); // 3
long  sys_fork(struct pt_regs *regs); // 4
long  sys_waitpid(long pid); // 5
void  sys_exit(int status); // 6
int   sys_stop(long pid); // 7
#endif