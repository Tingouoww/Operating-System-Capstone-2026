#ifndef MMAP_H
#define MMAP_H

#include "task.h"
#include "vm.h"
#include "utils.h"
#include "mem_allocator.h"

#define USER_MMAP_BASE  0x0000000010000000UL
#define USER_MMAP_LIMIT USER_SIGNAL_STACK_VA

// mmap protection
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
// mmap flags
#define MAP_ANONYMOUS 0x20
#define MAP_POPULATE 0x8000

long  sys_mmap(unsigned long addr, unsigned long length, int prot, int flags);


#endif
