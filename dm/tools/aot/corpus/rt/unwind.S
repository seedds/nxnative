// The register halves of rt/unwind.c. Context layout (rt_uw_context): [0..30] x0..x30, [31] sp, [32] pc, [33..40] d8..d15 (328 bytes).
    .text

// SAVE_CALLER: a context on the stack that describes the caller at this call: x19..x30, sp (as the caller had it), pc = x30 (the return
// address), d8..d15. The walk starts from that frame, so these functions need no unwind entry of their own (a noreturn function's own frame may
// not even hold its return address)
    .macro SAVE_CALLER
    sub     sp, sp, #336
    stp     x19, x20, [sp, #152]
    stp     x21, x22, [sp, #168]
    stp     x23, x24, [sp, #184]
    stp     x25, x26, [sp, #200]
    stp     x27, x28, [sp, #216]
    stp     x29, x30, [sp, #232]
    add     x9, sp, #336
    stp     x9, x30, [sp, #248]
    stp     d8, d9, [sp, #264]
    stp     d10, d11, [sp, #280]
    stp     d12, d13, [sp, #296]
    stp     d14, d15, [sp, #312]
    .endm

// rt_throw(x0 = value): unwinds from the caller (rt_uw_raise)
    .global rt_throw
    .type   rt_throw, %function
    .align  2
rt_throw:
    SAVE_CALLER
    mov     x1, x0
    mov     x0, sp
    b       rt_uw_raise          // a tail call: its frame goes below the context, which stays where it is
    .size   rt_throw, . - rt_throw

// _Unwind_Resume(x0 = the exception): what a cleanup landing pad calls when its cleanups ran; the walk goes on from the pad's frame
    .global _Unwind_Resume
    .type   _Unwind_Resume, %function
    .align  2
_Unwind_Resume:
    SAVE_CALLER
    mov     x0, sp
    b       rt_uw_resume
    .size   _Unwind_Resume, . - _Unwind_Resume

// rt_uw_install(x0 = context, x1 = landing pad, x2 = exception): x19..x29, sp and d8..d15 from the context, x0 = the exception, x1 = 0 (the
// selector of a cleanup), then a return to the pad (x30 = the pad), as libunwind's jumpto and libgcc's eh_return do it
    .global rt_uw_install
    .type   rt_uw_install, %function
    .align  2
rt_uw_install:
    mov     x30, x1
    mov     x16, x0
    mov     x0, x2
    mov     x1, #0
    ldp     x19, x20, [x16, #152]
    ldp     x21, x22, [x16, #168]
    ldp     x23, x24, [x16, #184]
    ldp     x25, x26, [x16, #200]
    ldp     x27, x28, [x16, #216]
    ldr     x29, [x16, #232]
    ldp     d8, d9, [x16, #264]
    ldp     d10, d11, [x16, #280]
    ldp     d12, d13, [x16, #296]
    ldp     d14, d15, [x16, #312]
    ldr     x17, [x16, #248]
    mov     sp, x17
    ret
    .size   rt_uw_install, . - rt_uw_install
