// The display-link callback's late latch (pure functions, shared by Mac / iOS; unit test tests/t_link_latch.c).
// The callback (nvn_mtl.m NvMetalDisplayLinkDelegate) wakes the game's acquire and then takes a frame from the present queue. A game presents a little after its acquire
// returns (Ocarina of Time 1.3 ms later), so the frame it took was the one presented just after the previous callback, which had waited a whole period in the queue: on a
// 60 Hz link about 15 ms of input lag on every frame (issue #2648: 40.3 ms from the game's present to the screen with the link, 31.7 ms without it, on a fixed 120 Hz screen).
// With the latch the callback that woke an acquire waits a short, bounded time for that acquire's present and takes it, so the frame goes out on the tick that woke it.
// Waking the acquire early instead, before the callback, fought macOS: in a window the link delivered callbacks late or skipped them after a present that landed a few ms before
// them, and a lead near half a period cost a whole period (docs/reference/frame-rate.md).
// The frame standing in the queue is also the slack that hides a frame that runs a little long. Without it such a frame leaves its tick empty and the one before stays up
// twice as long, so the latch runs only while the game earns it (the gate below), and it never takes a present over a standing frame (that dropped the standing one).
#pragma once
#include <stdint.h>

#define LINK_LATCH_GIVE_UP 8   // latches in a row that found no present before the callback stops waiting on every tick
#define LINK_LATCH_PROBE 64    // ticks between tries once it stopped (a game that presents late after its wake, or a scene that changed)
#define LINK_LATCH_GATE 120    // frames in a row whose present came early enough before the latch is entered (2 s at 60 fps)

// Seconds the callback waits for the woken acquire's present (cb the callback's time, target_ts the update's targetTimestamp, by which the frame's work should be committed,
// both CACurrentMediaTime seconds; period the link's; misses the latches in a row that timed out; tick the callback's number); 0 = take the queued frame at once.
// Up to an eighth of a period before targetTimestamp, so the present still has time to be encoded and committed. The callback waits only when no frame stands in the queue, so
// the wait delays no frame: the tick would be empty without it
static inline double nvn_link_latch_wait(double cb, double target_ts, double period, unsigned misses, uint64_t tick) {
    if (period <= 0 || (misses >= LINK_LATCH_GIVE_UP && tick % LINK_LATCH_PROBE)) return 0;
    double until = target_ts - period / 8;
    return until > cb ? until - cb : 0;
}

// The gate: whether the game's presents come early enough after the callback that woke their acquire for the latch to hold without the queue's slack. One note per frame
// the callback takes: wake_to_present, seconds from that callback to the frame's present (negative when no callback woke its acquire: the game was late for the tick),
// and room, seconds from that callback to its targetTimestamp. A frame counts as early when its present came within half the room; any other frame (a slow one, or one
// past a latch's wait) closes the gate at once, and it opens again only after LINK_LATCH_GATE early frames in a row. Ocarina of Time at 60 fps: on an external fixed
// 120 Hz screen (room 8.3 ms) its presents came 1.3 ms after the wake; on a built-in ProMotion screen in the fixed 60 Hz mode (room 16.5 ms) some frames ran 17-20 ms,
// and without the gate each one left an empty tick (42-72 frames shown 33 ms in 25 s, against none without the latch)
typedef struct { unsigned early; } nvn_link_gate;
static inline void nvn_link_gate_note(nvn_link_gate *g, double wake_to_present, double room) {
    g->early = wake_to_present >= 0 && wake_to_present < room / 2 ? g->early + 1 : 0;
}
static inline int nvn_link_gate_open(const nvn_link_gate *g) { return g->early >= LINK_LATCH_GATE; }
