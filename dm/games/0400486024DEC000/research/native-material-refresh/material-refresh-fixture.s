// Isolated experiment: refresh an already initialized material through its
// existing native routine if its selected source entry has finished preparing.
// No reference tag is cleared here. No source or installed game is modified.
.text
.p2align 2
.globl _test_entry, _call_refresh, _refresh_probe, _original_pac, _branch_back
_test_entry:
    sub sp, sp, #704
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x30, [sp, #144]
    stp q0, q1, [sp, #160]
    stp q2, q3, [sp, #192]
    stp q4, q5, [sp, #224]
    stp q6, q7, [sp, #256]
    stp q8, q9, [sp, #288]
    stp q10, q11, [sp, #320]
    stp q12, q13, [sp, #352]
    stp q14, q15, [sp, #384]
    stp q16, q17, [sp, #416]
    stp q18, q19, [sp, #448]
    stp q20, q21, [sp, #480]
    stp q22, q23, [sp, #512]
    stp q24, q25, [sp, #544]
    stp q26, q27, [sp, #576]
    stp q28, q29, [sp, #608]
    stp q30, q31, [sp, #640]
    mrs x9, nzcv
    mrs x10, fpcr
    mrs x11, fpsr
    str x9, [sp, #672]
    str x10, [sp, #680]
    str x11, [sp, #688]
    mov x9, x1
    cbz x9, done
    cbz x4, done
    ldrb w10, [x9, #0x59]
    tbz w10, #0, done
    tbnz w10, #1, done
    ldr x10, [x9, #0x68]
    cbz x10, done
    ldrb w11, [x4, #0x62]
    cmp w11, #255
    b.ne pass_selected
    ldrb w11, [x4, #0x39]
    cmp w11, #15
    b.hs done
    add x12, x9, #0x2c
    ldrb w11, [x12, x11]
pass_selected:
    cmp w11, #23
    b.hs done
    add x12, x9, #0x3b
    ldrsb w13, [x12, x11]
    tbnz w13, #31, done
    ldrb w14, [x9, #0x5e]
    cmp w13, w14
    b.hs done
    add x16, x10, x11, lsl #6
    ldrb w17, [x16, #0x3d]
    cmp w17, #23
    b.hs done
    ldrsb w14, [x12, x17]
    cmp w14, w13
    b.ne done
    add x16, x10, x17, lsl #6
    ldrb w14, [x16, #0x3d]
    cmp w14, w17
    b.ne done
    ldp x14, x15, [x16, #0x18]
    cbz x14, done
    cbz x15, done
    tst x15, #3
    b.ne done
    ldr x16, [x9, #0x60]
    cbz x16, done
    ldrb w17, [x9, #0x5f]
    cmp w17, #16
    b.lo done
    umaddl x16, w17, w13, x16
    ldr x17, [sp]
    cmp x16, x17
    b.ne done
    ldp x10, x11, [x16]
    cmp x10, x14
    b.ne refresh
    cmp x11, x15
    b.eq done
refresh:
    mov x0, x9
    mov x1, xzr
_call_refresh:
    bl _test_refresh // Patched to BL main+0x501a70 by the experiment builder.
    ldr x0, [sp, #8] // Original material, for the opt-in post-refresh probe.
_refresh_probe:
    nop
 done:
    ldr x9, [sp, #672]
    ldr x10, [sp, #680]
    ldr x11, [sp, #688]
    msr nzcv, x9
    msr fpcr, x10
    msr fpsr, x11
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x30, [sp, #144]
    ldp q0, q1, [sp, #160]
    ldp q2, q3, [sp, #192]
    ldp q4, q5, [sp, #224]
    ldp q6, q7, [sp, #256]
    ldp q8, q9, [sp, #288]
    ldp q10, q11, [sp, #320]
    ldp q12, q13, [sp, #352]
    ldp q14, q15, [sp, #384]
    ldp q16, q17, [sp, #416]
    ldp q18, q19, [sp, #448]
    ldp q20, q21, [sp, #480]
    ldp q22, q23, [sp, #512]
    ldp q24, q25, [sp, #544]
    ldp q26, q27, [sp, #576]
    ldp q28, q29, [sp, #608]
    ldp q30, q31, [sp, #640]
    add sp, sp, #704
_original_pac:
    pacibsp
_branch_back:
    retab // Patched to B main+0x6fc3c.
