// Hand-written switch dispatches in the form clang emits for
// AArch64 (compressed 1- and 2-byte tables and classic 32-bit table).
    .text
    .p2align 2
    .globl jt_clang_byte
    .type jt_clang_byte,%function
jt_clang_byte:
    .cfi_startproc
    cmp w0, #3
    b.hi .Ldef0
    mov w8, w0
    adrp x9, .LJTI0_0
    add x9, x9, :lo12:.LJTI0_0
    adr x10, .LBB0_2
    ldrb w11, [x9, x8]
    add x10, x10, x11, lsl #2
    br x10
.LBB0_2: mov w0, #10
    ret
.LBB0_3: mov w0, #11
    ret
.LBB0_4: mov w0, #12
    ret
.LBB0_5: mov w0, #13
    ret
.Ldef0: mov w0, wzr
    ret
    .cfi_endproc
    .size jt_clang_byte, .-jt_clang_byte

    .globl jt_clang_half
    .type jt_clang_half,%function
jt_clang_half:
    .cfi_startproc
    sub w8, w0, #1
    cmp w8, #2
    b.hi .Ldef1
    adrp x9, .LJTI1_0
    add x9, x9, :lo12:.LJTI1_0
    adr x10, .LBB1_2
    ldrh w11, [x9, x8, lsl #1]
    add x10, x10, x11, lsl #2
    br x10
.LBB1_2: mov w0, #20
    ret
.LBB1_3: mov w0, #21
    ret
.LBB1_4: mov w0, #22
    ret
.Ldef1: mov w0, wzr
    ret
    .cfi_endproc
    .size jt_clang_half, .-jt_clang_half

    .globl jt_clang_word
    .type jt_clang_word,%function
jt_clang_word:
    .cfi_startproc
    cmp w0, #4
    b.hs .Ldef2
    adrp x8, .LJTI2_0
    add x8, x8, :lo12:.LJTI2_0
    ldrsw x9, [x8, w0, uxtw #2]
    add x9, x8, x9
    br x9
.LBB2_2: mov w0, #30
    ret
.LBB2_3: mov w0, #31
    ret
.LBB2_4: mov w0, #32
    ret
.LBB2_5: mov w0, #33
    ret
.Ldef2: mov w0, wzr
    ret
    .cfi_endproc
    .size jt_clang_word, .-jt_clang_word

    .section .rodata
    .p2align 2
.LJTI0_0:
    .byte (.LBB0_2-.LBB0_2)>>2
    .byte (.LBB0_3-.LBB0_2)>>2
    .byte (.LBB0_4-.LBB0_2)>>2
    .byte (.LBB0_5-.LBB0_2)>>2
    .p2align 1
.LJTI1_0:
    .hword (.LBB1_2-.LBB1_2)>>2
    .hword (.LBB1_3-.LBB1_2)>>2
    .hword (.LBB1_4-.LBB1_2)>>2
    .p2align 2
.LJTI2_0:
    .word .LBB2_2-.LJTI2_0
    .word .LBB2_3-.LJTI2_0
    .word .LBB2_4-.LJTI2_0
    .word .LBB2_5-.LJTI2_0
