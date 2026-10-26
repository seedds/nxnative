// Whole-frame active-time controller for Z-A's optional dynamic60 experiment.
// Independent of Mach, game memory, and diagnostics so clock conservation can
// be tested against the deltas actually consumed by a model clock provider.
#pragma once
#include <stdint.h>
#include <string.h>

#define ZA_FRAME_SEED_NS 16666667ull
#define ZA_FRAME_MAX_NS 50000000ull
#define ZA_FRAME_PHASE_NS 2000000ll
#define ZA_FRAME_SUSPEND_NS 250000000ull
enum {
    ZA_FRAME_SEED = 1, ZA_FRAME_FROZEN = 2, ZA_FRAME_LIMITED = 4,
    ZA_FRAME_RESUMED = 8, ZA_FRAME_CLOCK_RESET = 16
};
typedef struct {
    uint64_t guest, wall;
    int64_t ema, debt;
    int initialized;
    uint64_t samples, raw_sum, step_sum, limited_ns, frozen, resumed, resets, seeds;
} za_frame_delta_state;
typedef struct {
    uint64_t raw_ns, step_ns;
    unsigned flags;
    int code;
} za_frame_delta_result;

static inline void za_frame_delta_reset(za_frame_delta_state *s) {
    memset(s, 0, sizeof *s);
}
static inline za_frame_delta_result za_frame_delta_advance(
        za_frame_delta_state *s, uint64_t guest, uint64_t wall) {
    za_frame_delta_result r = {0};
    if (!s->initialized || guest < s->guest || wall < s->wall) {
        r.flags = ZA_FRAME_SEED;
        if (s->initialized) { r.flags |= ZA_FRAME_CLOCK_RESET; s->resets++; }
        s->initialized = 1; s->guest = guest; s->wall = wall;
        s->ema = ZA_FRAME_SEED_NS; s->seeds++;
        // A seed has no measured preceding frame. Keep it out of conservation
        // totals, and report it explicitly instead of pretending it was timed.
        r.step_ns = ZA_FRAME_SEED_NS;
        return r;
    }
    uint64_t raw = guest - s->guest, real = wall - s->wall;
    s->guest = guest; s->wall = wall; r.raw_ns = raw;
    if (real > raw && real - raw > ZA_FRAME_SUSPEND_NS) {
        r.flags |= ZA_FRAME_RESUMED; s->resumed++; s->ema = 0;
    }
    if (!raw) {
        r.flags |= ZA_FRAME_FROZEN; s->frozen++; s->ema = 0;
        // Do not write a zero duration into the shared table: other original
        // callers divide by it. The caller leaves the native pause path alone.
        return r;
    }
    s->samples++; s->raw_sum += raw;
    int64_t bounded = (int64_t)(raw > ZA_FRAME_MAX_NS ? ZA_FRAME_MAX_NS : raw);
    if (raw > ZA_FRAME_MAX_NS) {
        r.flags |= ZA_FRAME_LIMITED; s->limited_ns += raw - ZA_FRAME_MAX_NS;
        // Preserve the ordering before the untouched native 50 ms table tier.
        // Long stalls are explicitly limited, not repaid as acceleration later.
        s->ema = bounded;
    } else if (!s->ema) s->ema = bounded;
    else s->ema += (bounded - s->ema) / 4;

    // An EMA alone loses speed when frame time changes. Carry signed phase
    // error and constrain it to +/-2 ms. Thus raw_sum - limited_ns - step_sum
    // always equals debt, even for asymmetric or periodically signed jitter.
    int64_t owed = s->debt + bounded;
    int64_t lo = owed - ZA_FRAME_PHASE_NS, hi = owed + ZA_FRAME_PHASE_NS;
    if (lo < 1) lo = 1;
    if (hi > (int64_t)ZA_FRAME_MAX_NS) hi = ZA_FRAME_MAX_NS;
    int64_t step = s->ema;
    if (step < lo) step = lo;
    if (step > hi) step = hi;
    s->debt = owed - step; s->step_sum += (uint64_t)step;
    r.step_ns = (uint64_t)step;

    // Only slots 0 and 1 may change. Keep the entire original duration table
    // ordered, including whichever of those two slots remains nominal.
    r.code = s->ema > 21739130; // 46 FPS, with no FPS integer quantization.
    if (r.step_ns < ZA_FRAME_SEED_NS) r.code = 0;
    if (r.step_ns > 33333334ull) r.code = 1;
    return r;
}
