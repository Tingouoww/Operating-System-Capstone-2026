# Lab 2: Booting

## 1. Objective
This lab builds a RISC-V bootloader/kernel runtime that initializes UART from DTB, provides an interactive shell, parses Flatten Device Tree (FDT) and CPIO `newc` initramfs data, receives a kernel image over UART and transfers control to it, and performs bootloader self-relocation before entering the C runtime.

## 2. Implemented Scope
- [x] Implement a bootloader that loads kernel images through UART.
- [x] Parse the Flatten Device Tree and provide interfaces to query device information.
- [x] Implement a parser to read files in the CPIO archive.
- [x] Bootloader self-relocation.

## 3. Project Layout
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
└── README.md
```

## 4. Boot Flow
```text
[Boot Firmware / OpenSBI]
    │
    │ a0 = hartid, a1 = dtb
    ▼
[_start in start.S]
    │
    ├─ copy image [_start, __image_end) to RELOC_BASE
    │    - board: 0x20000000
    │    - qemu : 0x80A00000
    ├─ jump to relocated_entry at relocated address
    ├─ clear .bss
    ├─ set sp = _end
    └─ tail start_kernel(hartid, dtb)
            │
            ▼
[start_kernel in main.c]
    │
    ├─ uart_init(dtb)
    ├─ shell_init(dtb)
    ├─ bootloader_init(hartid, dtb)
    │    - relocation safety check
    └─ command loop (UART)
```

## 5. Shell Commands
- `help`: print command list
- `hello`: print hello message
- `info`: print SBI implementation/spec info
- `ls`: list file names from initramfs (`cpio newc`)
- `cat <file>`: print file content from initramfs
- `load`: receive image via UART and jump to `KERNEL_LOAD_ADDR`

## 6. UART Load Protocol
### 6.1 Format
Host sends:
1. 4 bytes magic (`0x544F4F42`, little-endian, string "BOOT")
2. 4 bytes payload size (little-endian)
3. raw payload bytes

`tools/send_kernel.py` already implements this format.

### 6.2 Bootloader Behavior (`bootloader_load`)
- wait for header
- verify magic
- verify `size` (`0 < size <= 16 MiB`)
- verify target range safety
- receive payload into `KERNEL_LOAD_ADDR`
- execute `fence.i`
- jump to loaded entry with `(hartid, dtb)`

### 6.3 Core Functions
- `bootloader_init(hartid, dtb)`: Stores boot context and runs relocation safety checks before `load` is allowed.
- `bootloader_load()`: Receives image data from UART, validates protocol fields, and jumps to the loaded entry.
- `kernel_load_range_is_safe(size)`: Checks that the target load range does not overlap relocated bootloader, DTB, or initramfs.
- `bootloader_reloc_range_is_safe()`: Verifies the relocation destination is valid against memory/DTB/initramfs ranges from DTB.

## 7. Self-Relocation and Safety Checks
### 7.1 Self-Relocation (`start.S`)
- relocation runs before clearing `.bss` and before entering C runtime
- copy range is `[_start, __image_end)`
- execution continues at relocated `relocated_entry`

### 7.2 Relocation Safety (`bootloader_init`)
Bootloader checks relocation range against DTB-derived ranges:
- `/memory` range (must be inside)
- DTB occupied range (must not overlap)
- initramfs range from `/chosen` (must not overlap)

### 7.3 Kernel Load Safety (`bootloader_load`)
Before receiving payload, bootloader checks target load range against:
- relocated bootloader range
- DTB range
- initramfs range

If any overlap/invalid range is detected, loading is rejected.

## 8. FDT Parser
Implemented interfaces:
- `fdt_path_offset(fdt, path)`
- `fdt_getprop(fdt, nodeoffset, name, lenp)`
- `fdt_get_memory_range(fdt, &base, &size)`
- `fdt_get_initrd_range(fdt, &start, &end)`

Used by:
- UART initialization
- initrd discovery from `/chosen`
- relocation/load safety checks

Core functions:
- `fdt_path_offset(fdt, path)`: Finds a node offset by absolute DT path (for example `/chosen` or `/memory`).
- `fdt_getprop(fdt, nodeoffset, name, lenp)`: Returns a property pointer and length from a specific node.
- `fdt_get_memory_range(fdt, &base, &size)`: Parses the `/memory` `reg` property into usable base/size values.
- `fdt_get_initrd_range(fdt, &start, &end)`: Reads `linux,initrd-start/end` from `/chosen`.

## 9. CPIO Parser
Implemented features (`newc` format):
- parse archive headers and alignment
- iterate entries until `TRAILER!!!`
- list file names (`ls`)
- print file content (`cat <file>`)

Core functions:
- `initrd_init(start, end)`: Records the in-memory initramfs range for later archive traversal.
- `initrd_list(rd)`: Iterates through archive entries and prints file names.
- `initrd_cat(rd, filename)`: Searches for a target file in the archive and prints its content.
- `hextoi(s, n)`: Converts ASCII hex fields in CPIO headers into integer values.

## 10. Build and Run
### 10.1 Common targets
- `make build`: build `kernel.elf` / `kernel.bin` (default `linker.ld`)
- `make build-payload`: build `kernel_payload.elf` / `kernel_payload.bin`
- `make run`: QEMU run with `-serial stdio`
- `make run-pty`: QEMU run with `-serial pty`
- `make fit`: build FIT image (`kernel.fit`)
- `make board`: alias of `make fit`
- `make qemu-dtb`: dump `qemu.dtb`
- `make qemu-dts`: generate readable `qemu.dts`
- `make clean`: remove build artifacts

### 10.2 QEMU workflow with UART sender
1. Start target in PTY mode:
```bash
make run-pty
```
2. Note the generated PTY path from QEMU output (example: `/dev/pts/5`).
3. Send image from another terminal:
```bash
python3 tools/send_kernel.py /dev/pts/5 kernel_payload.bin
```

### 10.3 Board example
```bash
sudo python3 tools/send_kernel.py /dev/ttyUSB0 kernel_payload.bin
```

## 11. Address Notes
- `KERNEL_LOAD_ADDR` is selected in `include/bootloader.h`:
  - QEMU build (`-DQEMU`): `0x80200000`
  - board build: `0x00200000`
- relocation base is selected in `start.S` / `src/bootloader.c`:
  - QEMU build (`-DQEMU`): `0x80A00000`
  - board build: `0x20000000`

These values are validated at runtime by safety checks before accepting `load`.
