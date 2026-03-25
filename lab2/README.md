# Lab 1: Hello World

## Objective

Build a tiny bare-metal RISC-V kernel that can boot, set itself up, talk through UART, and provide a simple shell.

## File Overview

- [`start.S`](/home/ting/Operating-System-Capstone-2026/lab1/start.S): early boot code and low-level startup
- [`linker.ld`](/home/ting/Operating-System-Capstone-2026/lab1/linker.ld): memory layout and section placement
- [`main.c`](/home/ting/Operating-System-Capstone-2026/lab1/main.c): main kernel loop and UART input handling
- [`uart.c`](/home/ting/Operating-System-Capstone-2026/lab1/uart.c): UART driver
- [`sbi.c`](/home/ting/Operating-System-Capstone-2026/lab1/sbi.c): SBI wrapper functions
- [`shell.c`](/home/ting/Operating-System-Capstone-2026/lab1/shell.c): shell prompt, command parsing, and built-in commands
- [`kernel.its`](/home/ting/Operating-System-Capstone-2026/lab1/kernel.its): FIT image description for Orange Pi RV2

## Implementation

### Boot Setup and System Initialization
#### [`linker.ld`](/home/ting/Operating-System-Capstone-2026/lab1/linker.ld)

- Place the kernel load address at `0x00200000`
- Define the expected memory layout:
  - `.text`: executable instructions
  - `.rodata`: read-only data
  - `.data`: initialized global and static variables
  - `.bss`: uninitialized global and static variables
  - `sbss`: a small BSS area for tiny uninitialized variables
- Define `__bss_start` and `__bss_stop` so the kernel can clear the `.bss` section during boot
- Reserve stack space with `. = . + 0x4000;` so the stack does not overwrite `.bss`

#### [`start.S`](/home/ting/Operating-System-Capstone-2026/lab1/start.S)

- Manually clear the `.bss` section so uninitialized data starts at zero
- Set the stack pointer from `_end`, which is the top of the reserved memory area
- Hand control to `start_kernel` after early initialization finishes

### UART Setup and I/O

- Access UART through memory-mapped I/O

#### [`uart.c`](/home/ting/Operating-System-Capstone-2026/lab1/uart.c)

- Implement the low-level I/O functions: `uart_getc()`, `uart_putc()`, `uart_puts()`, and `uart_hex()`
- Use UART base address `0xD4017000`
- `uart_getc()`: poll the Line Status Register and wait for the `DR` bit to be set before reading data
- `uart_putc()`: poll the Line Status Register and wait for the `TDRQ` bit before sending data

### SBI Calls

#### [`sbi.c`](/home/ting/Operating-System-Capstone-2026/lab1/sbi.c)

- Query OpenSBI information through the SBI Base extension
- Support reading:
  - SBI specification version
  - implementation ID
  - implementation version
- Inline assembly note:

```c
asm volatile("ecall"
               : "+r"(a0), "+r"(a1)
               : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
               : "memory");
```

1. `+r` means the register is both read and written. Here, `a0` stores the return error code and `a1` stores the return value.
2. `volatile` tells the compiler not to optimize away this `ecall`.
3. `memory` prevents the compiler from reordering memory accesses across this assembly block.

### Simple Shell and Commands

#### [`shell.c`](/home/ting/Operating-System-Capstone-2026/lab1/shell.c)

- Parse user commands and dispatch them to the right handler
- Use `opi-rv2>` as the shell prompt
- Support these commands:
  - `help`: show the list of available commands
  - `hello`: print `Hello world.`
  - `info`: print OpenSBI-related system information
- Print an error message for unknown commands

### Kernel Main Loop

#### [`main.c`](/home/ting/Operating-System-Capstone-2026/lab1/main.c)

- Run the main kernel loop
- Read input from UART
- Support Enter and Backspace for basic command-line editing

## Build and Run the Kernel

### [`Makefile`](/home/ting/Operating-System-Capstone-2026/lab1/Makefile)

- Provide `build`, `run`, and `clean` targets
- `make build`: compile the assembly and C sources, link them into `kernel.elf`, and convert the result into `kernel.bin`
- `make run`: build the kernel and boot it in QEMU
- `make clean`: remove generated files

### Build `kernel.fit`

```sh
mkimage -f kernel.its kernel.fit
```

## Orange Pi RV2 Boot Image

- [`kernel.its`](/home/ting/Operating-System-Capstone-2026/lab1/kernel.its): describes a U-Boot FIT image for Orange Pi RV2
- Package these files into the boot image:
  - `kernel.bin`
  - `x1_orangepi-rv2.dtb`
- Set both the kernel load address and entry address to `0x00200000`
- Generate `kernel.fit` for booting on the board
- `fdt` stands for Flattened Device Tree. In this FIT image, it refers to the bundled `x1_orangepi-rv2.dtb`, which the bootloader passes to the kernel so the kernel knows the board's hardware layout, such as UART and memory

## Result

This lab ends with a small but working bare-metal kernel that can boot, communicate over UART, query OpenSBI information, and run a few basic shell commands.
