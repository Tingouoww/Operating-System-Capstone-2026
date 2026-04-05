RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy
ARCHFLAGS = -march=rv64gc_zicsr_zifencei -mabi=lp64
CFLAGS = $(ARCHFLAGS) -mcmodel=medany -ffreestanding -nostdlib -g -Wall -Iinclude
MKIMAGE ?= mkimage
LINKER_SCRIPT ?= linker.ld
PAYLOAD_LINKER_SCRIPT ?= linker_payload.ld
QEMU_LINKER_SCRIPT ?= linker_qemu.ld
QEMU_PAYLOAD_LINKER_SCRIPT ?= linker_qemu_payload.ld
LDFLAGS = -T $(LINKER_SCRIPT)
QEMU ?= qemu-system-riscv64
TARGET = kernel
PAYLOAD = kernel_payload
FIT = kernel.fit
SRCS = start.S main.c src/bootloader.c src/cpio.c src/fdt.c src/sbi.c src/uart.c src/shell.c src/string.c
OBJS = $(notdir $(SRCS:.c=.o))
OBJS := $(notdir $(OBJS:.S=.o))

build:
	$(CC) $(CFLAGS) -c $(SRCS)
	$(LD) $(LDFLAGS) -o $(TARGET).elf $(OBJS)
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

build-payload:
	$(CC) $(CFLAGS) -c $(SRCS)
	$(LD) -T $(PAYLOAD_LINKER_SCRIPT) -o $(PAYLOAD).elf $(OBJS)
	$(OBJCOPY) -O binary $(PAYLOAD).elf $(PAYLOAD).bin

fit: build
	$(MKIMAGE) -f kernel.its $(FIT)

board: fit

run:
# -DQEMU 是編譯器參數(在預處理階段定義一個巨集 QEMU)
	$(MAKE) build LINKER_SCRIPT=$(QEMU_LINKER_SCRIPT) CFLAGS="$(CFLAGS) -DQEMU"
	$(QEMU) -M virt -m 8G -kernel $(TARGET).bin -initrd initramfs.cpio -display none -serial stdio

run-pty:
	$(MAKE) build LINKER_SCRIPT=$(QEMU_LINKER_SCRIPT) CFLAGS="$(CFLAGS) -DQEMU"
	$(QEMU) -M virt -m 8G -kernel $(TARGET).bin -initrd initramfs.cpio -display none -serial pty

qemu-dtb:
	$(QEMU) -machine virt,dumpdtb=qemu.dtb -display none -serial none

qemu-dts: qemu-dtb
	dtc -I dtb -O dts qemu.dtb -o qemu.dts

clean:
	rm -f $(TARGET) $(TARGET).elf $(TARGET).bin $(PAYLOAD).elf $(PAYLOAD).bin $(FIT) $(OBJS) qemu.dtb qemu.dts
