// Assembly guest caller for t_hle_wait.c: load recognizable registers on an 8 KiB guest stack, call the real wrapper, snapshot its return, restore the host caller ABI.
.text
.p2align 2
.globl _wait_test_invoke
_wait_test_invoke:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    stp x0, x1, [sp, #160]
    adrp x9, _g_wait_test_host_sp@PAGE
    mov x10, sp
    str x10, [x9, _g_wait_test_host_sp@PAGEOFF]
    mov sp, x2
    mov x30, x0
    ldp q0, q1, [x30, #256]
    ldp q2, q3, [x30, #288]
    ldp q4, q5, [x30, #320]
    ldp q6, q7, [x30, #352]
    ldp q8, q9, [x30, #384]
    ldp q10, q11, [x30, #416]
    ldp q12, q13, [x30, #448]
    ldp q14, q15, [x30, #480]
    ldp q16, q17, [x30, #512]
    ldp q18, q19, [x30, #544]
    ldp q20, q21, [x30, #576]
    ldp q22, q23, [x30, #608]
    ldp q24, q25, [x30, #640]
    ldp q26, q27, [x30, #672]
    ldp q28, q29, [x30, #704]
    ldp q30, q31, [x30, #736]
    ldr x9, [x30, #768]
    msr nzcv, x9
    ldr x9, [x30, #776]
    msr fpsr, x9
    ldr x9, [x30, #784]
    msr fpcr, x9
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldr x19, [x30, #152]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    bl _wait_probe_wait
.globl _wait_test_return
_wait_test_return:
    sub sp, sp, #800
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    add x9, sp, #800
    str x9, [sp, #248]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x9, nzcv
    str x9, [sp, #768]
    mrs x9, fpsr
    str x9, [sp, #776]
    mrs x9, fpcr
    str x9, [sp, #784]
    adrp x9, _g_wait_test_host_sp@PAGE
    ldr x9, [x9, _g_wait_test_host_sp@PAGEOFF]
    ldr x0, [x9, #168]
    mov x1, sp
    mov x2, #800
    bl _memcpy
    adrp x9, _g_wait_test_host_sp@PAGE
    ldr x9, [x9, _g_wait_test_host_sp@PAGEOFF]
    mov sp, x9
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #176
    ret
