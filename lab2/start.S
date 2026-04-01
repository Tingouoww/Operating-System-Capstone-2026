.section ".text.boot"
.global _start

#ifdef QEMU
    .equ RELOC_BASE, 0x80A00000
#else
    .equ RELOC_BASE, 0x20000000
#endif

_start:
    # source start location
    la t0, _start 

    # relocate location
    li t1, RELOC_BASE

    # 已經在 RELOC_BASE 就跳過 copy
    beq t0, t1, relocated_entry

    # source end location
    la t2, __image_end

copy_locate_loop:
    bgeu t0, t2, copy_done
    lbu t3, 0(t0)
    sb t3, 0(t1)
    addi t0, t0, 1
    addi t1, t1, 1
    j copy_locate_loop

copy_done:
    fence.i

    la t0, relocated_entry
    la t1, _start

    # offset
    sub t0, t0, t1
    li t1, RELOC_BASE
    add t0, t0, t1
    jr t0

relocated_entry:
    la t0, __bss_start
    la t1, __bss_stop
    bgeu t0, t1, clear_bss_done

clear_bss:
    sd zero, (t0) # store doubleword
    addi t0, t0, 8
    bltu t0, t1, clear_bss

clear_bss_done:
    la sp, _end
    tail start_kernel
