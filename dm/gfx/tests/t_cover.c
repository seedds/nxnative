// Unit tests for the geometric proof of a full-coverage first draw (nvn_cover.h): expected values are hand-calculated from geometry, without running the implementation under test.
// Positive cases: fullscreen big triangle (-1,-1)(3,-1)(-1,3), fullscreen quad strip; negative cases: viewport covers only part of the attachment, scissor covers only part, triangle missing a corner,
// back-face culled, z out of range, w ≠ 1, two triangles each covering half (no single triangle covers all four corners, so by the rule it does not hold - conservative).
#include "../nvn/nvn_cover.h"
#include <stdio.h>

static int fail, total;
static void t(const char *name, int want, int got) {
    total++; fail += want != got;
    printf("%-44s expected %s → got %s %s\n", name, want ? "covers" : "does not cover", got ? "covers" : "does not cover", want == got ? "✓" : "✗");
}
int main(void) {
    const float big[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};        // counterclockwise (NDC y up)
    const float big_cw[3][4] = {{-1, -1, 0, 1}, {-1, 3, 0, 1}, {3, -1, 0, 1}};     // same triangle, clockwise
    const float quad[4][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}};   // strip: two triangles each covering half the screen
    const float small[3][4] = {{-1, -1, 0, 1}, {1.9f, -1, 0, 1}, {-1, 1.9f, 0, 1}}; // hypotenuse x+y=0.9 cuts off the (1,1) corner
    const float zout[3][4] = {{-1, -1, 1.5f, 1}, {3, -1, 1.5f, 1}, {-1, 3, 1.5f, 1}};
    const float w2[3][4] = {{-2, -2, 0, 2}, {6, -2, 0, 2}, {-2, 6, 0, 2}};
    const float vp_full[4] = {0, 0, 1600, 900}, vp_half[4] = {0, 0, 800, 900}, vp_off[4] = {0, 450, 1600, 900};
    const int sc_full[4] = {0, 0, 1600, 900}, sc_part[4] = {0, 0, 1600, 450};
    t("full-screen triangle, viewport = whole attachment, no culling", 1, nvn_cover(big, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("full-screen triangle, no viewport set (defaults to whole attachment)", 1, nvn_cover(big, 3, 4, 0, 1, 0, NULL, 0, NULL, 1600, 900));
    t("full-screen triangle, back-face culling, counter-clockwise front", 1, nvn_cover(big, 3, 4, 2, 1, 1, vp_full, 1, sc_full, 1600, 900));
    t("neg: same triangle clockwise, back-face culling (culled)", 0, nvn_cover(big_cw, 3, 4, 2, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: viewport covers only the left half of the attachment", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_half, 0, NULL, 1600, 900));
    t("neg: viewport shifted down (does not start at 0)", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_off, 0, NULL, 1600, 900));
    t("neg: viewport 1600x900, attachment is 1920x1080", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1920, 1080));
    t("neg: scissor covers only the top half", 0, nvn_cover(big, 3, 4, 0, 1, 1, vp_full, 1, sc_part, 1600, 900));
    t("neg: triangle misses the (1,1) corner", 0, nvn_cover(small, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: z outside [0,1]", 0, nvn_cover(zout, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: w ≠ 1", 0, nvn_cover(w2, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: full-screen quad strip (no single triangle covers it; conservative)", 0, nvn_cover(quad, 4, 5, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: both faces culled", 0, nvn_cover(big, 3, 4, 3, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: primitive is lines (not triangles)", 0, nvn_cover(big, 3, 1, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    // Two-triangle quad (nvn_cover_quad): P, A, B | B, A, Q with the diagonal A-B shared
    const float q_full[6][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}};   // not a quad on purpose: see below
    const float qd[6][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, 1, 0.5f, 1}};     // counterclockwise, diagonal (-1,-1)-(1,1)
    const float qd_cw[6][4] = {{-1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {1, -1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {-1, 1, 0.5f, 1}, {1, 1, 0.5f, 1}};  // the same quad clockwise
    const float qd_big[6][4] = {{-1.5f, -1.5f, 0, 1}, {2, -1.5f, 0, 1}, {2, 2, 0, 1}, {-1.5f, -1.5f, 0, 1}, {2, 2, 0, 1}, {-1.5f, 2, 0, 1}};
    const float qd_small[6][4] = {{-1, -1, 0.5f, 1}, {0.9f, -1, 0.5f, 1}, {0.9f, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {0.9f, 1, 0.5f, 1}, {-1, 1, 0.5f, 1}};   // misses the right strip
    const float qd_gap[6][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1.001f, 1, 0.5f, 1}, {-1, 1, 0.5f, 1}};         // the diagonal's end is not shared exactly
    const float qd_fold[6][4] = {{-1, -1, 0.5f, 1}, {1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {1, 1, 0.5f, 1}, {1, -1, 0.5f, 1}};              // second triangle folds back onto the first
    const float qd_z[6][4] = {{-1, -1, 1.5f, 1}, {1, -1, 1.5f, 1}, {1, 1, 1.5f, 1}, {-1, -1, 1.5f, 1}, {1, 1, 1.5f, 1}, {-1, 1, 1.5f, 1}};
    const float qd_w[6][4] = {{-2, -2, 0.5f, 2}, {2, -2, 0.5f, 2}, {2, 2, 0.5f, 2}, {-2, -2, 0.5f, 2}, {2, 2, 0.5f, 2}, {-2, 2, 0.5f, 2}};
    const float qd_dart[6][4] = {{-1, -1, 0.5f, 1}, {3, 0, 0.5f, 1}, {0, 0, 0.5f, 1}, {-1, -1, 0.5f, 1}, {0, 0, 0.5f, 1}, {0, 3, 0.5f, 1}};   // a reflex ring around the diagonal: not convex
    t("quad of two triangles, viewport = attachment", 1, nvn_cover_quad(qd, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("quad of two triangles, no viewport, full scissor, back-face culling", 1, nvn_cover_quad(qd, 6, 2, 1, 0, NULL, 1, sc_full, 1600, 900));
    t("quad larger than the screen", 1, nvn_cover_quad(qd_big, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("quad clockwise, clockwise is front", 1, nvn_cover_quad(qd_cw, 6, 2, 0, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: quad clockwise, counterclockwise is front, back-face culling", 0, nvn_cover_quad(qd_cw, 6, 2, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: the same two triangles, nvn_cover refuses (neither holds all corners)", 0, nvn_cover(qd, 3, 4, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: quad misses the right strip", 0, nvn_cover_quad(qd_small, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: diagonal end not shared exactly (a possible gap)", 0, nvn_cover_quad(qd_gap, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: second triangle folds back over the first", 0, nvn_cover_quad(qd_fold, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: reflex ring (not convex)", 0, nvn_cover_quad(qd_dart, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: z outside [0,1]", 0, nvn_cover_quad(qd_z, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: w != 1", 0, nvn_cover_quad(qd_w, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: viewport covers half", 0, nvn_cover_quad(qd, 6, 0, 1, 1, vp_half, 0, NULL, 1600, 900));
    t("neg: scissor covers the top half", 0, nvn_cover_quad(qd, 6, 0, 1, 1, vp_full, 1, sc_part, 1600, 900));
    t("neg: both faces culled", 0, nvn_cover_quad(qd, 6, 3, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: five vertices", 0, nvn_cover_quad(qd, 5, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    t("neg: two triangles sharing only a corner", 0, nvn_cover_quad(q_full, 6, 0, 1, 1, vp_full, 0, NULL, 1600, 900));
    printf("== %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
