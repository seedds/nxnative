// DM_AUDIO_LIMITER (on unless =0): a peak limiter on the renderer's stereo downmix, before the 16-bit clamp. A game that mixes six channels at full level can
// sum above full scale in the 6 -> 2 downmix (front 1.0, center 0.707, LFE 0.251, back 0.707), and the clamp then cuts the peaks flat: clicks in
// loud fights (Metroid Prime 4). On by default (the user's choice); =0 plays the clamped mix as before.
// Per block (one renderer frame, 5 ms, known whole before output): when the block's peak times the current gain would pass the threshold, the gain
// drops at once to fit it, for the whole block; otherwise it returns towards 1 with a ramp across the block (about 150 ms to recover from a 6 dB cut).
// Blocks under the threshold at gain 1 are left exactly as they are.
#pragma once
#include <stdint.h>

enum { LIMITER_THRESHOLD = 32000 };   // about -0.2 dBFS
#define LIMITER_RELEASE 0.033f        // per 5 ms block: the share of the distance to gain 1 recovered

// st: interleaved samples (int32, 16-bit range before clamping), n values; g: the gain carried from the last block. Returns the gain to carry on.
static inline float limiter_block(int32_t *st, uint32_t n, float g) {
    int32_t peak = 0;
    for (uint32_t i = 0; i < n; i++) { int32_t v = st[i] < 0 ? -st[i] : st[i]; if (v > peak) peak = v; }
    float need = peak > LIMITER_THRESHOLD ? (float)LIMITER_THRESHOLD / (float)peak : 1.0f;
    float g0 = g, g1;
    if (need < g) g0 = g1 = need;                         // attack: the whole block at the gain that fits its peak
    else { g1 = g + (1.0f - g) * LIMITER_RELEASE; if (g1 > need) g1 = need; }   // release, never above what this block allows
    if (g0 >= 1.0f && g1 >= 1.0f) return 1.0f;           // untouched
    uint32_t frames = n / 2;
    for (uint32_t f = 0; f < frames; f++) {
        float k = frames > 1 ? g0 + (g1 - g0) * (float)f / (float)(frames - 1) : g1;
        for (int c = 0; c < 2; c++) { float v = (float)st[2 * f + c] * k; st[2 * f + c] = (int32_t)(v < 0 ? v - 0.5f : v + 0.5f); }
    }
    return g1;
}
