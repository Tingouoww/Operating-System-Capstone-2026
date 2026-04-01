#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stddef.h>
#include <stdint.h>

#ifndef KERNEL_LOAD_ADDR
#ifdef QEMU
#define KERNEL_LOAD_ADDR 0x80200000UL
#else
#define KERNEL_LOAD_ADDR 0x00200000UL
#endif
#endif

void bootloader_init(unsigned long hartid, void *dtb);
void bootloader_load(void);

#endif
