// A minimal, personality-free unwinder for the except program (rt/unwind.c, rt/unwind.S): rt_throw walks the guest's own frames with their CFI
// (.eh_frame_hdr's search table, the FDEs' call frame programs), runs every landing pad its LSDA call-site tables name on the way (the
// cleanups: C++ destructors), and ends in the innermost catcher registered with rt_try (a setjmp continuation). It installs a landing pad as
// libunwind and libgcc do: every callee-saved register and sp of that frame from the unwound context, x0 = the exception, x1 = 0, and a `ret`
// with x30 = the pad. The pad's code calls _Unwind_Resume, which goes on from there. Design section 3.4: the guest's unwinder runs as guest
// code, and its register restore reaches each landing pad through a return to an address that is not the caller's.
#ifndef NXAOT_CORPUS_UNWIND_H
#define NXAOT_CORPUS_UNWIND_H

#include "rt.h"

#ifdef __cplusplus
extern "C" {
#endif

// A catcher: setjmp(c->env) == 0, then rt_try_push(c); a throw that reaches the frame of the function that called setjmp longjmps there with
// the thrown value in c->value (the catcher is popped)
typedef struct rt_catcher {
    jmp_buf env;
    uint64_t value;
    struct rt_catcher *prev;
} rt_catcher;

void rt_try_push(rt_catcher *c);
void rt_try_pop(rt_catcher *c);
__attribute__((noreturn)) void rt_throw(uint64_t value);

// What the unwinder did, for the program's checks
extern volatile uint64_t rt_unwind_frames, rt_unwind_pads;

#ifdef __cplusplus
}
#endif

#endif
