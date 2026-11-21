// Frame-rate counter (DM_SHOW_FPS, docs/settings.md#frame-rate-counter): the frames that reached the screen. nvn_mtl.m notes every drawable's presented callback
// (fps_tally_presented, for NVN and the Vulkan layer alike); a drawable the system discarded reports a presented time of 0 and is not counted, nor is a frame the
// display link merged away (it never gets a drawable). The front end reads the tally on a timer (fps_meter_read) and shows the rate (fps_label). Plain C, no Metal:
// tests/t_fps_meter.c checks it
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

// Written on the presented callbacks' thread, read on the main thread
typedef struct { _Atomic uint64_t shown, generated; } fps_tally;
// generated: the frame was frame generation's (nvn_mtl.m's serial 2^61 + n with bit 40), not a picture the game rendered
static inline void fps_tally_presented(fps_tally *t, double presented_time, int generated) {
    if (!(presented_time > 0)) return;   // discarded: never on screen (also NaN)
    atomic_fetch_add_explicit(&t->shown, 1, memory_order_relaxed);
    if (generated) atomic_fetch_add_explicit(&t->generated, 1, memory_order_relaxed);
}

// The rate over the last two samples' span (about a second at a reading every half second), so the number moves twice a second but counts a full second's frames.
// A sample later than max_gap after the one before (the app in the background, a stalled main thread) starts over: no reading for a span the counter did not watch
typedef struct { uint64_t shown[2], generated[2]; double t[2]; int n; } fps_meter;
typedef struct { double shown, real; int generated; } fps_reading;   // frames on screen per second, of those the game's own; generated: frame generation added some
static inline void fps_meter_reset(fps_meter *m) { m->n = 0; }
// 1 with a reading in *out, 0 while the meter has no earlier sample to compare with
static inline int fps_meter_read(fps_meter *m, uint64_t shown, uint64_t generated, double now, double max_gap, fps_reading *out) {
    if (m->n && (now - m->t[m->n - 1] > max_gap || now <= m->t[m->n - 1] || shown < m->shown[m->n - 1] || generated < m->generated[m->n - 1])) m->n = 0;
    int got = 0;
    if (m->n) {
        double dt = now - m->t[0]; uint64_t s = shown - m->shown[0], g = generated - m->generated[0];
        out->shown = (double)s / dt; out->real = (double)(s - (g < s ? g : s)) / dt; out->generated = g > 0; got = 1;
    }
    if (m->n == 2) { m->shown[0] = m->shown[1]; m->generated[0] = m->generated[1]; m->t[0] = m->t[1]; m->n = 1; }
    m->shown[m->n] = shown; m->generated[m->n] = generated; m->t[m->n] = now; m->n++;
    return got;
}
// "60 fps", or with generated frames in the span "60 fps (40 real)": all frames on screen, then the game's own among them
static inline void fps_label(const fps_reading *r, char *buf, size_t n) {
    if (r->generated) snprintf(buf, n, "%.0f fps (%.0f real)", r->shown, r->real);
    else snprintf(buf, n, "%.0f fps", r->shown);
}
