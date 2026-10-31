// C++ exceptions through the LSDA tables (design section 3.4), built as C++ with -fexceptions and no C++ library: objects with destructors
// give every frame cleanup landing pads in its LSDA call-site table, and rt_throw (the personality-free unwinder of rt/unwind.c) walks the frames
// with their CFI, enters each landing pad (the destructors run, then _Unwind_Resume goes on) and ends in the innermost catcher (a setjmp
// continuation). Checked: the destructors that ran and their order, through deep recursion, a call through a function pointer, frames with no
// cleanup, values kept in callee-saved registers across the unwind, nested catchers, a rethrow from a catch, and the unwinder's frame and
// landing-pad counts. On the recompiled code every landing pad is entered through a return to an address that is not the caller's: the frames
// between propagate it (st->pending) to the frame whose call site the pad covers, which resumes there (section 3.3).
#include "rt.h"
#include "unwind.h"

// A C++ object with a destructor: every frame that holds one gets a landing pad that runs it
static volatile uint64_t destroyed, order;
struct Guard {
    uint64_t id;
    explicit Guard(uint64_t i) : id(i) {}
    ~Guard() {
        destroyed = destroyed + 1;
        order = order * 31 + id;
    }
};

// No personality routine runs here (the unwinder enters every pad); the compilers name one in the CIEs of C++ functions
extern "C" int __gxx_personality_v0(int, int, uint64_t, void *, void *) {
    rt_say("[RED] __gxx_personality_v0: the corpus's unwinder runs no personality");
    rt_svc_exit_process();
}

static RT_NOINLINE uint64_t level(unsigned n, unsigned throw_at) {
    Guard g{n};
    if (n == throw_at) {
        rt_throw(1000 + n);
    }
    if (n == 0) {
        return 1;
    }
    return level(n - 1, throw_at) + g.id;  // g is destroyed after the call returns: a landing pad covers it
}

static uint64_t catch_level(unsigned depth, unsigned at) {
    rt_catcher c;
    if (setjmp(c.env) == 0) {
        rt_try_push(&c);
        const uint64_t r = level(depth, at);
        rt_try_pop(&c);
        return r;
    }
    return c.value;
}

// A frame with no cleanup between two that have one, and a call through a pointer the compilers cannot see through
static RT_NOINLINE uint64_t plain(unsigned n) {
    return RT_OPAQUE(n) == 7 ? (rt_throw(77), 0u) : n * 2u;
}
static uint64_t (*volatile plain_ptr)(unsigned) = plain;
static RT_NOINLINE uint64_t through_pointer(unsigned n) {
    Guard g{500 + n};
    return plain_ptr(n) + g.id;
}

// Values held in callee-saved registers by the catcher's caller survive: the frames between restored them (CFI), the unwinder installs them
static RT_NOINLINE uint64_t keep(uint64_t seed) {
    uint64_t a = RT_OPAQUE(seed) * 3, b = RT_OPAQUE(seed) ^ 0x5555, c = RT_OPAQUE(seed) + 17;
    uint64_t caught = 0;
    for (unsigned i = 0; i < 4; ++i) {
        caught += catch_level(6, 3);  // a, b and c live across these calls, in callee-saved registers
        a += i;
        b ^= i;
        c -= i;
    }
    return rt_mix(rt_mix(rt_mix(rt_mix(RT_MIX_INIT, a), b), c), caught);
}

// A catch that throws again: the inner catcher's frame is gone, the outer one takes the second throw
static RT_NOINLINE uint64_t rethrow(unsigned n) {
    rt_catcher inner;
    Guard g{900};
    if (setjmp(inner.env) == 0) {
        rt_try_push(&inner);
        level(n, n / 2);
        rt_try_pop(&inner);
        return 0;
    }
    rt_throw(inner.value + 5000);  // g's pad runs on the way out
}
static uint64_t catch_rethrow(unsigned n) {
    rt_catcher outer;
    if (setjmp(outer.env) == 0) {
        rt_try_push(&outer);
        const uint64_t r = rethrow(n);
        rt_try_pop(&outer);
        return r;
    }
    return outer.value;
}

// crt0.S calls main by its C name (freestanding C++ gives main no special linkage)
extern "C" int main(void) {
    destroyed = order = 0;
    RT_CHECK64("normal.value", catch_level(RT_OPAQUE(5u), 99), 1 + 1 + 2 + 3 + 4 + 5);
    RT_CHECK64("normal.destroyed", destroyed, 6);
    rt_note("normal.order", order, 16);

    destroyed = order = 0;
    RT_CHECK64("throw.value", catch_level(RT_OPAQUE(8u), 0), 1000);
    RT_CHECK64("throw.destroyed", destroyed, 9);
    rt_note("throw.order", order, 16);

    destroyed = order = 0;
    RT_CHECK64("deep.value", catch_level(RT_OPAQUE(200u), 3), 1003);
    RT_CHECK64("deep.destroyed", destroyed, 198);
    rt_note("deep.order", order, 16);

    destroyed = order = 0;
    {
        rt_catcher c;
        uint64_t v = 0;
        if (setjmp(c.env) == 0) {
            rt_try_push(&c);
            v = through_pointer(RT_OPAQUE(7u));
            rt_try_pop(&c);
        } else {
            v = c.value;
        }
        RT_CHECK64("pointer.value", v, 77);
        RT_CHECK64("pointer.destroyed", destroyed, 1);
        RT_CHECK64("pointer.order", order, 507);
    }

    destroyed = order = 0;
    RT_CHECK64("callee_saved", keep(RT_OPAQUE(0x1234567ull)),
               rt_mix(rt_mix(rt_mix(rt_mix(RT_MIX_INIT, 0x1234567ull * 3 + 6), (0x1234567ull ^ 0x5555) ^ 0 ^ 1 ^ 2 ^ 3), 0x1234567ull + 17 - 6),
                      4 * 1003));
    RT_CHECK64("callee_saved.destroyed", destroyed, 4 * 4);

    destroyed = order = 0;
    RT_CHECK64("rethrow.value", catch_rethrow(RT_OPAQUE(10u)), 1005 + 5000);
    RT_CHECK64("rethrow.destroyed", destroyed, 6 + 1);
    rt_note("rethrow.order", order, 16);

    rt_note("unwind.frames", rt_unwind_frames, 8);
    rt_note("unwind.pads", rt_unwind_pads, 8);
    return rt_finish();
}
