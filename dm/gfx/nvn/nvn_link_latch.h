// The display-link callback's late latch (pure function, shared by Mac / iOS; unit test tests/t_link_latch.c).
// The callback (nvn_mtl.m NvMetalDisplayLinkDelegate) wakes the game's acquire and then takes a frame from the present queue. A game presents a little after its acquire
// returns (Ocarina of Time 1.3 ms later), so the frame it took was the one presented just after the previous callback, which had waited a whole period in the queue: on a
// 60 Hz link about 15 ms of input lag on every frame (issue #2648: 40.3 ms from the game's present to the screen with the link, 31.7 ms without it, on a fixed 120 Hz screen).
// With the latch the callback that woke an acquire waits a short, bounded time for that acquire's present and takes it, so the frame goes out on the tick that woke it.
// Waking the acquire early instead, before the callback, fought macOS: in a window the link delivered callbacks late or skipped them after a present that landed a few ms before
// them, and a lead near half a period cost a whole period (docs/reference/frame-rate.md).
// The latch waits only into an empty queue: it never takes a present over a standing frame (that dropped the standing one). The queue runs empty through the drain
// (nvn_link_drain.h), which counts the frame presented just after a tick as a standing frame when it came early after its wake, and backs off for games whose frames
// sometimes run long: that frame is also the queue's slack.
#pragma once
#include <stdint.h>

#define LINK_LATCH_GIVE_UP 8   // latches in a row that found no present before the callback stops waiting on every tick
#define LINK_LATCH_PROBE 64    // woken callbacks between tries once it stopped (a game that presents late after its wake, or a scene that changed)
#define LINK_LATCH_SKIP_GAP 1.35 // callback interval past this share of a period after a latched present counts as a skipped tick (above 144 Hz virtual-tick 1.25; catches 25 and 33.3 ms slips)
#define LINK_LATCH_SKIP_MAX 0.25 // gaps this long or longer (seconds) are pauses or stalls, not a skipped tick caused by the previous latched present

// Seconds the callback waits for the woken acquire's present (cb the callback's time, target_ts the update's targetTimestamp, by which the frame's GPU work must finish,
// both CACurrentMediaTime seconds; period the link's; misses the latches in a row that timed out; woken the callbacks that woke an acquire so far, this one included;
// drain_end: this callback ends a drain into the latch); 0 = take the queued frame at once.
// Up to an eighth of a period before targetTimestamp, so the present still has time to be encoded and committed, and never past an eighth of a period before the next
// callback (cb + period), so a present that does not come cannot hold the callback thread past the next tick where the room is longer than a period (frame latency 2).
// The callback waits only when no frame stands in the queue, so the wait delays no frame: the tick would be empty without it. After LINK_LATCH_GIVE_UP timeouts in a row it
// waits only on every LINK_LATCH_PROBE-th woken callback (counted in woken callbacks, so an acquire of two ticks probes too) and at the end of a drain into the latch.
// nvn_link_latch_reach is that wait's bound from room, the seconds from the callback to its targetTimestamp; the drain's early-present rule (nvn_link_latch_spare) stays
// an eighth of a period inside it, so a present it counts as early is one the latch would have taken, with a margin for the frames after it
static inline double nvn_link_latch_reach(double room, double period) {
    if (period <= 0) return 0;
    double r = room - period / 8, cap = period - period / 8;
    if (r > cap) r = cap;
    return r > 0 ? r : 0;
}
static inline double nvn_link_latch_wait(double cb, double target_ts, double period, unsigned misses, uint64_t woken, int drain_end) {
    if (misses >= LINK_LATCH_GIVE_UP && !drain_end && woken % LINK_LATCH_PROBE) return 0;
    return nvn_link_latch_reach(target_ts - cb, period);
}

// Runtime health of the latch across callbacks: a latched present whose GPU work finishes after update.targetTimestamp (or causes CAMetalDisplayLink at frame latency 1
// to skip the next 60 Hz callback) leaves the queue empty across the skipped tick, so the next callback latches again and locks the game to 30 fps with zero CPU timeouts
// (issue #3251). Exiting the latch for one tick (skipping the wait and entering give-up) lets the woken present stand in the queue, restores 60 fps slack on the next tick,
// and feeds nvn_link_drain_next's backoff.
typedef struct {
    int prev;              // 1 if the previous callback latched a present
    unsigned skips, clean; // skipped ticks after latched presents, and clean latched ticks since the last skip
} nvn_link_latch_health;

// Called at the start of a display-link callback while the latch is enabled. gap: seconds since the previous callback (<= 0 on the first); period: tick period (seconds);
// late_gpu: 1 if any latched present's GPU completion passed its targetTimestamp since the previous check; fresh_entry: 1 right after a drain into the latch (before
// LINK_LATCH_GIVE_UP frames have held), where a single skipped tick exits at once. Returns 1 when the callback should exit the latch now.
static inline int nvn_link_latch_check(nvn_link_latch_health *h, double gap, double period, int late_gpu, int fresh_entry) {
    if (late_gpu) { h->skips = 0; h->clean = 0; return 1; }
    if (!h->prev) { h->skips = 0; h->clean = 0; return 0; }
    if (period > 0 && gap > LINK_LATCH_SKIP_GAP * period && gap < LINK_LATCH_SKIP_MAX) {
        h->skips++; h->clean = 0;
        if (h->skips >= 2 || fresh_entry) { h->skips = 0; return 1; }
        return 0;
    }
    if (period > 0 && gap > 0 && gap <= LINK_LATCH_SKIP_GAP * period) {
        if (++h->clean >= LINK_LATCH_GIVE_UP) h->skips = 0;
    }
    return 0;
}
