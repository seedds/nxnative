// Frame lag cap (DM_NVN_FRAME_LAG=<k>; pure ring, unit test tests/t_lag.c). Each present records the fence of the work submitted before it. When a frame starts (the first
// AcquireTexture after a present), the host waits until the GPU has completed the work presented k presents before the newest one: k = 0 means the previous frame's GPU work
// is done before the next frame's CPU work begins (no cross-frame CPU / GPU overlap), k = 1 allows one frame in flight, and so on.
#pragma once
#include <stdint.h>

enum { NVN_LAG_RING = 16 };
typedef struct { uint64_t fence[NVN_LAG_RING]; uint64_t n; } nvn_lag;   // n: presents recorded

static inline void nvn_lag_present(nvn_lag *l, uint64_t fence) { l->fence[l->n++ % NVN_LAG_RING] = fence; }
// Early release (DM_NVN_FRAME_LAG_EARLY=<n>): the frame starts once the GPU has completed all but the last n submissions before the fence (fences count submissions, one
// apart), so the game records its next frame while the GPU runs the previous frame's tail. Ocarina of Time at lag 0: the GPU ran out about 5 times a frame (1.5 ms of a 17.4 ms
// span) waiting for the game's next submission, its recording started only after the whole previous frame had finished. Never earlier than the frame before's own fence (prev).
static inline uint64_t nvn_lag_early(uint64_t fence, int n, uint64_t prev) {
    if (n <= 0 || !fence) return fence;
    uint64_t e = fence > (uint64_t)n ? fence - (uint64_t)n : 0;
    return e > prev ? e : (prev < fence ? prev : fence);
}
// The fence a frame start waits for with lag k (0 = none: lag off (k < 0), fewer than k + 1 presents yet, or k beyond the ring).
static inline uint64_t nvn_lag_fence(const nvn_lag *l, int k) {
    if (k < 0 || k >= NVN_LAG_RING || l->n < (uint64_t)k + 1) return 0;
    return l->fence[(l->n - 1 - (uint64_t)k) % NVN_LAG_RING];
}
