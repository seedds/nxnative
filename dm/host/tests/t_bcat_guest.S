// Original AAPCS64 synthetic guest call. The entry comes from swforce exactly as
// the loader resolves the game import. Capture both result registers after BIG.
.text
.p2align 2
.globl _bcat_guest_digest_call
_bcat_guest_digest_call:
    stp x29, x30, [sp, #-64]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    mov x29, sp
    mov x19, sp
    mov x20, x3
    mov x21, x4
    mov x22, x1
    mov x23, x2
    mov sp, x0
    stp x29, x30, [sp, #-16]!
    mov x0, x23
    mov x1, #0xbad
    blr x22
    stp x0, x1, [x20]
    mov x9, sp
    str x9, [x21]
    ldp x29, x30, [sp], #16
    mov sp, x19
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret
