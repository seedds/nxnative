// GPU keep-busy (DM_NVN_GPU_BUSY, off by default; nvn_gpu_busy.m): the pure parts, shared with the unit test tests/t_gpu_busy.c.
// A game that paces itself at present interval 2 or more leaves the GPU idle for much of each frame, and macOS lowers the GPU clock for that load; a dynamic resolution
// that budgets a fixed 60 fps span (Nintendo Switch Sports Resort) then reads the same work as slower and stays at its floor (docs/reference/frame-rate.md, "A dynamic
// resolution held at its floor by the GPU clock macOS picks at 30 fps"). While the game's interval is 2 or more, the host keeps the GPU busy for a share of wall time
// (duty) with short dispatches of a few threadgroups on a queue of its own, so the clock controller sees a busy GPU and the game's frame keeps most of the cores.
#pragma once
#include <stdint.h>
#include <stdlib.h>

#define GPU_BUSY_DUTY 100          // DM_NVN_GPU_BUSY=1: percent of wall time asked for; one dispatch at a time reaches about 75%. On an M2 Max at the floor resolution of a
                                   // 30 fps scene (the game's own GPU work 59% of the frame) 34% held the clock up and 19% did not
#define GPU_BUSY_GROUPS 1          // threadgroups of 32 threads a dispatch, by default
#define GPU_BUSY_GROUPS_MAX 64
#define GPU_BUSY_CHUNK_MS 1.0      // the GPU time one dispatch aims for: a frame's work that lands behind one waits at most about this long
#define GPU_BUSY_ITV_MIN 2         // the game's present interval from which the work runs
#define GPU_BUSY_ITERS0 4096u      // multiply-adds a thread runs in the first dispatch, before any measurement
#define GPU_BUSY_ITERS_MIN 64u
#define GPU_BUSY_ITERS_MAX (1u << 26)

typedef struct { int duty, groups; } nvn_gpu_busy_cfg;   // duty 0: off

// The switch's value: unset, empty or 0 is off; 1 is on with the defaults; <duty> (2-100) sets the percent; <duty>:<groups> also the threadgroups a dispatch.
// Returns 0 for a value it cannot read (the switch then stays off)
static inline int nvn_gpu_busy_parse(const char *s, nvn_gpu_busy_cfg *c) {
    c->duty = 0; c->groups = GPU_BUSY_GROUPS;
    if (!s || !*s) return 1;
    char *e; long d = strtol(s, &e, 10), g = GPU_BUSY_GROUPS;
    if (e == s || d < 0 || d > 100) return 0;
    if (*e == ':') { const char *gs = e + 1; g = strtol(gs, &e, 10); if (e == gs || g < 1 || g > GPU_BUSY_GROUPS_MAX) return 0; }
    if (*e) return 0;
    c->duty = d == 1 ? GPU_BUSY_DUTY : (int)d; c->groups = (int)g;
    return 1;
}
// Whether the work runs now: the switch is on and the game paces itself at interval 2 or more (0 = no reading yet)
static inline int nvn_gpu_busy_active(const nvn_gpu_busy_cfg *c, int game_itv) { return c->duty > 0 && game_itv >= GPU_BUSY_ITV_MIN; }
// The next dispatch's length from the last one's: scaled to the target GPU time, by at most half or twice a step (the clock moves under it), within the bounds
static inline uint32_t nvn_gpu_busy_iters(uint32_t iters, double gpu_ms, double target_ms) {
    double f = gpu_ms > 0 ? target_ms / gpu_ms : 2.0;
    if (f > 2.0) f = 2.0;
    if (f < 0.5) f = 0.5;
    double n = iters * f;
    return n < GPU_BUSY_ITERS_MIN ? GPU_BUSY_ITERS_MIN : n > GPU_BUSY_ITERS_MAX ? GPU_BUSY_ITERS_MAX : (uint32_t)n;
}
// The idle time after a dispatch that keeps the duty: the dispatch took gpu_ms on the GPU and wall_ms from its commit to the thread's wake-up after it, so a share of
// duty percent needs gpu_ms × 100 / duty of wall time in all. None when the round trip alone is longer (one dispatch in flight reaches about 75% on an M2 Max)
static inline double nvn_gpu_busy_gap_ms(double gpu_ms, double wall_ms, int duty) {
    if (duty <= 0 || gpu_ms <= 0) return 0;
    double g = gpu_ms * 100.0 / duty - wall_ms;
    return g > 0 ? g : 0;
}
