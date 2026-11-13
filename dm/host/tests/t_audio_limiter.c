// DM_AUDIO_LIMITER (audio_limiter.h): peaks fit under the clamp, quiet audio is untouched, the gain recovers smoothly
#include <stdio.h>
#include <stdlib.h>
#include "../audio_limiter.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int32_t peak_of(const int32_t *s, int n) { int32_t p = 0; for (int i = 0; i < n; i++) { int32_t v = abs(s[i]); if (v > p) p = v; } return p; }

int main(void) {
    enum { N = 480 };   // 240 stereo frames: one 5 ms renderer block
    int32_t s[N], ref[N];
    for (int i = 0; i < N; i++) s[i] = ref[i] = (i % 7 - 3) * 3000;   // quiet: peak 9000
    float g = limiter_block(s, N, 1.0f);
    int same = 1; for (int i = 0; i < N; i++) same &= s[i] == ref[i];
    CHECK(g == 1.0f && same, "a block under the threshold at gain 1 is left bit for bit");

    for (int i = 0; i < N; i++) s[i] = i == 100 ? 60000 : i == 101 ? -58000 : 20000 * ((i & 1) ? 1 : -1);   // a 6-channel sum past full scale
    g = limiter_block(s, N, 1.0f);
    CHECK(peak_of(s, N) <= LIMITER_THRESHOLD, "a loud block fits under the threshold after limiting");
    CHECK(g > 0.52f && g < 0.54f, "the gain carried is the one that fitted the peak");

    float prev = g; int smooth = 1;
    for (int b = 0; b < 200; b++) {
        for (int i = 0; i < N; i++) s[i] = (i % 2 ? 1 : -1) * 10000;
        float before = prev; prev = limiter_block(s, N, prev);
        smooth &= prev >= before && prev - before < 0.05f;
        smooth &= abs(s[0]) <= 10000 && abs(s[N - 1]) <= 10000;
    }
    CHECK(smooth, "after a peak the gain only rises, in small steps, and never boosts");
    CHECK(prev > 0.99f, "the gain is back near 1 after a second of quiet audio");

    for (int i = 0; i < N; i++) s[i] = 40000;
    g = limiter_block(s, N, 0.9f);
    CHECK(peak_of(s, N) <= LIMITER_THRESHOLD, "a peak during the release is still caught");
    printf("audio limiter: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
