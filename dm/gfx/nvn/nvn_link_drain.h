// Draining frames that stand in the present queue (pure function, shared by Mac / iOS; unit test tests/t_link_drain.c).
// The display-link callback (nvn_mtl.m NvMetalDisplayLinkDelegate) wakes the game's acquire and then takes a frame from the present queue, so a frame the game presents
// right after a tick waits for the next one. That wait is under one link tick. After a late frame the acquire returns at once to catch up (mtl_link_wait_ticks), two frames
// reach the queue before one tick, the callback shows them in order (one per tick), and from then on every frame waits one tick more than it needs: a standing frame, 33.3 ms
// of input lag at 30 fps and 16.7 ms at 60, which never clears while the game keeps up (Ocarina of Time at a 30 fps cap in a window: 115 ms from the game's present to the
// screen, 82 ms without the standing frame). On a 60 Hz link that holds each frame for two ticks (a 30 fps target in fixed-rate fullscreen) the frames can also be taken on
// the ticks between the acquire's, half a hold more.
// The drain: when every frame taken over LINK_DRAIN_SECS waited at least a whole link tick longer than it had to, while the game was waiting for the link in its acquire
// (it keeps up with room to spare), the next acquire waits that many ticks more, once. The callback shows the queued frames meanwhile, one per tick as before, so no frame
// is dropped or repeated on screen; the game loses those ticks of wall time once. A game that runs slower than the link never waits in its acquire, its queue empties by
// itself, and it is left alone: there the standing frame is the slack that keeps a slightly slow game from locking to half the rate (the display link's frames-in-order rule).
#pragma once
#include <math.h>
#include <stdint.h>

#define LINK_DRAIN_MARGIN 0.002    // seconds a frame keeps in the queue before its tick after a drain: the callback must find it queued (the game presents about 1 ms after a tick)
#define LINK_DRAIN_SECS 2.0        // how long the frames must have stood before a drain: a hiccup's catch-up frames are not a standing queue yet
#define LINK_DRAIN_TAKES 30        // and at least this many frames taken in a row
#define LINK_DRAIN_BLOCKED 0.15    // the acquire must have waited for the link at least this share of a tick (and 2 ms) before each of those frames: the game has room to spare

typedef struct {
    int run;                  // frames taken in a row that stood at least one tick too long with the game waiting
    double since, min_age;    // when the run started (seconds), and the shortest queue time in it
    uint64_t drains;          // drains asked for (statistics for the log)
} nvn_link_drain;

// Whole link ticks a frame that waited `age` seconds in the queue could have been presented later and still be taken on the same tick
static inline int nvn_link_drain_spare(double age, double tick) { return tick > 0 && age > LINK_DRAIN_MARGIN ? (int)floor((age - LINK_DRAIN_MARGIN) / tick) : 0; }

// Once per frame the display-link callback takes. now: the callback's time (seconds); age: how long that frame waited in the present queue (seconds); tick: the link's
// period (seconds); blocked: how long the game's last acquire waited for the link (seconds, 0 = it did not wait); ok: 0 when this take says nothing about a standing queue
// (frames were dropped on it, the link is rescued, present snapshots run the game ahead on purpose). Returns the link ticks the next acquire should wait more (0 = none).
static inline int nvn_link_drain_next(nvn_link_drain *d, double now, double age, double tick, double blocked, int ok) {
    double need = LINK_DRAIN_BLOCKED * tick > 0.002 ? LINK_DRAIN_BLOCKED * tick : 0.002;
    if (!ok || nvn_link_drain_spare(age, tick) < 1 || blocked < need) { d->run = 0; return 0; }
    if (!d->run++) { d->since = now; d->min_age = age; } else if (age < d->min_age) d->min_age = age;
    if (d->run < LINK_DRAIN_TAKES || now - d->since < LINK_DRAIN_SECS) return 0;
    int spare = nvn_link_drain_spare(d->min_age, tick);   // by the frame that waited least, so none of them would have missed its tick
    d->run = 0; d->drains++;
    return spare;
}
