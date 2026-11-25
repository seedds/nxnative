// The full-screen backgrounds on a taller screen (DM_SCREEN_ASPECT, Taiko no Tatsujin: Rhythm Festival 7.0.0). Almost every 2D scene's background, fade and mask is a sprite of
// one 16:9 screen (1920x1080 world units, centred on its transform, under a camera of size 540), so with the cameras widened to the screen's shape (patches.py) the extra rows
// above and below show what lies behind it. Unity's sprite extraction fills each SpriteRenderer's render node from its TransformInfo: the world matrix at +0 (column-major: x
// axis at +0, y axis at +0x10, translation at +0x30), the world bounds at +0x80 and the local bounds at +0x98 (each centre xyz, extents xyz). Right after, for a sprite whose
// world size is one 16:9 screen (at least 1900 wide, 1060..1190 high, not rotated, vertically centred on its transform), bg_cover scales the node's x and y axes by k, the
// screen's height / 1080, and its world bounds with them: it covers the screen about its own centre and loses a little of its sides, nothing stretched. The transform is not
// changed, so scripts and children see the original. A sprite already taller (one a rule scaled) is left. st: the state page (+0x48 the output's height, 0 = 16:9).
// Compiled to position-independent code with no data, as top_lift.c
__attribute__((always_inline)) static float absf(float v) { return v < 0 ? -v : v; }

void bg_cover(char *node, const char *st) {
    int h = *(const int *)(st + 0x48);
    if (h <= 1080) return;
    float *m = (float *)node, *a = (float *)(node + 0x80);
    const float *b = (const float *)(node + 0x98);
    if (absf(m[1]) > 1e-3f || absf(m[4]) > 1e-3f) return;   // rotated
    float w = 2 * b[3] * absf(m[0]), hh = 2 * b[4] * absf(m[5]), cy = b[1] * m[5];
    if (w < 1900 || hh < 1060 || hh > 1190 || absf(cy) > 4) return;
    float k = h / 1080.0f + 0.001f;   // a hair more, so no edge row shows
    m[0] *= k; m[1] *= k; m[2] *= k;
    m[4] *= k; m[5] *= k; m[6] *= k;
    a[0] = m[12] + k * (a[0] - m[12]); a[1] = m[13] + k * (a[1] - m[13]);
    a[3] *= k; a[4] *= k;
}
