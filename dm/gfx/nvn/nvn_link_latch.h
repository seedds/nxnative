// The display-link callback's late latch (pure functions, shared by Mac / iOS; unit test tests/t_link_latch.c).
// The callback (nvn_mtl.m NvMetalDisplayLinkDelegate) wakes the game's acquire and then takes a frame from the present queue. A game presents a little after its acquire
// returns (Ocarina of Time 1.3 ms later), so the frame it took was the one presented just after the previous callback, which had waited a whole period in the queue: on a
// 60 Hz link about 15 ms of input lag on every frame (issue #2648: 40.3 ms from the game's present to the screen with the link, 31.7 ms without it, on a fixed 120 Hz screen).
// With the latch the callback that woke an acquire waits a short, bounded time for that acquire's present and takes it, so the frame goes out on the tick that woke it.
// Waking the acquire early instead, before the callback, fought macOS: in a window the link delivered callbacks late or skipped them after a present that landed a few ms before
// them, and a lead near half a period cost a whole period (docs/reference/frame-rate.md).
#pragma once
#include <stdint.h>

#define LINK_LATCH_GIVE_UP 8   // latches in a row that found no present before the callback stops waiting on every tick
#define LINK_LATCH_PROBE 64    // ticks between tries once it stopped (a game that presents late after its wake, or a scene that changed)

// Seconds the callback waits for the woken acquire's present (cb the callback's time, target_ts the update's targetTimestamp, by which the frame's work should be committed,
// both CACurrentMediaTime seconds; period the link's; misses the latches in a row that timed out; tick the callback's number); 0 = take the queued frame at once.
// At most a third of a period, and never closer than an eighth of a period to targetTimestamp, so the present still has time to be encoded and committed
static inline double nvn_link_latch_wait(double cb, double target_ts, double period, unsigned misses, uint64_t tick) {
    if (period <= 0 || (misses >= LINK_LATCH_GIVE_UP && tick % LINK_LATCH_PROBE)) return 0;
    double until = target_ts - period / 8, cap = cb + period / 3;
    if (until > cap) until = cap;
    return until > cb ? until - cb : 0;
}
