// setjmp and longjmp for the corpus (rt.h), as AAPCS64 needs them: the callee-saved state x19..x29, the return address x30, sp and the low halves
// d8..d15. The names are the standard ones, so that discovery's census of non-local control flow finds them by symbol (aot-recompiler.md 1.3).
// jmp_buf: [0..11] x19..x30, [12] sp, [13..20] d8..d15
    .text

    .global setjmp
    .type   setjmp, %function
    .align  2
setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    mov     w0, #0
    ret
    .size   setjmp, . - setjmp

    .global longjmp
    .type   longjmp, %function
    .align  2
longjmp:                        // x0 = env, w1 = value (0 becomes 1)
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    ldp     d8, d9, [x0, #104]
    ldp     d10, d11, [x0, #120]
    ldp     d12, d13, [x0, #136]
    ldp     d14, d15, [x0, #152]
    cmp     w1, #0
    csinc   w0, w1, wzr, ne
    br      x30
    .size   longjmp, . - longjmp
