// GLSLC's sample position table (c0 + 0x620 in the pixel stage, filled by nvn_mtl.m's c0_sample_rows; unit test tests/t_sample.c): one byte per sample, x in the low nibble and
// y in the high one, in 1/16 pixel. Switch 1 GLSLC reads a nibble as the position from the pixel's corner, 0x88 for one sample at the centre (Metroid Prime Remastered).
// NVN2's GLSLC (SDK `Ounce`) reads it as a signed offset from the centre: gl_SamplePosition is ((byte ^ 0x88) & 15) / 16 - 0.5 per nibble (213 of PRAGMATA's pixel shaders,
// 9c045635fda5ae99 among them), and IPA.OFFSET takes byte << 8 as is, a signed 12-bit offset in 1/4096 pixel (PRAGMATA's 4a9d4507eea6c837). The corner's table put every sample of an
// NVN2 game in the pixel's corner: one sample at (-0.5, -0.5) from the centre.
#pragma once
#include <stdint.h>

// The byte for a sample at (x, y), each 0..1 from the pixel's corner; centred: NVN2's signed offset from the centre
static inline uint8_t nvn_sample_byte(float x, float y, int centred) {
    int nx = (int)(x * 16) & 15, ny = (int)(y * 16) & 15;
    if (centred) { nx = (nx - 8) & 15; ny = (ny - 8) & 15; }
    return (uint8_t)(nx | ny << 4);
}
