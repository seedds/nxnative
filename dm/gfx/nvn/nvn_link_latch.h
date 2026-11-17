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

// Seconds the callback waits for the woken acquire's present (cb the callback's time, target_ts the update's targetTimestamp, by which the frame's work should be committed,
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
