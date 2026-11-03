// Geometric proof of a full-coverage first draw (pure function, unit test tests/t_cover.c): whether this draw touches every pixel of the whole block of that attachment level.
// Conditions (all must hold): the scissor covers the whole block; the viewport is exactly (0, 0, W, H) (NDC [-1,1]^2 is the whole block); some non-culled triangle encloses the four corners in NDC (closed containment:
// pixel centers are all inside the square, so the four corners being in the closed triangle means the whole block is inside); every vertex has w = 1 and z in [0,1] (unchanged by perspective divide and depth clipping).
// Winding: in NDC (y up) counter-clockwise is front; odd-numbered triangles of a strip have reversed vertex order. Anything unrecognized does not hold.
#pragma once
#include <math.h>

enum { NVN_COVER_TRIANGLES = 4, NVN_COVER_TRIANGLE_STRIP = 5 };   // NVNdrawPrimitive
enum { NVN_COVER_CULL_NONE = 0, NVN_COVER_CULL_FRONT = 1, NVN_COVER_CULL_BACK = 2, NVN_COVER_CULL_BOTH = 3 };   // NVNface

static inline int nvn_cover(const float (*v)[4], int n, int prim, int cull, int front_ccw,
                            int has_vp, const float vp[4], int has_sc, const int sc[4], long W, long H) {
    if (W <= 0 || H <= 0 || n < 3) return 0;
    if (has_sc && !(sc[0] <= 0 && sc[1] <= 0 && (long)sc[0] + sc[2] >= W && (long)sc[1] + sc[3] >= H)) return 0;
    if (has_vp && !(vp[0] == 0 && vp[1] == 0 && vp[2] == (float)W && vp[3] == (float)H)) return 0;
    if (prim != NVN_COVER_TRIANGLES && prim != NVN_COVER_TRIANGLE_STRIP) return 0;
    if (cull == NVN_COVER_CULL_BOTH) return 0;
    for (int k = 0; k < n; k++)
        if (v[k][3] != 1.0f || !(v[k][2] >= 0.0f && v[k][2] <= 1.0f) || !isfinite(v[k][0]) || !isfinite(v[k][1])) return 0;
    static const double C[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    int ntri = prim == NVN_COVER_TRIANGLES ? n / 3 : n - 2;
    for (int k = 0; k < ntri; k++) {
        int i0 = prim == NVN_COVER_TRIANGLES ? 3 * k : k, i1 = i0 + 1, i2 = i0 + 2;
        if (prim == NVN_COVER_TRIANGLE_STRIP && (k & 1)) { int x = i1; i1 = i2; i2 = x; }
        double A[2] = {v[i0][0], v[i0][1]}, B[2] = {v[i1][0], v[i1][1]}, D[2] = {v[i2][0], v[i2][1]};
        double d = (B[0] - A[0]) * (D[1] - A[1]) - (B[1] - A[1]) * (D[0] - A[0]);
        if (d == 0) continue;
        int front = front_ccw ? d > 0 : d < 0;
        if ((cull == NVN_COVER_CULL_BACK && !front) || (cull == NVN_COVER_CULL_FRONT && front)) continue;
        int in = 1;
        for (int c = 0; c < 4 && in; c++) {
            const double *p = C[c];
            double e0 = (B[0] - A[0]) * (p[1] - A[1]) - (B[1] - A[1]) * (p[0] - A[0]);
            double e1 = (D[0] - B[0]) * (p[1] - B[1]) - (D[1] - B[1]) * (p[0] - B[0]);
            double e2 = (A[0] - D[0]) * (p[1] - D[1]) - (A[1] - D[1]) * (p[0] - D[0]);
            in = d > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0);
        }
        if (in) return 1;
    }
    return 0;
}

