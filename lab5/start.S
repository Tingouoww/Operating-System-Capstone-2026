.section ".text.boot"
.global _start
_start:
    la a3, __bss_start
    la a4, __bss_stop
    ble a4, a3, clear_bss_done
clear_bss:
    sd zero, (a3)
    add a3, a3, 8
    blt a3, a4, clear_bss
clear_bss_done:
    la a2, handle_exception
    csrw stvec, a2  # stvec: 存 S-mode trap handler 的入口位址
    la sp, _end
    tail start_kernel

.global handle_exception
.align 2 # 對齊 2 的 2 次方 byte(4 byte)
handle_exception:
    /*
     * Retrieve kernel sp from sscratch and
     * save user sp to sscratch
     */
    # csrrw: Control and Status Register Read/Write
    csrrw sp, sscratch, sp  # 交換 sp 和 sscratch, sscratch : 避免覆寫使用者暫存器, 完成後 => sp = kernel_sp, sscratch = user_ap
    bnez sp, save_context # 如果 trap 是從 S-mode 來，sscratch 是 0，交換後 sp 會變 0, 用 bnez sp 判斷失敗

    csrr sp, sscratch 
save_context:
    /* Save context */
    /* 把 trap 發生當下的 CPU 狀態存到 kernel stack 上，形成一個 trap frame，讓 C 函式 do_trap() 可以安全處理 trap，處理完再恢復回去。*/
    addi sp, sp, -8 * 35
    sd ra,  8 *  0(sp)
    sd gp,  8 *  2(sp)
    sd tp,  8 *  3(sp)
    sd t0,  8 *  4(sp)
    sd t1,  8 *  5(sp)
    sd t2,  8 *  6(sp)
    sd s0,  8 *  7(sp)
    sd s1,  8 *  8(sp)
    sd a0,  8 *  9(sp)
    sd a1,  8 * 10(sp)
    sd a2,  8 * 11(sp)
    sd a3,  8 * 12(sp)
    sd a4,  8 * 13(sp)
    sd a5,  8 * 14(sp)
    sd a6,  8 * 15(sp)
    sd a7,  8 * 16(sp)
    sd s2,  8 * 17(sp)
    sd s3,  8 * 18(sp)
    sd s4,  8 * 19(sp)
    sd s5,  8 * 20(sp)
    sd s6,  8 * 21(sp)
    sd s7,  8 * 22(sp)
    sd s8,  8 * 23(sp)
    sd s9,  8 * 24(sp)
    sd s10, 8 * 25(sp)
    sd s11, 8 * 26(sp)
    sd t3,  8 * 27(sp)
    sd t4,  8 * 28(sp)
    sd t5,  8 * 29(sp)
    sd t6,  8 * 30(sp)
    csrr s0, sscratch
    csrr s1, sepc # trap 發生時的 PC，(CPU 正在處理哪一個位址的指令)
    csrr s2, sstatus  #  trap 發生時相關狀態，包含 SPP/SPIE 等
    csrr s3, scause # trap 原因，例如 U-mode ecall 是 8 (746-747)
    csrr s4, stval # trap 額外資訊，例如 fault address，ecall 通常是 0
    sd s0,  8 *  1(sp)
    sd s1,  8 * 31(sp)
    sd s2,  8 * 32(sp)
    sd s3,  8 * 33(sp)
    sd s4,  8 * 34(sp)

    csrw sscratch, zero
    move a0, sp # 第一個參數放在 a0
    call do_trap #do_trap(sp) , sp 指向剛建立的 trap frame

ret_from_exception:
    ld s0,  8 * 32(sp)
    andi s0, s0, (1 << 8)
    bnez s0, 1f

    addi s0, sp, 8 * 35
    csrw sscratch, s0
1:
    /* Restore context */
    ld s1,  8 * 31(sp)
    ld s2,  8 * 32(sp)
    csrw sepc, s1
    csrw sstatus, s2
    ld ra,  8 *  0(sp)
    ld gp,  8 *  2(sp)
    ld tp,  8 *  3(sp)
    ld t0,  8 *  4(sp)
    ld t1,  8 *  5(sp)
    ld t2,  8 *  6(sp)
    ld s0,  8 *  7(sp)
    ld s1,  8 *  8(sp)
    ld a0,  8 *  9(sp)
    ld a1,  8 * 10(sp)
    ld a2,  8 * 11(sp)
    ld a3,  8 * 12(sp)
    ld a4,  8 * 13(sp)
    ld a5,  8 * 14(sp)
    ld a6,  8 * 15(sp)
    ld a7,  8 * 16(sp)
    ld s2,  8 * 17(sp)
    ld s3,  8 * 18(sp)
    ld s4,  8 * 19(sp)
    ld s5,  8 * 20(sp)
    ld s6,  8 * 21(sp)
    ld s7,  8 * 22(sp)
    ld s8,  8 * 23(sp)
    ld s9,  8 * 24(sp)
    ld s10, 8 * 25(sp)
    ld s11, 8 * 26(sp)
    ld t3,  8 * 27(sp)
    ld t4,  8 * 28(sp)
    ld t5,  8 * 29(sp)
    ld t6,  8 * 30(sp)
    ld sp,  8 *  1(sp)
    sret
