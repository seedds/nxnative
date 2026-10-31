// Fibers: two stacks besides the main one and a hand-written context switch (rt/fiber.S), as engines' job systems and coroutines use them.
// A generator fiber yields a sequence to the main context; two fibers pass a token around a ring with the main one, each switching from inside a
// recursion that must return correctly after it is resumed; values held in callee-saved registers (x19..x28, d8..d15) survive the switches;
// and a callee returns straight to its caller's caller (rt/fiber.S, skip_leaf). Design section 3.4: every switch returns to a call site that is
// not the caller's, with no native frame there, so the recompiled code propagates it and the continuation runs in the JIT fallback (section 11)
// until it calls compiled code again; the JIT-only and native runs must print the same lines.
#include "rt.h"

typedef uint64_t fiber_ctx[22];
void fiber_switch(fiber_ctx *from, fiber_ctx *to);
void fiber_start(void);
uint64_t skip_parent(uint64_t n);

static void fiber_init(fiber_ctx *c, void (*fn)(void *), void *arg, uint8_t *stack, size_t size) {
    for (unsigned i = 0; i < 22; ++i) {
        (*c)[i] = 0;
    }
    (*c)[0] = (uint64_t)(uintptr_t)fn;    // x19
    (*c)[1] = (uint64_t)(uintptr_t)arg;   // x20
    (*c)[11] = (uint64_t)(uintptr_t)fiber_start;  // x30: the first switch returns into fiber_start
    (*c)[12] = ((uint64_t)(uintptr_t)stack + size) & ~15ull;  // sp
}

static uint8_t stack_a[32 * 1024] __attribute__((aligned(16)));
static uint8_t stack_b[32 * 1024] __attribute__((aligned(16)));
static fiber_ctx ctx_main, ctx_a, ctx_b;

// ---- A generator: the fiber yields 1, 4, 9, ... to the main context, which sums them ----
static volatile uint64_t yielded;
static void squares(void *arg) {
    const uint64_t n = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = 1; i <= n; ++i) {
        yielded = i * i;
        fiber_switch(&ctx_a, &ctx_main);
    }
    yielded = 0;
    for (;;) {
        fiber_switch(&ctx_a, &ctx_main);  // finished: every further switch yields 0
    }
}

// ---- A ring: main -> a -> b -> main, each fiber switching from the bottom of a recursion of its own ----
static volatile uint64_t token, laps;
static RT_NOINLINE uint64_t descend(unsigned depth, fiber_ctx *self, fiber_ctx *next, uint64_t salt) {
    volatile uint64_t frame[2] = {depth, salt};  // a frame of its own on every level
    if (depth == 0) {
        token = token * 3 + salt;
        fiber_switch(self, next);
        return frame[1];
    }
    return descend(depth - 1, self, next, salt) + frame[0];  // the sum of the depths, after the resume
}
static void ring_a(void *arg) {
    (void)arg;
    for (;;) {
        const uint64_t r = descend(5, &ctx_a, &ctx_b, 7);
        laps += r;  // 7 + (1 + 2 + 3 + 4 + 5) = 22 per lap
    }
}
static void ring_b(void *arg) {
    (void)arg;
    for (;;) {
        const uint64_t r = descend(3, &ctx_b, &ctx_main, 11);
        laps += r * 100;  // (11 + 1 + 2 + 3) * 100 = 1700 per lap
    }
}

// ---- Callee-saved registers across a switch ----
#ifndef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
#endif
static void clobber(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("mov x19, #1\n mov x20, #2\n mov x21, #3\n mov x22, #4\n mov x23, #5\n mov x24, #6\n mov x25, #7\n mov x26, #8\n"
                         "mov x27, #9\n mov x28, #10\n fmov d8, xzr\n fmov d9, xzr\n fmov d10, xzr\n fmov d11, xzr\n fmov d12, xzr\n"
                         "fmov d13, xzr\n fmov d14, xzr\n fmov d15, xzr"
                         ::: "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "d8", "d9", "d10", "d11", "d12", "d13",
                         "d14", "d15");
        fiber_switch(&ctx_b, &ctx_main);
    }
}
static RT_NOINLINE uint64_t callee_saved(uint64_t seed) {
    register uint64_t r19 __asm__("x19") = seed ^ 0x1919;
    register uint64_t r24 __asm__("x24") = seed * 24;
    register uint64_t r28 __asm__("x28") = seed + 0x2828;
    register double d9 __asm__("d9") = (double)(seed & 0xffff) * 0.25;
    register double d14 __asm__("d14") = 1.0 / 7.0;
    __asm__ volatile("" : "+r"(r19), "+r"(r24), "+r"(r28), "+w"(d9), "+w"(d14));
    fiber_switch(&ctx_main, &ctx_b);
    __asm__ volatile("" : "+r"(r19), "+r"(r24), "+r"(r28), "+w"(d9), "+w"(d14));
    const double t9 = d9, t14 = d14;
    uint64_t b9, b14;
    memcpy(&b9, &t9, 8);
    memcpy(&b14, &t14, 8);
    return rt_mix(rt_mix(rt_mix(rt_mix(rt_mix(RT_MIX_INIT, r19), r24), r28), b9), b14);
}
#ifndef __clang__
#pragma GCC diagnostic pop
#endif
static uint64_t callee_saved_expected(uint64_t seed) {
    const double d9 = (double)(seed & 0xffff) * 0.25, d14 = 1.0 / 7.0;
    uint64_t b9, b14;
    memcpy(&b9, &d9, 8);
    memcpy(&b14, &d14, 8);
    return rt_mix(rt_mix(rt_mix(rt_mix(rt_mix(RT_MIX_INIT, seed ^ 0x1919), seed * 24), seed + 0x2828), b9), b14);
}

int main(void) {
    fiber_init(&ctx_a, squares, (void *)(uintptr_t)RT_OPAQUE(40u), stack_a, sizeof stack_a);
    uint64_t sum = 0, n = 0;
    for (;;) {
        fiber_switch(&ctx_main, &ctx_a);
        if (!yielded) {
            break;
        }
        sum += yielded;
        ++n;
    }
    RT_CHECK64("generator.count", n, 40);
    RT_CHECK64("generator.sum", sum, 40 * 41 * 81 / 6);

    token = 1;
    laps = 0;
    fiber_init(&ctx_a, ring_a, 0, stack_a, sizeof stack_a);
    fiber_init(&ctx_b, ring_b, 0, stack_b, sizeof stack_b);
    for (unsigned lap = 0; lap < 25; ++lap) {
        fiber_switch(&ctx_main, &ctx_a);  // a, then b, then back here
    }
    rt_note("ring.token", token, 16);
    RT_CHECK64("ring.laps", laps, 24 * (22 + 1700));  // the 25th lap's sums are not added yet: both fibers wait inside descend

    fiber_init(&ctx_b, clobber, 0, stack_b, sizeof stack_b);
    RT_CHECK64("callee_saved", callee_saved(RT_OPAQUE(0x5eed1234abcdull)), callee_saved_expected(0x5eed1234abcdull));
    RT_CHECK64("callee_saved.again", callee_saved(RT_OPAQUE(0x77ull)), callee_saved_expected(0x77ull));

    RT_CHECK64("skip.return", skip_parent(RT_OPAQUE(14ull)), 42);
    RT_CHECK64("skip.return_twice", skip_parent(RT_OPAQUE(5ull)) + skip_parent(RT_OPAQUE(6ull)), 33);
    return rt_finish();
}
