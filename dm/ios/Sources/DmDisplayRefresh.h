// Display-request policy and timestamp measurements only. No game clock,
// frame generation, drawable presentation or NVN pacing is controlled here.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

static inline int dm_display_hz_parse(const char *value) {
    if (!value || !*value) return 0;   // retain the system's existing policy
    if (!strcmp(value, "60")) return 60;
    if (!strcmp(value, "120")) return 120;
    return -1;
}
static inline int dm_display_hz_select(int requested, int supported_max) {
    if (requested != 60 && requested != 120) return 0;
    if (supported_max <= 0) return 60;
    return supported_max < requested ? supported_max : requested;
}

typedef struct {
    double start, last, interval_min, interval_max, target_sum;
    uint64_t intervals, target_intervals, presented_start;
} dm_display_probe;
typedef struct {
    double seconds, callback_hz, target_period_ms, min_period_ms, max_period_ms, presented_fps;
    uint64_t callback_intervals, presented_frames;
} dm_display_probe_result;
static inline void dm_display_probe_reset(dm_display_probe *probe) {
    *probe = (dm_display_probe){0};
}
static inline void dm_display_probe_seed(dm_display_probe *probe, double timestamp, uint64_t presented) {
    *probe = (dm_display_probe){.start=timestamp, .last=timestamp, .presented_start=presented};
}
static inline int dm_display_probe_tick(dm_display_probe *probe, double timestamp,
        double target_timestamp, uint64_t presented, dm_display_probe_result *result) {
    if (!isfinite(timestamp) || timestamp <= 0) return 0;
    double delta = timestamp - probe->last;
    // A pause, clock discontinuity or counter reset starts a new window. Long
    // background time must not lower the reported foreground callback cadence.
    if (!probe->last || delta <= 0 || delta > 0.25 || presented < probe->presented_start) {
        dm_display_probe_seed(probe, timestamp, presented); return 0;
    }
    if (!probe->intervals || delta < probe->interval_min) probe->interval_min = delta;
    if (delta > probe->interval_max) probe->interval_max = delta;
    probe->intervals++; probe->last = timestamp;
    double target = target_timestamp - timestamp;
    if (isfinite(target) && target > 0 && target <= 0.25) {
        probe->target_sum += target; probe->target_intervals++;
    }
    double elapsed = timestamp - probe->start;
    if (elapsed < 2.0) return 0;
    *result = (dm_display_probe_result){
        .seconds=elapsed, .callback_hz=probe->intervals / elapsed,
        .target_period_ms=probe->target_intervals ? 1000 * probe->target_sum / probe->target_intervals : 0,
        .min_period_ms=1000 * probe->interval_min, .max_period_ms=1000 * probe->interval_max,
        .presented_fps=(presented - probe->presented_start) / elapsed,
        .callback_intervals=probe->intervals, .presented_frames=presented - probe->presented_start};
    dm_display_probe_seed(probe, timestamp, presented);
    return 1;
}
