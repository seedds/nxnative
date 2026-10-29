// setjmp and longjmp (rt/setjmp.S): the second return of setjmp with the value longjmp passes (0 becomes 1), a longjmp out of deep recursion and
// through a function pointer, nested handlers in the manner of an exception runtime (try / throw / rethrow on a stack of jmp_bufs), and the
// callee-saved state (x19..x28, d8..d15, the frame) a longjmp restores. These are the non-local transfers design section 3.4 resumes at setjmp
// continuations; the dispatcher and the run time that follow them are later tasks, so today only the native run and the dynarmic reference run them.
#include "rt.h"

static jmp_buf env_main;
static volatile unsigned depth_reached;

static RT_NOINLINE void deep(unsigned n, unsigned target, jmp_buf env) {
    volatile uint64_t frame[4] = {n, n * 2, n * 3, n * 4};  // a frame of its own on every level
    depth_reached = n + (unsigned)(frame[1] - frame[0] - n);
    if (n == target) {
        longjmp(env, (int)n);
    }
    if (n > 1000) {
        return;
    }
    deep(n + 1, target, env);
    frame[2] += 1;  // not reached
}

static RT_NOINLINE void thrower(jmp_buf env, int v) {
    longjmp(env, v);
}
static void (*volatile thrower_ptr)(jmp_buf, int) = thrower;

// A handler stack: try pushes a jmp_buf, throw longjmps to the top one
static jmp_buf *handlers[8];
static unsigned nhandlers;
static RT_NOINLINE void throw_code(int code) {
    if (!nhandlers) {
        rt_say("[RED] sjlj: throw with no handler");
        rt_svc_exit_process();
    }
    longjmp(*handlers[--nhandlers], code);
}
static RT_NOINLINE int inner_try(int code) {
    jmp_buf env;
    handlers[nhandlers++] = &env;
    const int r = setjmp(env);
    if (r == 0) {
        throw_code(code);
    }
    if (r == 7) {
        return 70;  // handled here
    }
    throw_code(r + 100);  // rethrow to the outer handler
    return -1;
}
static RT_NOINLINE int outer_try(int code) {
    jmp_buf env;
    handlers[nhandlers++] = &env;
    const int r = setjmp(env);
    if (r == 0) {
        const int v = inner_try(code);
        --nhandlers;
        return v;
    }
    return r;
}

// Callee-saved registers across a longjmp: values held in x19..x28 and d8..d15 by the caller survive, because setjmp saved them and longjmp
// restores them; the clobbers make the compiler keep the values there and the callee overwrite them
__attribute__((noreturn)) static RT_NOINLINE void clobber_and_jump(jmp_buf env) {
    __asm__ volatile("mov x19, #1\n mov x20, #2\n mov x21, #3\n mov x22, #4\n mov x23, #5\n mov x24, #6\n mov x25, #7\n mov x26, #8\n"
                     "mov x27, #9\n mov x28, #10\n fmov d8, xzr\n fmov d9, xzr\n fmov d10, xzr\n fmov d11, xzr\n fmov d12, xzr\n"
                     "fmov d13, xzr\n fmov d14, xzr\n fmov d15, xzr"
                     ::: "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15");
    longjmp(env, 3);
}
// The values are not changed between setjmp and longjmp, so they are defined after it (C11 7.13.2.1); GCC warns all the same
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
#endif
static RT_NOINLINE uint64_t callee_saved(uint64_t seed) {
    register uint64_t r19 __asm__("x19") = seed ^ 0x1919;
    register uint64_t r23 __asm__("x23") = seed * 23;
    register uint64_t r28 __asm__("x28") = seed + 0x2828;
    register double d8 __asm__("d8") = (double)(seed & 0xffff) * 0.5;
    register double d15 __asm__("d15") = 1.0 / 3.0;
    __asm__ volatile("" : "+r"(r19), "+r"(r23), "+r"(r28), "+w"(d8), "+w"(d15));
    jmp_buf env;
    if (setjmp(env) == 0) {
        clobber_and_jump(env);
    }
    __asm__ volatile("" : "+r"(r19), "+r"(r23), "+r"(r28), "+w"(d8), "+w"(d15));
    const double t8 = d8, t15 = d15;
    uint64_t d8bits, d15bits;
    memcpy(&d8bits, &t8, 8);
    memcpy(&d15bits, &t15, 8);
    return rt_mix(rt_mix(rt_mix(rt_mix(rt_mix(RT_MIX_INIT, r19), r23), r28), d8bits), d15bits);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
static uint64_t callee_saved_expected(uint64_t seed) {
    const double d8 = (double)(seed & 0xffff) * 0.5, d15 = 1.0 / 3.0;
    uint64_t d8bits, d15bits;
    memcpy(&d8bits, &d8, 8);
    memcpy(&d15bits, &d15, 8);
    return rt_mix(rt_mix(rt_mix(rt_mix(rt_mix(RT_MIX_INIT, seed ^ 0x1919), seed * 23), seed + 0x2828), d8bits), d15bits);
}

int main(void) {
    volatile int rounds = 0;
    const int r = setjmp(env_main);
    rounds = rounds + 1;
    if (r < 3) {
        longjmp(env_main, r + 1);
    }
    RT_CHECK32("setjmp.value", r, 3);
    RT_CHECK32("setjmp.rounds", rounds, 4);

    jmp_buf env;
    int v = setjmp(env);
    if (v == 0) {
        thrower(env, 0);
    }
    RT_CHECK32("longjmp.zero_is_one", v, 1);

    v = setjmp(env);
    if (v == 0) {
        deep(0, 40, env);
    }
    RT_CHECK32("longjmp.deep", v, 40);
    RT_CHECK32("longjmp.depth", depth_reached, 40);

    v = setjmp(env);
    if (v == 0) {
        thrower_ptr(env, 0x1234);
    }
    RT_CHECK32("longjmp.pointer", v, 0x1234);

    RT_CHECK32("handlers.caught_inner", outer_try(7), 70);
    RT_CHECK32("handlers.rethrown", outer_try(9), 109);
    RT_CHECK32("handlers.empty", nhandlers, 0);

    RT_CHECK64("callee_saved", callee_saved(RT_OPAQUE(0xabcdef12345ull)), callee_saved_expected(0xabcdef12345ull));
    return rt_finish();
}
