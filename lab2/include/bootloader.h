#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stddef.h>
#include <stdint.h>

#ifndef KERNEL_LOAD_ADDR
#define KERNEL_LOAD_ADDR 0x20000000UL
// #define KERNEL_LOAD_ADDR 0x82000000UL
#endif

void bootloader_init(unsigned long hartid, void *dtb);
void bootloader_load(void);

#endif
