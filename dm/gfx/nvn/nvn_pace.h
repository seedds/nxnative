// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    // The beat runs at most one period ahead of now. A caller that does not wait (acquire paced by the display link instead) presents earlier than the beat whenever
    // frames come faster than the period, and the target then ran ahead without bound: 27.6 s on Ocarina of Time over 18,000 frames on a 120 Hz variable-refresh link, slept
    // in one go by acquire's fallback at the next display-link silence
    if (now <= next) { if (next - now > period) next = now + period; p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}
