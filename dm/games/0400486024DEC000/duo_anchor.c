// The iPhone Duo / iPad / Mac taller layout space (DM_SCREEN_ASPECT, Ocarina of Time 1.1.1 v131072).
// CalculateMtx (main+0x14e9980):
//   x19 (pane): global matrix 3x4 row-major at +0x74:
//     row 0: +0x74, row 1: +0x84 (M10, M11, M12, Ty at +0x90), row 2: +0x94
//   x21 (p21): pane rect size (w, h) at +0x58 / +0x5c
//
// duo_calc runs at calc_fn + 0x1a0 (0x14e9b20) right after x19's global matrix is stored.
// Children receive q16, q5, q7, q6 on stack (not x19+0x74..0xa0), so modifying x19+0x84..0x90
// affects only the current pane and never double-shifts children.
//
// st: parameters at state + 0x80:
//   st[0]: shift in 720 units (d = st[0] * 1.5f in 1080 units)
//   st[1]: vertical stretch factor k

static inline float fab(float x) { return x < 0.0f ? -x : x; }

void duo_calc(char *pane, const float *st, const char *p21) {
    float d720 = st[0];
    if (d720 == 0.0f) return;

    float d = d720 * 3.0f;   // 720 units -> 2160 canvas units
    float *row1 = (float *)(pane + 0x84);
    float ty = row1[3];      // global Y translation at +0x90

    // Full-screen panes: width >= 1000, height >= 650, w > h * 1.4, centered (|ty| < 40)
    // Scale row 1 by k so full-screen faders/backgrounds cover the expanded canvas without gaps.
    if (p21) {
        float w = *(const float *)(p21 + 0x58);
        float h = *(const float *)(p21 + 0x5c);
        float k = st[1];
        if (w >= 1000.0f && h >= 650.0f && w > h * 1.4f && fab(ty) < 40.0f) {
            row1[0] *= k;
            row1[1] *= k;
            row1[2] *= k;
            row1[3] *= k;
            return;
        }
    }

    // Edge threshold: 600 units in 2160 canvas so centered buttons/dialogs stay untouched.
    float edge = 600.0f;
    if (fab(ty) < edge) return;

    // Shift towards top edge (+d) or bottom edge (-d)
    row1[3] += (ty > 0.0f ? d : -d);
}
