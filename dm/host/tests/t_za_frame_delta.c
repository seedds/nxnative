// clang -O1 -Wall -Wextra -Werror -o /tmp/t_za_frame_delta t_za_frame_delta.c
#include "../za_frame_delta.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void invariant(const za_frame_delta_state *s, za_frame_delta_result r) {
    assert(llabs(s->debt) <= ZA_FRAME_PHASE_NS);
    assert((int64_t)(s->raw_sum - s->limited_ns - s->step_sum) == s->debt);
    if (r.flags & ZA_FRAME_FROZEN) { assert(!r.step_ns); return; }
    assert(r.step_ns && r.step_ns <= ZA_FRAME_MAX_NS);
    uint64_t table[] = {16666667, 33333334, 50000000, 66666667, 83333334};
    table[r.code] = r.step_ns;
    for (int i = 1; i < 5; i++) assert(table[i - 1] <= table[i]);
}
static void sequence(const char *name, const uint64_t *pattern, size_t n) {
    za_frame_delta_state s = {0}; uint64_t guest = 1000000000, wall = guest;
    za_frame_delta_result r = za_frame_delta_advance(&s, guest, wall);
    assert(r.flags == ZA_FRAME_SEED);
    // This independent game-clock total includes every emitted step, including
    // the seed. No diagnostic epoch/sample counter participates in the check.
    uint64_t consumed = r.step_ns, active = 0;
    for (int i = 0; i < 12000; i++) {
        uint64_t dt = pattern[i % n]; guest += dt; wall += dt; active += dt;
        r = za_frame_delta_advance(&s, guest, wall); invariant(&s, r);
        consumed += r.step_ns;
        assert(llabs((int64_t)(active - (consumed - ZA_FRAME_SEED_NS))) <= ZA_FRAME_PHASE_NS);
    }
    double ratio = (double)(consumed - ZA_FRAME_SEED_NS) / active;
    double tolerance = (double)ZA_FRAME_PHASE_NS / active + 1e-12;
    assert(ratio >= 1 - tolerance && ratio <= 1 + tolerance);
    printf("%s: consumed/active=%.9f error=%lld ns\n", name, ratio,
        (long long)((int64_t)active - (int64_t)(consumed - ZA_FRAME_SEED_NS)));
}
int main(void) {
    const uint64_t sixty[] = {16666667,16666666,16666667};
    const uint64_t one_twenty[] = {8333333,8333334,8333333};
    const uint64_t high_jitter[] = {7500000,9166667,8333333,8000000,8666667};
    const uint64_t steady[] = {27000000};
    const uint64_t jitter[] = {24000000,30000000};
    const uint64_t skew[] = {12000000,40000000,24000000,32000000,27000000};
    const uint64_t positive[] = {27500000,29000000,31500000,30000000,28000000};
    const uint64_t negative[] = {26500000,25000000,22500000,24000000,26000000};
    const uint64_t extremes[] = {1,50000000,16666667,33333334,50000000,1};
    sequence("60 FPS measured time", sixty, 3);
    sequence("120 FPS measured time", one_twenty, 3);
    sequence("120 FPS periodic jitter", high_jitter, 5);
    sequence("37 FPS steady", steady, 1); sequence("signed 3 ms jitter", jitter, 2);
    sequence("asymmetric timing", skew, 5); sequence("positive periodic jitter", positive, 5);
    sequence("negative periodic jitter", negative, 5); sequence("valid boundaries", extremes, 6);

    za_frame_delta_state s = {0}; uint64_t guest = 1000000000, wall = guest;
    za_frame_delta_advance(&s, guest, wall);
    for (int i = 0; i < 20; i++) {
        guest += 27000000; wall += 27000000;
        invariant(&s, za_frame_delta_advance(&s, guest, wall));
    }
    uint64_t before = s.step_sum;
    wall += 300000000000ull; // guest clock is frozen during five minutes in background
    za_frame_delta_result r = za_frame_delta_advance(&s, guest, wall);
    assert((r.flags & (ZA_FRAME_RESUMED | ZA_FRAME_FROZEN)) == (ZA_FRAME_RESUMED | ZA_FRAME_FROZEN));
    assert(s.step_sum == before && !r.step_ns); invariant(&s, r);
    guest += 27000000; wall += 27000000;
    r = za_frame_delta_advance(&s, guest, wall); invariant(&s, r);
    assert(r.step_ns <= 29000000 && !(r.flags & ZA_FRAME_SEED));
    // Resume may occur between two callbacks, with no callback while frozen.
    guest += 27000000; wall += 60027000000ull;
    r = za_frame_delta_advance(&s, guest, wall); invariant(&s, r);
    assert(r.flags & ZA_FRAME_RESUMED); assert(r.step_ns <= 29000000);
    // A real long active stall is capped explicitly; its loss is observable,
    // and no subsequent catch-up frame can accelerate the game.
    guest += 2000000000; wall += 2000000000;
    r = za_frame_delta_advance(&s, guest, wall); invariant(&s, r);
    assert(r.flags & ZA_FRAME_LIMITED); assert(s.limited_ns == 1950000000);
    for (int i = 0; i < 40; i++) {
        guest += 27000000; wall += 27000000;
        r = za_frame_delta_advance(&s, guest, wall); invariant(&s, r);
        assert(r.step_ns <= 31000000);
    }
    r = za_frame_delta_advance(&s, guest - 1, wall); invariant(&s, r);
    assert(r.flags & ZA_FRAME_CLOCK_RESET);
    puts("Z-A frame delta: PASS");
    return 0;
}
