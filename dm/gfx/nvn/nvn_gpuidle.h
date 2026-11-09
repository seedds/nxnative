// GPU idle accounting for the timing line (pure arithmetic on tagged command-buffer spans; unit test tests/t_gpu_idle.c).
// All times are nanoseconds on one monotonic clock: the GPUStartTime / GPUEndTime of a frame's tagged command buffers (nvn_mtl.m, the g_gpu ring that mtl_gpu_ms / mtl_gpu_span read).
// The idle this reports is idle of the tagged work, which is the game's own submissions (mtl_sub_commit, its sub_cut segments included): the present command buffer, snapshot
// copies, upload batches and old-queue command buffers are not tagged, so a gap where only they run still reads as idle (docs/reference/frame-rate.md).
#pragma once
#include <stdint.h>

// Idle between two frames: from the previous sampled frame's last GPU end to this frame's first GPU start. Both spans must be known and the frames consecutive
// (the tracker keeps the last frame with a known span), else -1 = not measured: a missing span is skipped, not counted as 0. Overlap (this frame starts before the
// previous one ends) is idle 0, not negative.
typedef struct { int has; uint64_t frame, end_ns; } gpuidle_tracker;
static inline int64_t gpuidle_gap_ns(gpuidle_tracker *t, uint64_t f, int span_known, uint64_t start_ns, uint64_t end_ns) {
    int64_t r = -1;
    if (span_known && t->has && t->frame == f - 1) r = start_ns > t->end_ns ? (int64_t)(start_ns - t->end_ns) : 0;
    if (span_known) { t->has = 1; t->frame = f; t->end_ns = end_ns; }
    return r;
}

// Idle inside a frame: the span from its first tagged start to its last tagged end minus the sum of the command buffers' GPU times. Command buffers may overlap,
// then the sum exceeds the span and the idle is 0.
static inline uint64_t gpuidle_inside_ns(uint64_t span_ns, uint64_t sum_ns) { return span_ns > sum_ns ? span_ns - sum_ns : 0; }

// GPU busy share over a window of frames with known spans: the sum of their GPU times divided by the window from the earliest start to the latest end, as a percentage;
// -1 when the window is empty or degenerate (no span or nothing but overlap at its edges).
typedef struct { uint64_t sum_ns, t0_ns, t1_ns; unsigned n; } gpuidle_window;
static inline void gpuidle_window_add(gpuidle_window *w, uint64_t sum_ns, uint64_t start_ns, uint64_t end_ns) {
    if (!w->n || start_ns < w->t0_ns) w->t0_ns = start_ns;
    if (!w->n || end_ns > w->t1_ns) w->t1_ns = end_ns;
    w->sum_ns += sum_ns; w->n++;
}
static inline double gpuidle_window_busy(const gpuidle_window *w) {
    return w->n && w->t1_ns > w->t0_ns ? 100.0 * (double)w->sum_ns / (double)(w->t1_ns - w->t0_ns) : -1.0;
}
