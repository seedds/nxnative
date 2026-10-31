// Stack switching by hand for the fiber program (src/fiber.c), as engines' fibers and coroutines do it: fiber_switch saves the callee-saved
// state (x19..x30, sp, d8..d15) of the running context and loads another one's, then returns to the loaded x30, on the loaded stack. Design
// section 3.4: the return goes to a call site that is not the caller's, with no native frame waiting there. Context layout (22 words, as the
// corpus's jmp_buf): [0..11] x19..x30, [12] sp, [13..20] d8..d15.
// skip_parent / skip_leaf: a callee that returns to its caller's caller (it pops its caller's frame record and frame itself), the shape of a
// return to an address that is not the caller's.
    .text

    .global fiber_switch
    .type   fiber_switch, %function
    .align  2
fiber_switch:                   // x0 = the context to save into, x1 = the context to switch to
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x2, sp
    str     x2, [x0, #96]
    stp     d8, d9, [x0, #104]
    stp     d10, d11, [x0, #120]
    stp     d12, d13, [x0, #136]
    stp     d14, d15, [x0, #152]
    ldp     x19, x20, [x1, #0]
    ldp     x21, x22, [x1, #16]
    ldp     x23, x24, [x1, #32]
    ldp     x25, x26, [x1, #48]
    ldp     x27, x28, [x1, #64]
    ldp     x29, x30, [x1, #80]
    ldr     x2, [x1, #96]
    mov     sp, x2
    ldp     d8, d9, [x1, #104]
    ldp     d10, d11, [x1, #120]
    ldp     d12, d13, [x1, #136]
    ldp     d14, d15, [x1, #152]
    ret
    .size   fiber_switch, . - fiber_switch

// The first x30 of a new fiber (fiber_init): x19 = its function, x20 = its argument. A fiber function never returns
    .global fiber_start
    .type   fiber_start, %function
    .align  2
fiber_start:
    mov     x0, x20
    blr     x19
    brk     #0xf1
    .size   fiber_start, . - fiber_start

    .global skip_parent
    .type   skip_parent, %function
    .align  2
skip_parent:                    // x0 = n: skip_leaf returns n * 3 straight to skip_parent's caller
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      skip_leaf
    add     x0, x0, #1000       // never reached
    ldp     x29, x30, [sp], #16
    ret
    .size   skip_parent, . - skip_parent

    .global skip_leaf
    .type   skip_leaf, %function
    .align  2
skip_leaf:
    add     x0, x0, x0, lsl #1
    ldp     x29, x30, [x29]     // skip_parent's frame record: its caller's x29 and return address
    add     sp, sp, #16         // and its frame: sp as its caller had it
    ret
    .size   skip_leaf, . - skip_leaf
