// Main-queue audio-session retry policy, independent of UIKit/AVFoundation so
// lifecycle cancellation and rate-limited retry behavior can be tested without I/O.
#pragma once
#include <stdint.h>
#include <string.h>
enum { DM_AUDIO_SESSION_SOLO, DM_AUDIO_SESSION_PLAYBACK_MIX };
// Unset/empty remains the original SoloAmbient default. A malformed diagnostic
// value must not silently change session policy.
static inline int dm_audio_session_category_parse(const char *value) {
    if (!value || !*value || !strcmp(value, "solo")) return DM_AUDIO_SESSION_SOLO;
    if (!strcmp(value, "playback_mix")) return DM_AUDIO_SESSION_PLAYBACK_MIX;
    return -1;
}
typedef struct { uint64_t generation; unsigned attempts; int pending; } dm_audio_retry;
static inline void dm_audio_retry_reset(dm_audio_retry *r) {
    r->generation++; r->attempts = 0; r->pending = 0;
}
static inline unsigned dm_audio_retry_plan(dm_audio_retry *r, int blocked, int off, uint64_t *token) {
    if (blocked || !off || r->pending) return 0;
    unsigned milliseconds = 1000u << (r->attempts < 3 ? r->attempts : 3);
    if (r->attempts != UINT32_MAX) r->attempts++;
    r->pending = 1; *token = r->generation; return milliseconds;
}
static inline int dm_audio_retry_fire(dm_audio_retry *r, uint64_t token, int blocked, int off) {
    if (token != r->generation || !r->pending) return 0;
    r->pending = 0; return !blocked && off;
}
