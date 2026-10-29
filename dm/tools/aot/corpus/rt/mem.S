// memcpy, memmove, memset and memcmp for the corpus (rt.h): the compilers emit calls to them for block copies and clears even in freestanding
// code. In assembly, so that no compiler turns their loops back into calls to themselves. Byte loops: the corpus copies little
    .text

    .global memcpy
    .type   memcpy, %function
    .align  2
memcpy:                         // x0 = dst, x1 = src, x2 = n; returns dst
    mov     x3, x0
    cbz     x2, 2f
1:  ldrb    w4, [x1], #1
    strb    w4, [x3], #1
    subs    x2, x2, #1
    b.ne    1b
2:  ret
    .size   memcpy, . - memcpy

    .global memmove
    .type   memmove, %function
    .align  2
memmove:                        // copies backwards when dst lies above src
    cmp     x0, x1
    b.ls    memcpy
    cbz     x2, 2f
    add     x3, x0, x2
    add     x1, x1, x2
1:  ldrb    w4, [x1, #-1]!
    strb    w4, [x3, #-1]!
    subs    x2, x2, #1
    b.ne    1b
2:  ret
    .size   memmove, . - memmove

    .global memset
    .type   memset, %function
    .align  2
memset:                         // x0 = dst, w1 = byte, x2 = n; returns dst
    mov     x3, x0
    cbz     x2, 2f
1:  strb    w1, [x3], #1
    subs    x2, x2, #1
    b.ne    1b
2:  ret
    .size   memset, . - memset

    .global memcmp
    .type   memcmp, %function
    .align  2
memcmp:                         // x0, x1, x2 = n; returns the difference of the first differing bytes
    cbz     x2, 2f
1:  ldrb    w3, [x0], #1
    ldrb    w4, [x1], #1
    subs    w5, w3, w4
    b.ne    3f
    subs    x2, x2, #1
    b.ne    1b
2:  mov     w0, #0
    ret
3:  mov     w0, w5
    ret
    .size   memcmp, . - memcmp
