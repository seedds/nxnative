// Source timestamps are nanoseconds. Repeated metadata reads are not new pictures.
#pragma once
#include <stdint.h>
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
