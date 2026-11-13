#include "../codec_cadence.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    codec_cadence s = {0};
    for (int i = 0; i < 4; i++) codec_cadence_note(&s, (int64_t)i * 16666667, (uint64_t)i * 33333333);
    assert(codec_cadence_60(&s, 100000000));
    assert(!codec_cadence_60(&s, s.seen + 500000000));
    uint64_t seen = s.seen; codec_cadence_note(&s, s.pts, seen + 400000000);
    assert(s.seen == seen); // repeated properties do not keep a stopped movie alive
    codec_cadence_note(&s, s.pts + 33333333, seen + 450000000);
    assert(!codec_cadence_60(&s, seen + 450000000));
    codec_cadence_note(&s, 0, seen + 450000001);
    assert(!s.samples);
    for (int i = 1; i < 5; i++) codec_cadence_note(&s, i * 33333333LL, seen + 450000001 + i);
    assert(!codec_cadence_60(&s, seen + 450000100));
    puts("codec cadence: 60-fps recognition, expiry, repeat reads, seek and 30-fps rejection pass");
}
