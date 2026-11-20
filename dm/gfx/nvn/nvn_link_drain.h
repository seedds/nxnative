// Draining frames that stand in the present queue (pure function, shared by Mac / iOS; unit test tests/t_link_drain.c).
// The display-link callback (nvn_mtl.m NvMetalDisplayLinkDelegate) wakes the game's acquire and then takes a frame from the present queue, so a frame the game presents
// right after a tick waits for the next one. That wait is under one link tick. After a late frame the acquire returns at once to catch up (mtl_link_wait_ticks), two frames
// reach the queue before one tick, the callback shows them in order (one per tick), and from then on every frame waits one tick more than it needs: a standing frame, 33.3 ms
// of input lag at 30 fps and 16.7 ms at 60, which never clears while the game keeps up (Ocarina of Time at a 30 fps cap in a window: 115 ms from the game's present to the
// screen, 82 ms without the standing frame). On a 60 Hz link that holds each frame for two ticks (a 30 fps target in fixed-rate fullscreen) the frames can also be taken on
// the ticks between the acquire's, half a hold more.
// The drain: when every frame taken over LINK_DRAIN_SECS could have gone out at least a whole link tick earlier, while the game was waiting for the link in its acquire
// (it keeps up with room to spare), the next acquire waits that many ticks more, once. The callback shows the queued frames meanwhile, one per tick as before, so no frame
// is dropped or repeated on screen; the game loses those ticks of wall time once. A game that runs slower than the link never waits in its acquire, its queue empties by
// itself, and it is left alone: there the standing frame is the slack that keeps a slightly slow game from locking to half the rate (the display link's frames-in-order rule).
// How much earlier a frame could have gone out has two rules. Without the late latch (nvn_link_latch.h), on the tick before the one it was taken on if it was queued by then
// (nvn_link_drain_spare). With it, also on the tick of the callback that woke its acquire, if its present came early after that callback (nvn_link_latch_spare): so the
// frame the game presents just after each tick, which waits that tick in the queue, is a standing frame too, and a drain of every frame of a run counted so ends in the latch
// (LINK_DRAIN_LATCH): the callback that ends it latches its own present, as do the woken callbacks after it. That frame is also the slack that hides a frame that runs a
// little long, which the latch gives up, so the drain into the latch backs off (below)
#pragma once
#include <math.h>
#include <stdint.h>
#include "nvn_link_latch.h"

#define LINK_DRAIN_MARGIN 0.002    // seconds a frame keeps in the queue before its tick after a drain: the callback must find it queued (the game presents about 1 ms after a tick)
#define LINK_DRAIN_SECS 2.0        // how long the frames must have stood before a drain: a hiccup's catch-up frames are not a standing queue yet
#define LINK_DRAIN_TAKES 30        // and at least this many frames taken in a row
#define LINK_DRAIN_BLOCKED 0.15    // the acquire must have waited for the link at least this share of a tick (and 2 ms) before each of those frames: the game has room to spare
#define LINK_DRAIN_LATCH 0x100     // in a request (nvn_link_drain_next's result, beside the ticks): the drain ends in the late latch
#define LINK_LATCH_KEEP 600        // frames latched in a row after a drain into the latch that count as holding (10 s at 60 fps)
#define LINK_LATCH_BACK_MAX 4      // the most doublings of LINK_DRAIN_SECS a drain into the latch can ask for (32 s)

typedef struct {
    int run;                  // frames taken in a row that waited at least one tick too long in the queue, with the game waiting
    double since, min_age;    // when the run started (seconds), and the shortest queue time in it
    int erun, min_woke;       // with the latch: frames taken in a row that came early after the callback that woke their acquire and were taken at least a tick later, with the
    double esince;            // game waiting, the fewest ticks from that callback to the take, and when this run started. Two runs: a frame that stood under a tick is in the
                              // second only, and kept in the first it would hold that run's shortest queue time under a tick, so a whole standing frame was never drained
    unsigned back;            // the backoff: a drain into the latch needs LINK_DRAIN_SECS << back of such frames
    int in;                   // a drain into the latch is under way or holding
    unsigned stand, held;     // its ticks that still show a frame that stood, and the frames latched in a row since it
    uint64_t drains, entries; // drains asked for, and of them drains into the latch (statistics for the log)
} nvn_link_drain;

// Whole link ticks a frame that waited `age` seconds in the queue could have been presented later and still be taken on the same tick
static inline int nvn_link_drain_spare(double age, double tick) { return tick > 0 && age > LINK_DRAIN_MARGIN ? (int)floor((age - LINK_DRAIN_MARGIN) / tick) : 0; }

