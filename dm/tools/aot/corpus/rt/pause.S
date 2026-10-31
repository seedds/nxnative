// The register-invariant loop the host's pause test checks paused threads against (docs/design/aot-recompiler.md section 6.2; src/rt/host.cpp,
// PauseTest): nxaot_pause_loop(x0 = n) counts x19 from 0 to n with x20 = 2 * x19 and x21 = ~x19 kept at the head of every iteration, so that
// a thread stopped there (a poll on the loop's back-edge records pc = the head) must show the invariant in its state block, whatever iteration
// it was in. A state block written back incompletely, or from another moment than pc, breaks it. The head's label is a global symbol without a
// type (no function entry for discovery), which the host finds by name. Returns x19 (n); x19..x21 are restored as AAPCS64 requires.
    .text

    .global nxaot_pause_loop
    .type   nxaot_pause_loop, %function
    .align  2
nxaot_pause_loop:
    stp     x19, x20, [sp, #-32]!
    str     x21, [sp, #16]
    mov     x19, #0
    mov     x20, #0
    mvn     x21, x19
    cbz     x0, 2f
    .global nxaot_pause_loop_head
nxaot_pause_loop_head:
    add     x19, x19, #1
    add     x20, x20, #2
    mvn     x21, x19
    cmp     x19, x0
    b.lo    nxaot_pause_loop_head
2:  mov     x0, x19
    cmp     x20, x19, lsl #1     // the invariant once more, at the end: x0 = n only when it held
    csinv   x0, x0, xzr, eq
    ldr     x21, [sp, #16]
    ldp     x19, x20, [sp], #32
    ret
    .size   nxaot_pause_loop, . - nxaot_pause_loop
