// Source timestamps are nanoseconds. Repeated metadata reads are not new pictures.
#pragma once
#include <stdint.h>
#include <stdatomic.h>
typedef struct { int64_t pts; uint64_t seen; unsigned samples; int have; } codec_cadence;
static inline void codec_cadence_note(codec_cadence *s, int64_t pts, uint64_t now) {
    if (s->have && pts == s->pts) return;
    if (s->have) {
        int64_t delta = s->pts >= 0 && pts > s->pts ? pts - s->pts : 0;
        if (delta >= 16000000 && delta <= 17500000) {
            if (s->samples < 3) s->samples++;
        } else s->samples = 0;
    }
    s->have = 1; s->pts = pts; s->seen = now;
}
static inline int codec_cadence_60(const codec_cadence *s, uint64_t now) {
    return s->samples >= 3 && now >= s->seen && now - s->seen < 500000000;
}

// A fixed registry slot outlives each decoder object. Publish consumed-picture time,
// never a metadata poll's time; the frame-step reader needs no decoder/registry lock.
static inline void codec_cadence_publish(_Atomic uint64_t *slot, const codec_cadence *s, int eligible) {
    atomic_store_explicit(slot, eligible && s->samples >= 3 ? s->seen : 0, memory_order_release);
}
static inline int codec_cadence_published_60(const _Atomic uint64_t *slot, uint64_t now) {
    uint64_t seen = atomic_load_explicit(slot, memory_order_acquire);
    return seen && now >= seen && now - seen < 500000000;
}
