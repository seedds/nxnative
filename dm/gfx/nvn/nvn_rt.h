// The computation a thread that carries a frame declares in its time-constraint (real-time) policy (hle_nvn.c rt_tick): the CPU time it needs per presented frame.
// The scheduler and the performance controller take the declaration as the thread's need: under it the thread is held up each frame (DARK SOULS: REMASTERED's render
// thread, declared at 2.5 ms where one or two frames in 120 needed 4.4, stalled 30-60 ms about once a second), and the clock is set so that it fits the period.
// So the declaration covers the window's peak as well as its 90th percentile: the larger of 1.25 x the p90 and 1.1 x the peak of the last RT_WIN frames, between 1 ms and
// 0.9 x the period; it is declared again only when it moves by more than 15%. Pure logic, no system calls (dm/gfx/tests/t_thread_rt.c).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// hle_nvn.c: a host thread that carries a frame's CPU work registers itself, on the thread (NULL when the table is full), then calls nvn_rt_tick before each piece of its
// work: one sample a presented frame, and the frame's deadline declared with the thread's own computation (DM_RT=0 / DM_RT_FRAME=0 leave it on its QoS)
void *nvn_rt_join(const char *name);
void nvn_rt_tick(void *h);

#define RT_WIN 120
typedef struct {
    uint64_t frame, cpu;   // the frame count and the thread's CPU nanoseconds at its last sample (frame 0: none yet)
    double win[RT_WIN]; int n;   // CPU milliseconds per frame of the current window
    double comp_ms;   // the computation declared (0: none yet)
} nvn_rt;

static inline int nvn_rt_dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
// The computation the window asks for, in milliseconds
static inline double nvn_rt_need(const double *win, int n, double period_ms) {
    double v[RT_WIN]; memcpy(v, win, (size_t)n * sizeof *v); qsort(v, (size_t)n, sizeof *v, nvn_rt_dcmp);
    double p90 = v[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1], c = p90 * 1.25 > v[n - 1] * 1.1 ? p90 * 1.25 : v[n - 1] * 1.1, cap = period_ms * 0.9;
    if (c > cap) c = cap;
    return c < 1 ? 1 : c;
}
// One sample on the thread itself: `frame` is the count of presented frames, `cpu_ns` the thread's CPU time. The CPU since the last sample counts for the frames presented since,
// as one value (their mean): a thread idle through a loading screen adds one small sample, not a window of them. Returns the computation to declare now (ms), 0 for no change
static inline double nvn_rt_sample(nvn_rt *r, uint64_t frame, uint64_t cpu_ns, double period_ms) {
    if (!r->frame || frame < r->frame) { r->frame = frame; r->cpu = cpu_ns; return 0; }
    if (frame == r->frame) return 0;
    r->win[r->n++] = (double)(cpu_ns - r->cpu) / 1e6 / (double)(frame - r->frame); r->frame = frame; r->cpu = cpu_ns;
    if (r->n < RT_WIN) return 0;
    r->n = 0;
    double c = nvn_rt_need(r->win, RT_WIN, period_ms);
    return !r->comp_ms || c > r->comp_ms * 1.15 || c < r->comp_ms * 0.85 ? c : 0;
}