// With the late latch: whole link ticks from the callback that woke the frame's acquire to the one that took it, when its present came early after the first one (lead
// seconds after it, under half of room, the seconds from that callback to its targetTimestamp, and an eighth of a tick or more inside the latch's wait from there,
// nvn_link_latch_reach), so the latch would have taken it there; -1 otherwise (lead < 0: no callback woke its acquire, the game was late for the tick; room 0: no latch).
// Ocarina of Time at 60 fps: on an external fixed 120 Hz screen (room 8.3 ms) its presents came 1.3 ms after the wake; on a built-in ProMotion screen in the fixed 60 Hz mode
// (room 16.5 ms) some came 6-13 ms after it and some frames ran 17-20 ms, and without this test each one left an empty tick (42-72 frames shown 33 ms in 25 s, against none
// without the latch). The margin to the wait keeps a later frame of the same scene inside it: half the room leaves 2.1 ms at 8.3 ms of room (82 % of ticks latched, no frame
// dropped) and 6.2 ms at 16.5 ms, and an eighth of a tick (2.1 ms at 60 Hz) is that smallest one. Half the room alone passes the wait where the room is over 1.75 ticks (a
// window on a built-in ProMotion screen: about 33 ms against a wait of 14.6 ms at 60 Hz): a present between the two counted as early and missed its tick, so a drain into
// the latch came every 2 s and left its end tick empty; there the limit is 12.5 ms
static inline int nvn_link_latch_spare(double age, double lead, double room, double tick) {
    if (tick <= 0 || room <= 0 || lead < 0) return -1;
    double limit = room / 2, inside = nvn_link_latch_reach(room, tick) - tick / 8;
    if (limit > inside) limit = inside;
    return lead < limit ? (int)floor((age + lead) / tick + 0.5) : -1;
}

// Once per frame the display-link callback takes. now: the callback's time (seconds); age: how long that frame waited in the present queue (seconds; negative when it was
// presented after this callback started: the latch waited for it); tick: the link's period (seconds); blocked: how long the game's last acquire waited for the link (seconds,
// 0 = it did not wait); ok: 0 when this take says nothing about a standing queue (frames were dropped on it, the link is rescued, present snapshots run the game ahead on
// purpose); lead and room: for nvn_link_latch_spare (room 0 when the latch does not run for this frame). Returns the link ticks the next acquire should wait more, with
// LINK_DRAIN_LATCH when the drain ends in the latch (0 = none).
// The backoff: with no frame standing, a frame that comes late while the latch holds stays up 33 ms where the standing frame would have hidden it, and the frames fall back
// to the queue's slack (one frame shown 33 ms more than without the latch per hitch). A game that hitches every few seconds paid that each time and kept little of the gain:
// TUNIC with four 19 ms frames every 5 s, 17 frames shown 33 ms in 42 s against 12 without the latch, present to screen p50 40.8 against 41.0 ms. So a fallback within
// LINK_LATCH_KEEP latched frames of a drain into the latch, or such a drain that latched nothing, doubles the time the next one needs (up to LINK_LATCH_BACK_MAX doublings),
// and LINK_LATCH_KEEP latched frames in a row reset it. Only these drains count: during a hitch the queue runs empty and the latch takes a late present or two on its own
static inline int nvn_link_drain_next(nvn_link_drain *d, double now, double age, double tick, double blocked, int ok, double lead, double room) {
    if (d->in) {   // after a drain into the latch: a latched frame holds, a frame that stood past the drain's own ticks is a fallback
        if (room <= 0) d->in = 0;   // the latch stopped running for these frames (another link rate, a present interval of 2): no verdict
        else if (age < 0) { d->stand = 0; if (++d->held == LINK_LATCH_KEEP) d->back = 0; }
        else if (!d->held && d->stand) d->stand--;
        else { if (d->held < LINK_LATCH_KEEP && d->back < LINK_LATCH_BACK_MAX) d->back++; d->in = 0; }
    }
    double need = LINK_DRAIN_BLOCKED * tick > 0.002 ? LINK_DRAIN_BLOCKED * tick : 0.002;
    int good = ok && blocked >= need, as = good ? nvn_link_drain_spare(age, tick) : 0, ls = good ? nvn_link_latch_spare(age, lead, room, tick) : 0;
    if (as < 1) d->run = 0;
    else if (!d->run++) { d->since = now; d->min_age = age; } else if (age < d->min_age) d->min_age = age;
    if (ls < 1) d->erun = 0;
    else if (!d->erun++) { d->esince = now; d->min_woke = ls; } else if (ls < d->min_woke) d->min_woke = ls;
    if (d->erun >= LINK_DRAIN_TAKES && now - d->esince >= LINK_DRAIN_SECS * (double)(1u << d->back)) {   // by the frame that came closest to its wake: none would miss its tick
        int k = d->min_woke;
        d->run = d->erun = 0; d->drains++; d->entries++; d->in = 1; d->stand = (unsigned)k; d->held = 0;
        return k | LINK_DRAIN_LATCH;
    }
    int spare = d->run >= LINK_DRAIN_TAKES && now - d->since >= LINK_DRAIN_SECS ? nvn_link_drain_spare(d->min_age, tick) : 0;   // by the frame that waited least
    if (spare < 1) return 0;
    d->run = d->erun = 0; d->drains++;   // the frames that stay early after it count again from here, a tick closer to their wake
    return spare;
}
