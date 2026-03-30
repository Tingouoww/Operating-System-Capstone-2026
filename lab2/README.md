# Lab 2: Booting

## 1. Objective 
Implement a bootloader that supports UART-based image transfer, and build the program/data/hardware context required for OS boot by parsing a CPIO filesystem and a Device Tree.

## 2. Implemented Scope
- [x] Implement a bootloader that loads kernel images through UART.
- [x] Parsing the flatten devicetree and provide an interface to query the devicetree for device information
- [x] implement a parser to read files in the archive
- [ ] Bootloader Self-Relocation

## 3. Project Folder Structure
```text
lab2/
├── docs/
│   └── Lab2.pdf
├── include/
│   ├── bootloader.h
│   ├── cpio.h
│   ├── fdt.h
│   ├── sbi.h
│   ├── shell.h
│   ├── string.h
│   └── uart.h
├── rootfs/
├── src/
│   ├── bootloader.c
│   ├── cpio.c
│   ├── fdt.c
│   ├── sbi.c
│   ├── shell.c
│   ├── string.c
│   └── uart.c
├── tools/
│   └── send_kernel.py
├── main.c
├── start.S
├── Makefile
├── linker.ld
├── linker_qemu.ld
├── linker_payload.ld
├── linker_qemu_payload.ld
├── kernel.its
├── initramfs.cpio
├── x1_orangepi-rv2.dtb
├── x1_orangepi-rv2.dts
└── README.md
```

## 4. Boot Flow
```text
    [Boot Firmware / SBI]
        │
        │ Set a0 = hartid (usually 0), a1 = dtb
        ▼
    [_start in start.S]
        │
        ├─ Clear .bss section (__bss_start ~ __bss_stop)
        ├─ sp = _end
        └─ tail start_kernel   (pass a0/a1 through unchanged)
                │
                ▼
    [start_kernel(hartid, dtb) in main.c]
        │
        ├─ uart_init(dtb)
        ├─ shell_init(dtb)
        ├─ bootloader_init(hartid, dtb)
        ├─ print_shell_prompt()
        └─ while(1) read UART input + execute commands
```

## 5. UART Bootloader
### 5.1. Goal
Implement a bootloader that receives a kernel image over UART and writes it to a target memory address.
### 5.2. Host-side Loader: `tools/send_kernel.py`
  
The transfer protocol is **fixed header + payload**:
    1. Read the kernel image
        - Read `kernel.bin` in binary mode
        - Get the image size
    2. Pack the header
        - Use `struct.pack("<II", BOOT_MAGIC, image_size)` to build an 8-byte header
        - `BOOT_MAGIC = 0x544F4F42` (little-endian bytes correspond to the string `"BOOT"`).
        - `"<II"` means two 32-bit unsigned integers in little-endian order.
    3. Configure UART to raw mode
        - Disable echo, canonical mode, software flow control, output post-processing, etc., so data is sent as a raw byte stream without terminal-layer modification.
    4. Transmission order
        - first: Header
        - second: kernel payload
        - Use `tcdrain()` to wait for UART transmission to complete before exit.
    5. Cleanup protection
        - Restore the original terminal attributes before exiting to avoid affecting later terminal usage.

### 5.3. Bootloader-side Expected Behavior

The bootloader must follow the same protocol:
- Receive the 8-byte header first.
- Validate `BOOT_MAGIC`.
- Read `image_size` and verify it is legal (current implementation: `size > 0` and `size <= 16MB`).
- Receive exactly `image_size` bytes into the kernel load address.
- Jump to the kernel entry after transfer completes.

### Command Example
- Orange Pi
    ```bash
    sudo python3 tools/send_kernel.py /dev/ttyUSB0 kernel.bin
    ```
- QEMU
    1. 
    ```bash
    make run-pty
    ```
    2. Open another terminal (assume PTY is `/dev/pts/<n>`)
    ```
    sudo screen /dev/pts/<n> 115200
    ```
    3. 
    ```bash
    python3 tools/send_kernel.py /dev/pts/<n> kernel.bin
    ```
---
## 6. Flatten Device Tree (FDT) Parser

### 6.1 Goal
Parse the DTB passed at boot so the system can retrieve hardware information (for example UART and initramfs locations).

### 6.2 Code Walkthrough
1. `start_kernel(hartid, dtb)` in `main.c`
   - At boot, SBI places `hartid` and the `dtb` pointer in `a0/a1`; these become the function parameters in `start_kernel`.

2. Validate FDT header
   - Check `magic == 0xd00dfeed` in `fdt_path_offset()` to avoid parsing an invalid DTB.

3. `fdt_path_offset(fdt, path)` in `src/fdt.c`
   - Find the target node offset by path (for example `/chosen`, `/soc/serial...`).

4. `fdt_getprop(fdt, nodeoffset, name, len)` in `src/fdt.c`
   - Read a property value and length from the specified node.

5. Actual usage
   - Other modules use these query interfaces to fetch hardware parameters and complete initialization.
---
## 7. Part C - Archive Parser (CPIO newc)

### Goal
Parse initramfs (`cpio newc`) and provide `ls` and `cat` functionality.

### Key Functions
- `initrd_init(start, end)`  
  Record the initramfs memory range.
- `hextoi(s, n)`  
  Convert ASCII-hex fields in the header to integers (for example `c_filesize`, `c_namesize`).
- `align(n, 4)`  
  Handle 4-byte alignment in `newc`.
- `initrd_list(rd)`  
  Scan the archive entry-by-entry and print file names (stop at `TRAILER!!!`).
- `initrd_cat(rd, filename)`  
  Match file names entry-by-entry; print file content if found, otherwise report `No such file`.

### Shell Integration
- `ls` -> `initrd_list(NULL)`
- `cat <file>` -> `initrd_cat(NULL, filename)`

---

## 8. Makefile Usage

### 8.1 Targets
- `make build`  
  Build and generate `kernel.elf` and `kernel.bin` (use `linker.ld`, or override with `LINKER_SCRIPT`).
- `make build-payload`  
  Generate `kernel_payload.elf` and `kernel_payload.bin` (use `linker_qemu_payload.ld`).
- `make run`  
  Run the kernel on QEMU with serial on `stdio`.
- `make run-pty`  
  Run the kernel on QEMU with serial on `pty` (useful with `send_kernel.py`).
- `make qemu-dtb`  
  Export `qemu.dtb`.
- `make qemu-dts`  
  Convert `qemu.dtb` into readable `qemu.dts`.
- `make clean`  
  Clean build artifacts.

### 8.2 Why Multiple Linker Scripts
This project has two dimensions:
1. Runtime environment: Board / QEMU
2. Image role: main kernel / dynamically loaded payload

So different link addresses are required to avoid overlap:
- `linker.ld`: Board main kernel (`0x00200000`)
- `linker_qemu.ld`: QEMU main kernel (`0x80200000`)
- `linker_payload.ld`: Board payload (`0x20000000`)
- `linker_qemu_payload.ld`: QEMU payload (`0x82000000`)

### 8.3 Payload Purpose
`payload` is a second-stage image received by the `load` command over UART, written to a target address, then executed by jumping to it.  
Its purpose is to validate the full bootloader handoff path: **receive -> load -> transfer control**.

### 8.4 Command Examples
```bash
make build
make run
make run-pty
make build-payload
make qemu-dts
make clean
```