// Window copy (DM_NVN_WINDOW_COPY): a full-screen quad drawn as two triangles, which nvn_cover refuses because no single triangle holds the four corners. Six vertices (TRIANGLES), clip coordinates
// as nvn_cover takes them. Conditions: the same scissor / viewport / w / z conditions; neither triangle is degenerate or culled and both wind alike; they share one edge exactly (the same two x, y pairs, so the
// rasterizer's rule leaves no gap along it) and the other two vertices lie on opposite sides of it; the four vertices in order P, A, Q, B form a convex ring. A convex quad that holds the four NDC corners
// holds the square between them, and the square holds every pixel center of the block. Anything else does not hold.
static inline int nvn_cover_quad(const float (*v)[4], int n, int cull, int front_ccw, int has_vp, const float vp[4], int has_sc, const int sc[4], long W, long H) {
    if (W <= 0 || H <= 0 || n != 6 || cull == NVN_COVER_CULL_BOTH) return 0;
    if (has_sc && !(sc[0] <= 0 && sc[1] <= 0 && (long)sc[0] + sc[2] >= W && (long)sc[1] + sc[3] >= H)) return 0;
    if (has_vp && !(vp[0] == 0 && vp[1] == 0 && vp[2] == (float)W && vp[3] == (float)H)) return 0;
    for (int k = 0; k < n; k++)
        if (v[k][3] != 1.0f || !(v[k][2] >= 0.0f && v[k][2] <= 1.0f) || !isfinite(v[k][0]) || !isfinite(v[k][1])) return 0;
    #define NQ_X(i) ((double)v[i][0])
    #define NQ_Y(i) ((double)v[i][1])
    #define NQ_CROSS(a, b, c) ((NQ_X(b) - NQ_X(a)) * (NQ_Y(c) - NQ_Y(a)) - (NQ_Y(b) - NQ_Y(a)) * (NQ_X(c) - NQ_X(a)))
    #define NQ_SAME(a, b) (v[a][0] == v[b][0] && v[a][1] == v[b][1])
    double d0 = NQ_CROSS(0, 1, 2), d1 = NQ_CROSS(3, 4, 5);
    if (d0 == 0 || d1 == 0 || (d0 > 0) != (d1 > 0)) return 0;
    int front = front_ccw ? d0 > 0 : d0 < 0;
    if ((cull == NVN_COVER_CULL_BACK && !front) || (cull == NVN_COVER_CULL_FRONT && front)) return 0;
    // The shared edge: exactly two vertices of the first triangle equal to vertices of the second; the first triangle is rotated to (P, A, B) with A, B the shared ones
    int sh0[3] = {-1, -1, -1}, sh1[3] = {-1, -1, -1}, ns = 0;
    for (int i = 0; i < 3; i++) for (int j = 3; j < 6; j++) if (NQ_SAME(i, j)) { sh0[i] = j; sh1[j - 3] = i; ns++; }
    if (ns != 2) return 0;
    int p = -1; for (int i = 0; i < 3; i++) if (sh0[i] < 0) p = i;
    int q = -1; for (int j = 0; j < 3; j++) if (sh1[j] < 0) q = j + 3;
    if (p < 0 || q < 0) return 0;
    int a = (p + 1) % 3, b = (p + 2) % 3;   // t0 = (P, A, B): the shared edge A→B follows t0's winding, so t1 must run it as B→A
    int qa = -1, qb = -1; for (int j = 0; j < 3; j++) { if (NQ_SAME(a, 3 + j)) qa = 3 + j; if (NQ_SAME(b, 3 + j)) qb = 3 + j; }
    if (qa < 0 || qb < 0 || (qb - 3 + 1) % 3 != qa - 3) return 0;   // in t1, B is followed by A
    double sp = NQ_CROSS(a, b, p), sq = NQ_CROSS(a, b, q);
    if (sp == 0 || sq == 0 || (sp > 0) == (sq > 0)) return 0;
    int ring[4] = {p, a, q, b}; double sgn = d0 > 0 ? 1 : -1;
    for (int i = 0; i < 4; i++) {   // convex: every turn has the winding's sign (a straight one is allowed)
        int u = ring[i], w = ring[(i + 1) % 4], x = ring[(i + 2) % 4];
        if (sgn * NQ_CROSS(u, w, x) < 0) return 0;
    }
    static const double C[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int c = 0; c < 4; c++) for (int i = 0; i < 4; i++) {
        int u = ring[i], w = ring[(i + 1) % 4];
        double e = (NQ_X(w) - NQ_X(u)) * (C[c][1] - NQ_Y(u)) - (NQ_Y(w) - NQ_Y(u)) * (C[c][0] - NQ_X(u));
        if (sgn * e < 0) return 0;
    }
    #undef NQ_X
    #undef NQ_Y
    #undef NQ_CROSS
    #undef NQ_SAME
    return 1;
}
