RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy
ARCHFLAGS = -march=rv64gc_zicsr_zifencei -mabi=lp64
CFLAGS = $(ARCHFLAGS) -mcmodel=medany -ffreestanding -nostdlib -g -Wall -Iinclude
MKIMAGE ?= mkimage
LINKER_SCRIPT ?= linker.ld
QEMU_LINKER_SCRIPT ?= linker_qemu.ld
ITS ?= kernel.its
BOARD_DTB ?= x1_orangepi-rv2.dtb
INITRAMFS ?= initramfs.cpio
LDFLAGS = -T $(LINKER_SCRIPT)
QEMU ?= qemu-system-riscv64
TARGET = kernel
FIT = kernel.fit
SRCS = start.S main.c src/bootloader.c src/cpio.c src/fdt.c src/sbi.c src/uart.c src/shell.c src/utils.c src/mem_allocator.c src/mem_allocator_test.c src/timer.c src/task.c src/syscall.c src/video.c src/signal.c src/vm.c
OBJS = $(notdir $(SRCS:.c=.o))
OBJS := $(notdir $(OBJS:.S=.o))

build:
	$(CC) $(CFLAGS) -c $(SRCS)
	$(LD) $(LDFLAGS) -o $(TARGET).elf $(OBJS)
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

fit: build $(ITS) $(BOARD_DTB) $(INITRAMFS)
	$(MKIMAGE) -f $(ITS) $(FIT)

board: fit

run:
# -DQEMU 是編譯器參數(在預處理階段定義一個巨集 QEMU)
	$(MAKE) build LINKER_SCRIPT=$(QEMU_LINKER_SCRIPT) CFLAGS="$(CFLAGS) -DQEMU"
	$(QEMU) -M virt -m 8G -kernel $(TARGET).bin -initrd initramfs.cpio -device ramfb -serial stdio

run-pty:
	$(MAKE) build LINKER_SCRIPT=$(QEMU_LINKER_SCRIPT) CFLAGS="$(CFLAGS) -DQEMU"
	$(QEMU) -M virt -m 8G -kernel $(TARGET).bin -initrd initramfs.cpio -device ramfb -serial pty

qemu-dtb:
	$(QEMU) -machine virt,dumpdtb=qemu.dtb -display none -serial none

qemu-dts: qemu-dtb
	dtc -I dtb -O dts qemu.dtb -o qemu.dts

clean:
	rm -f $(TARGET) $(TARGET).elf $(TARGET).bin $(FIT) $(OBJS) qemu.dtb qemu.dts
