RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy
CFLAGS = -mcmodel=medany -ffreestanding -nostdlib -g -Wall # mcmodel -> pc relative
QEMU ?= qemu-system-riscv64
TARGET = kernel

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(LD) -T linker.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

run: build $(TARGET).bin
	$(QEMU) -M virt -m 8G -kernel $(TARGET).bin -display none -serial stdio

clean:
	rm -f $(TARGET) $(TARGET).elf *.o
