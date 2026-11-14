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
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}

// A frame that something other than this beat paced (the display link's ticks, or no pacing at all: present interval 0): the beat follows it and restarts at its present.
// The beat is only a clock while the caller waits for it. Stepped by the period for frames it did not pace, it ran ahead of real time whenever presents came faster than the
// period, without bound (Ocarina of Time on a 120 Hz variable-refresh link: 27.6 s ahead after 18,000 frames), and the fallback that waits for the beat when the display link
// goes silent or times out then slept the whole lead in one go. Restarted here, the fallback's first wait is at most one period from the last present, and nvn_pace steps
// from there; a caller that waits keeps nvn_pace as it is, including a lead of several periods when its swap chain queues frames ahead (three textures: two periods)
static inline void nvn_pace_follow(nvn_pacer *p, uint64_t now) { p->target = now; }
