// Tessellation (nvn_tess.inc, dmsh_tess.py): the layout and the expansion kernel on the GPU, from the shipped MSL.
// test_dmsh_tess.py --fixture <dir> writes <dir>/tess_test.metal: the host's expansion kernel text (k_tess_msl, which starts with
// dmsh_tess.TESS_MSL, the domain prologue's helpers) plus two test kernels that run dm_tcount / dm_tvert / dm_tcull. Every
// triangle a domain draw would make is generated on the GPU and checked against an independent reference, in every mode
// (quad and triangle domains; equal, fractional_odd and fractional_even spacing; ccw and cw):
//  A. per patch (curated and random factors, lmax 64 and 16): entry count; no folded, overlapping or non-manifold triangles;
//     |area| = the domain's (1 or 1/2); one winding (the domain's); the boundary is the domain's, each side cut at that edge's
//     own positions for its spacing
//  B. meshes of patches sharing edge factors: 10x10 quads in all 8 orientations, 10x10 squares of two triangles each in all 6
//     orientations of each triangle: no open edge inside the mesh (a crack), no fold
//  C. dm_tess_expand: entry counts, every entry listed once, culling, the budget (whole patches dropped, nothing written past
//     it), a fixed level for every patch (DM_NVN_TESS_LEVEL)
//  D. folds for game-like factor patterns (inside = min of the edges is what The Witcher 3's hulls write)
// Usage: t_tess <tess_test.metal> [seed]   (make check_t_tess writes the fixture and runs it)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef _Float16 h16;
typedef struct { double x, y, w; } P2;   // a domain point: (u, v) and, for a triangle domain, w as the layout gives it
typedef struct { P2 a, b, c; } Tri;

static uint64_t g_rng = 20261104;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static float rndf(void) { return (float)(rnd() % 1000000) / 1000000.0f; }
#define PICK(a) ((a)[rnd() % (sizeof(a) / sizeof((a)[0]))])

static id<MTLDevice> g_dev; static id<MTLCommandQueue> g_q;
static id<MTLComputePipelineState> g_tris, g_cull, g_expand;
static int g_fail;
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
    if (g_fail++ < 30) { va_list ap; va_start(ap, fmt); printf("FAIL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); }
}

// ---- modes: TESS_MSL's md ----
enum { MD_TRI = 1, MD_CW = 8 };
static int md_sp(int md) { return md >> 1 & 3; }
static const char *md_name(int md) {
    static char b[8][64]; static int k; char *s = b[k++ & 7];
    snprintf(s, 64, "%s %s %s", md & MD_TRI ? "triangles" : "quads", (const char *[]){"equal", "fractional_odd", "fractional_even"}[md_sp(md)], md & MD_CW ? "cw" : "ccw");
    return s;
}

// ---- independent reference (OpenGL 4.6, 11.2.2: clamping, rounding, segment lengths; the placement of the short segments is ours: symmetric) ----
static double clamp_level(h16 h, float lmax, int sp) { float l = (float)h; double lo = sp == 2 ? 2 : 1, hi = sp == 1 ? lmax - 1 : lmax; return l >= lo ? fmin(l, hi) : lo; }
static int seg_n(double l, int sp) { return sp == 0 ? (int)ceil(l) : sp == 1 ? (int)ceil((l - 1) / 2) * 2 + 1 : (int)ceil(l / 2) * 2; }
static int inner_n(double l, int sp) { int n = seg_n(l, sp); return n != 1 ? n : sp == 1 ? 3 : 2; }   // an inside level of one is 1 + epsilon
static int ref_positions(double l, int sp, double *p) {   // the n + 1 points of an edge
    int n = seg_n(l, sp), k = 0; double segs[160];
    if (sp == 0 || l == (double)n) for (int i = 0; i < n; i++) segs[k++] = sp == 0 ? 1.0 / n : 1 / l;
    else if (sp == 2) { double s = (1 - (double)(n - 2) / l) / 2; for (int i = 0; i < n / 2 - 1; i++) segs[k++] = 1 / l; segs[k++] = s; segs[k++] = s; for (int i = 0; i < n / 2 - 1; i++) segs[k++] = 1 / l; }
    else if (n == 1) segs[k++] = 1;
    else { int m = (n - 1) / 2; double s = (1 - (double)(n - 2) / l) / 2; for (int i = 0; i < m - 1; i++) segs[k++] = 1 / l; segs[k++] = s; segs[k++] = 1 / l; segs[k++] = s; for (int i = 0; i < m - 1; i++) segs[k++] = 1 / l; }
    p[0] = 0; for (int i = 0; i < k; i++) p[i + 1] = p[i] + segs[i];
    p[k] = 1; return k + 1;
}
static int all_ones(const h16 *L, float lmax, int md) {
    for (int i = 0; i < 6; i++) if ((md & MD_TRI ? (i != 3 && i != 5) : 1) && seg_n(clamp_level(L[i], lmax, md_sp(md)), md_sp(md)) != 1) return 0;
    return 1;
}
// Entries: quads: inner cells + per side ceil((edge segments + inner segments) / 2); triangles: per outer side the same with ring 1's side, per inner
// ring side ceil((its segments + the next ring's) / 2), and the middle triangle of an odd inside level
static int ref_count(const h16 *L, float lmax, int md) {
    int sp = md_sp(md);
    if (all_ones(L, lmax, md)) return 1;
    if (md & MD_TRI) {
        int ni = inner_n(clamp_level(L[4], lmax, sp), sp), c = ni % 2;
        for (int e = 0; e < 3; e++) c += (seg_n(clamp_level(L[e], lmax, sp), sp) + ni - 2 + 1) / 2;
        for (int m = ni - 2; m >= 2; m -= 2) c += 3 * ((m + m - 2 + 1) / 2);
        return c;
    }
    int n[6]; for (int i = 0; i < 6; i++) n[i] = seg_n(clamp_level(L[i], lmax, sp), sp);
    int nu = inner_n(clamp_level(L[4], lmax, sp), sp), nv = inner_n(clamp_level(L[5], lmax, sp), sp), side[4][2] = {{n[1], nu}, {n[2], nv}, {n[3], nu}, {n[0], nv}}, c = (nu - 2) * (nv - 2);
    for (int s = 0; s < 4; s++) c += (side[s][0] + side[s][1] - 2 + 1) / 2;
    return c;
}
static int is_culled(const h16 *L, int md) { return !((float)L[0] > 0 && (float)L[1] > 0 && (float)L[2] > 0 && ((md & MD_TRI) || (float)L[3] > 0)); }

// ---- the GPU side ----
#define TRI_STRIDE 24576   // float4 slots per patch in t_tris's output: 4096 entries of 6 vertices
static id<MTLBuffer> _Nonnull buf(const void *p, size_t n) {
    id<MTLBuffer> b = p ? [g_dev newBufferWithBytes:p length:n options:MTLResourceStorageModeShared] : [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "t_tess: no buffer of %zu bytes\n", n); exit(1); }
    return b;
}
static void run1(id<MTLComputePipelineState> ps, NSArray<id<MTLBuffer>> *b, const void *c0, size_t n0, const void *c1, size_t n1, MTLSize grid, MTLSize tg, int groups) {
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    for (NSUInteger i = 0; i < b.count; i++) [ce setBuffer:b[i] offset:0 atIndex:i];
    if (c0) [ce setBytes:c0 length:n0 atIndex:b.count];
    if (c1) [ce setBytes:c1 length:n1 atIndex:b.count + 1];
    if (groups) [ce dispatchThreadgroups:grid threadsPerThreadgroup:tg]; else [ce dispatchThreads:grid threadsPerThreadgroup:tg];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { fail("command buffer: %s", cb.error.localizedDescription.UTF8String); exit(1); }
}
// Triangles of np patches (6 levels each) in mode md: tris[p] gets counts[p] entries' 2 triangles each (at most 4096 entries)
static void gpu_tris(const h16 *lev, int np, float lmax, int md, Tri **tris, int *counts) {
    for (int s = 0; s < np; s += 200) {
        int n = np - s < 200 ? np - s : 200; uint32_t m = (uint32_t)md;
        id<MTLBuffer> bl = buf(lev + s * 6, n * 6 * sizeof(h16)), bo = buf(NULL, (size_t)n * TRI_STRIDE * 16), bc = buf(NULL, n * 4);
        run1(g_tris, @[bl, bo, bc], &lmax, 4, &m, 4, MTLSizeMake(n, 1, 1), MTLSizeMake(32, 1, 1), 0);
        const float *o = bo.contents; const uint32_t *cn = bc.contents;
        for (int p = 0; p < n; p++) {
            int k = (int)cn[p], c = k < 4096 ? k : 4096; counts[s + p] = k;
            Tri *t = malloc(sizeof(Tri) * (c * 2 + 1)); const float *v = o + (size_t)p * TRI_STRIDE * 4;
            for (int i = 0; i < c * 2; i++) { const float *q = v + i * 12;
                t[i] = (Tri){{q[0], q[1], q[2]}, {q[4], q[5], q[6]}, {q[8], q[9], q[10]}}; }
            tris[s + p] = t;
        }
    }
}
static double area(P2 a, P2 b, P2 c) { return ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / 2; }

// ---- topology: shared edges by vertex key ----
typedef struct { uint64_t ka, kb; int used, count, dir; P2 u, v; } Edge;
typedef struct { P2 *bnd; int nbnd, non_manifold, pos, neg, flat, degenerate; double abs_area; } Topo;
typedef uint64_t (*KeyFn)(P2, void *);
static uint64_t mix(uint64_t a, uint64_t b) { uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2)); h ^= h >> 31; return h * 0xBF58476D1CE4E5B9ull; }
static Topo topology(const Tri *tris, int n, KeyFn key, void *ctx) {
    Topo T = {0}; size_t cap = 64; while (cap < (size_t)n * 6 + 16) cap <<= 1;
    Edge *E = calloc(cap, sizeof(Edge));
    for (int i = 0; i < n; i++) {
        P2 p[3] = {tris[i].a, tris[i].b, tris[i].c}; uint64_t k[3]; for (int j = 0; j < 3; j++) k[j] = key(p[j], ctx);
        if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) { T.degenerate++; continue; }
        double ar = area(p[0], p[1], p[2]); T.abs_area += fabs(ar);
        if (ar > 1e-14) T.pos++; else if (ar < -1e-14) T.neg++; else T.flat++;
        for (int j = 0; j < 3; j++) {
            int a = j, b = (j + 1) % 3; uint64_t lo = k[a] < k[b] ? k[a] : k[b], hi = k[a] < k[b] ? k[b] : k[a]; int d = k[a] < k[b] ? 1 : -1;
            size_t h = mix(lo, hi) & (cap - 1);
            while (E[h].used && !(E[h].ka == lo && E[h].kb == hi)) h = (h + 1) & (cap - 1);
            if (E[h].used) { E[h].count++; E[h].dir += d; }
            else E[h] = (Edge){lo, hi, 1, 1, d, p[a], p[b]};
        }
    }
    T.bnd = malloc(sizeof(P2) * 2 * (cap + 1));
    for (size_t h = 0; h < cap; h++) if (E[h].used) {
        if (E[h].dir != 0) { T.bnd[T.nbnd * 2] = E[h].u; T.bnd[T.nbnd * 2 + 1] = E[h].v; T.nbnd++; }
        if (E[h].count > 2) T.non_manifold++;
    }
    free(E); return T;
}
static uint64_t exact_key(P2 p, void *ctx) { (void)ctx; float x = (float)p.x, y = (float)p.y; uint32_t a, b; memcpy(&a, &x, 4); memcpy(&b, &y, 4); return (uint64_t)a << 32 | b; }
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int uniq(double *v, int n) { qsort(v, n, sizeof(double), cmp_d); int k = 0; for (int i = 0; i < n; i++) if (!k || v[i] != v[k - 1]) v[k++] = v[i]; return k; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static const float k_interesting[] = {0.25f, 1, 1.5f, 1.99f, 2, 2.01f, 2.5f, 3, 3.5f, 3.99f, 4, 4.01f, 5.5f, 6, 7.3f, 8, 9, 12.7f, 15.9f, 16, 17, 23.5f, 31, 32, 33.3f, 47, 63.9f, 64, 70, 100};
static const float k_small[] = {0.25f, 1, 1.5f, 1.99f, 2, 2.01f, 2.5f, 3, 3.5f, 3.99f, 4, 4.01f, 5.5f, 6, 7.3f, 8, 9, 12.7f, 15.9f, 16, 17, 23.5f, 31, 32, 33.3f};   // k_interesting <= 40
static const float k_pow2[] = {2, 4, 8, 16};
static h16 rand_level(void) { return rnd() % 3 == 0 ? (h16)(rndf() * 66) : (h16)PICK(k_interesting); }
// The side of the domain a boundary edge lies on (quads: u=0, v=0, u=1, v=1 as edges 0..3; triangles: u=0, v=0, w=0 as 0..2), -1 for none
static int side_of(P2 a, P2 c, int tri) {
    if (a.x == 0 && c.x == 0) return 0;
    if (a.y == 0 && c.y == 0) return 1;
    if (tri) return a.w == 0 && c.w == 0 ? 2 : -1;
    return a.x == 1 && c.x == 1 ? 2 : a.y == 1 && c.y == 1 ? 3 : -1;
}
static double along(P2 a, int s, int tri) { return tri ? (s == 1 ? a.x : a.y) : (s == 0 || s == 2 ? a.y : a.x); }   // the coordinate that runs along side s

static int part_a(void) {
    static const float curated[][6] = {
        {2, 2, 2, 2, 2, 2}, {8, 8, 8, 8, 8, 8}, {16, 16, 16, 16, 16, 16}, {64, 64, 64, 64, 64, 64},
        {2, 2, 2, 2, 2, 16}, {2, 2, 2, 2, 16, 2}, {16, 16, 16, 16, 2, 2}, {2, 2, 2, 2, 64, 64}, {3.5f, 60, 3.5f, 60, 2, 64},
        {2.3f, 5.7f, 9.1f, 15.9f, 4, 4}, {3, 3, 3, 3, 3, 3}, {5.01f, 6.99f, 7.5f, 2.01f, 3.3f, 11.1f}, {1, 1, 1, 1, 1, 1}, {0.5f, 33, 0.5f, 33, 0.5f, 33},
        {64, 2, 64, 2, 64, 2}, {2, 64, 2, 64, 2, 64}, {100, 100, 100, 100, 100, 100}, {16, 2, 2, 2, 16, 16}, {2, 16, 2, 2, 2, 2}, {2, 2, 16, 2, 2, 2}, {2, 2, 2, 16, 2, 2},
        {16, 8, 16, 8, 8, 8}, {8, 16, 4, 2, 2, 2}, {1, 1, 1, 1, 3, 1}, {1, 1, 4, 1, 1, 1}, {1, 1, 1, 7, 1, 1}, {1.01f, 1, 1, 1, 1, 1}, {7, 7, 7, 1, 1, 1}, {63, 63, 63, 2, 63, 9}, {1, 1, 1, 1, 1.5f, 1.5f}};
    int nc = (int)(sizeof(curated) / sizeof(curated[0])), np = nc + 1500;
    h16 *lev = malloc(np * 6 * sizeof(h16));
    for (int i = 0; i < nc; i++) for (int j = 0; j < 6; j++) lev[i * 6 + j] = (h16)curated[i][j];
    for (int i = nc; i < np; i++) for (int j = 0; j < 6; j++) lev[i * 6 + j] = rand_level();
    Tri **tris = malloc(np * sizeof(Tri *)); int *counts = malloc(np * sizeof(int));
    long patches = 0, ntri = 0, flip_exact = 0, flip_off = 0, flat = 0; int flip_ulp = 0; double edge_err = 0;
    double *side = malloc(sizeof(double) * 2 * 8200), ref[160];
    for (int md = 0; md < 16; md++) {
        if (md_sp(md) == 3) continue;
        int tri = md & MD_TRI, cw = (md & MD_CW) != 0, sp = md_sp(md);
        for (int li = 0; li < 2; li++) {
            float lmax = li ? 16 : 64; gpu_tris(lev, np, lmax, md, tris, counts);
            for (int p = 0; p < np; p++) {
                const h16 *L = lev + p * 6; char tag[200];
                snprintf(tag, sizeof tag, "%s lmax %d L=[%g %g %g %g %g %g]", md_name(md), (int)lmax, (float)L[0], (float)L[1], (float)L[2], (float)L[3], (float)L[4], (float)L[5]);
                int want = ref_count(L, lmax, md), n = counts[p];
                if (n != want) { fail("%s: %d entries, expected %d", tag, n, want); free(tris[p]); continue; }
                if (n > 4096) { fail("%s: %d entries exceed the 12-bit entry field", tag, n); free(tris[p]); continue; }
                patches++; ntri += n * 2;
                for (int i = 0; i < n * 2; i++) { P2 v[3] = {tris[p][i].a, tris[p][i].b, tris[p][i].c};
                    for (int j = 0; j < 3; j++) if (!(v[j].x >= 0 && v[j].x <= 1 && v[j].y >= 0 && v[j].y <= 1 && (!tri || (v[j].w >= 0 && v[j].w <= 1 && fabs(v[j].x + v[j].y + v[j].w - 1) < 1e-6)))) {
                        fail("%s: coordinate outside the domain (%g, %g, %g)", tag, v[j].x, v[j].y, v[j].w); i = n * 2; break; } }
                Topo T = topology(tris[p], n * 2, exact_key, NULL); flat += T.flat;
                if (T.non_manifold) fail("%s: %d non-manifold edges", tag, T.non_manifold);
                if (cw ? T.pos : T.neg) fail("%s: %d triangles against the domain's winding (folds)", tag, cw ? T.pos : T.neg);
                if (fabs(T.abs_area - (tri ? 0.5 : 1)) > 1e-5) fail("%s: |area| %g (overlap or gap)", tag, T.abs_area);
                for (int e = 0; e < (tri ? 3 : 4); e++) {
                    int k = 0;
                    for (int b = 0; b < T.nbnd; b++) {
                        P2 a = T.bnd[b * 2], c = T.bnd[b * 2 + 1]; int s = side_of(a, c, tri);
                        if (s < 0) { if (e == 0) fail("%s: boundary edge inside the patch (%g,%g,%g)-(%g,%g,%g)", tag, a.x, a.y, a.w, c.x, c.y, c.w); continue; }
                        if (s == e) { side[k++] = along(a, s, tri); side[k++] = along(c, s, tri); }
                    }
                    k = uniq(side, k); int nr = ref_positions(clamp_level(L[e], lmax, sp), sp, ref);
                    if (k != nr) { fail("%s: side %d has %d boundary vertices, reference %d", tag, e, k, nr); continue; }
                    for (int i = 0; i < k; i++) edge_err = fmax(edge_err, fabs(side[i] - ref[i]));
                    for (int i = 0; i < k; i++) {
                        float f = 1.0f - (float)side[i]; int hit = 0, best = 0;
                        for (int j = 0; j < k; j++) { if (side[j] == (double)f) hit = 1; if (fabs(side[j] - f) < fabs(side[best] - f)) best = j; }
                        if (hit) flip_exact++; else { flip_off++; int u = abs((int)fbits((float)side[best]) - (int)fbits(f)); if (u > flip_ulp) flip_ulp = u; }
                    }
                }
                if (edge_err > 1e-6) fail("%s: edge position error %g", tag, edge_err);
                free(T.bnd); free(tris[p]);
            }
        }
    }
    static const float cull_cases[][6] = {{0, 8, 8, 8, 8, 8}, {8, -1, 8, 8, 8, 8}, {8, 8, NAN, 8, 8, 8}, {8, 8, 8, 0, 8, 8}, {8, 8, 8, 8, 0, 0}, {8, 8, 8, 8, NAN, -3}, {0.001f, 0.001f, 0.001f, 0.001f, 0, 0}};
    static const int cull_quads[] = {1, 1, 1, 1, 0, 0, 0}, cull_tris[] = {1, 1, 1, 0, 0, 0, 0};   // a triangle domain has no fourth edge
    h16 cl[7 * 6]; for (int i = 0; i < 7 * 6; i++) cl[i] = (h16)cull_cases[i / 6][i % 6];
    int cull_ok = 1;
    for (int t = 0; t < 2; t++) {
        id<MTLBuffer> bl = buf(cl, sizeof cl), bo = buf(NULL, 7 * 4); uint32_t m = (uint32_t)t;
        run1(g_cull, @[bl, bo], &m, 4, NULL, 0, MTLSizeMake(7, 1, 1), MTLSizeMake(32, 1, 1), 0);
        for (int i = 0; i < 7; i++) if ((((uint32_t *)bo.contents)[i] != 0) != (t ? cull_tris : cull_quads)[i]) { cull_ok = 0; fail("%s culling case %d: %u", t ? "triangle" : "quad", i, ((uint32_t *)bo.contents)[i]); }
    }
    printf("A. per patch: %ld patches (12 modes, lmax 64 and 16), %ld triangles: edge positions vs reference max error %.2e; reversed edges: %ld positions exact, %ld off by at most %d ulp; zero-area triangles with distinct vertices %ld; culling %s\n",
           patches, ntri, edge_err, flip_exact, flip_off, flip_ulp, flat, cull_ok ? "ok" : "FAILED");
    free(side); free(tris); free(counts); free(lev); return 0;
}

// ---- B: meshes ----
typedef struct { double *xs, *ys; uint64_t *xid, *yid; int nx, ny; } Clusters;
static uint64_t find_id(const double *v, const uint64_t *id, int n, double x) { int lo = 0, hi = n - 1; while (lo < hi) { int m = (lo + hi) / 2; if (v[m] < x) lo = m + 1; else hi = m; } return id[lo]; }
static uint64_t cluster_key(P2 p, void *ctx) { Clusters *c = ctx; return find_id(c->xs, c->xid, c->nx, p.x) << 32 | find_id(c->ys, c->yid, c->ny, p.y); }
static int cluster(double *v, int n, uint64_t *id) { n = uniq(v, n); uint64_t k = 0; double last = -1e9; for (int i = 0; i < n; i++) { if (v[i] - last > 1e-5) k++; id[i] = k; last = v[i]; } return n; }
static h16 mesh_level(int mesh) {
    if (mesh % 3 == 0) return (h16)PICK(k_pow2);
    return rnd() % 2 == 0 ? (h16)(rndf() * 30 + 1) : (h16)PICK(k_small);
}
// Cracks and folds of a mesh of k global triangles covering NX x NY
static void mesh_check(Tri *tris, int k, int NX, int NY, double want_area, const char *what, int mesh, int *cracks, int *folds) {
    Clusters c = {malloc(sizeof(double) * 3 * (k + 1)), malloc(sizeof(double) * 3 * (k + 1)), malloc(8 * 3 * (k + 1)), malloc(8 * 3 * (k + 1)), 0, 0};
    for (int i = 0; i < k; i++) { c.xs[i * 3] = tris[i].a.x; c.xs[i * 3 + 1] = tris[i].b.x; c.xs[i * 3 + 2] = tris[i].c.x; c.ys[i * 3] = tris[i].a.y; c.ys[i * 3 + 1] = tris[i].b.y; c.ys[i * 3 + 2] = tris[i].c.y; }
    c.nx = cluster(c.xs, k * 3, c.xid); c.ny = cluster(c.ys, k * 3, c.yid);
    Topo T = topology(tris, k, cluster_key, &c); int inner = 0; P2 ia = {0}, ib = {0};
    for (int b = 0; b < T.nbnd; b++) { P2 a = T.bnd[b * 2], d = T.bnd[b * 2 + 1];
        if ((fabs(a.x) < 1e-6 && fabs(d.x) < 1e-6) || (fabs(a.y) < 1e-6 && fabs(d.y) < 1e-6) || (fabs(a.x - NX) < 1e-6 && fabs(d.x - NX) < 1e-6) || (fabs(a.y - NY) < 1e-6 && fabs(d.y - NY) < 1e-6)) continue;
        if (!inner++) { ia = a; ib = d; } }
    if (inner) { (*cracks)++; fail("%s mesh %d: %d open edges inside the mesh (crack), e.g. (%g,%g)-(%g,%g)", what, mesh, inner, ia.x, ia.y, ib.x, ib.y); }
    if (T.non_manifold || T.neg || fabs(T.abs_area - want_area) > 1e-3) { (*folds)++; fail("%s mesh %d: non-manifold %d, reversed %d, |area| %g", what, mesh, T.non_manifold, T.neg, T.abs_area); }
    free(T.bnd); free(c.xs); free(c.ys); free(c.xid); free(c.yid);
}
static void to_global(float *u, float *v, int s) { if (s & 1) *u = 1 - *u; if (s & 2) *v = 1 - *v; if (s & 4) { float t = *u; *u = *v; *v = t; } }
static void part_b_quads(int sp, int *meshes, int *cracks, int *folds, long *ntri) {
    enum { NX = 10, NY = 10, NP = NX * NY };
    for (int mesh = 0; mesh < 12; mesh++) {
        h16 hE[NY + 1][NX], vE[NY][NX + 1], lev[NP * 6]; int sym[NP];
        for (int y = 0; y <= NY; y++) for (int x = 0; x < NX; x++) hE[y][x] = mesh_level(mesh);
        for (int y = 0; y < NY; y++) for (int x = 0; x <= NX; x++) vE[y][x] = mesh_level(mesh);
        static const float mids[4][2] = {{0, 0.5f}, {0.5f, 0}, {1, 0.5f}, {0.5f, 1}};
        for (int y = 0; y < NY; y++) for (int x = 0; x < NX; x++) {
            int p = y * NX + x, s = mesh % 4 == 0 ? 0 : (int)(rnd() % 8); h16 *L = lev + p * 6; sym[p] = s;
            for (int e = 0; e < 4; e++) { float u = mids[e][0], v = mids[e][1]; to_global(&u, &v, s);
                L[e] = u == 0 ? vE[y][x] : u == 1 ? vE[y][x + 1] : v == 0 ? hE[y][x] : hE[y + 1][x]; }
            if (mesh % 2 == 0) { float m = fminf(fminf((float)L[0], (float)L[1]), fminf((float)L[2], (float)L[3])); L[4] = L[5] = (h16)m; }   // even meshes: inside = min of the edges
            else { L[4] = mesh_level(mesh); L[5] = mesh_level(mesh); }
        }
        Tri *pt[NP]; int cnt[NP]; gpu_tris(lev, NP, 64, sp << 1, pt, cnt);
        int n = 0; for (int p = 0; p < NP; p++) n += (cnt[p] < 4096 ? cnt[p] : 4096) * 2;
        Tri *tris = malloc(sizeof(Tri) * (n + 1)); int k = 0;
        for (int p = 0; p < NP; p++) {
            int s = sym[p], odd = ((s & 1) + ((s >> 1) & 1) + ((s >> 2) & 1)) % 2, x = p % NX, y = p / NX, m = (cnt[p] < 4096 ? cnt[p] : 4096) * 2;
            for (int i = 0; i < m; i++) {
                P2 v[3] = {pt[p][i].a, pt[p][i].b, pt[p][i].c};
                for (int j = 0; j < 3; j++) { float u = (float)v[j].x, w = (float)v[j].y; to_global(&u, &w, s); v[j] = (P2){(double)((float)x + u), (double)((float)y + w), 0}; }
                tris[k++] = odd ? (Tri){v[0], v[2], v[1]} : (Tri){v[0], v[1], v[2]};
            }
            free(pt[p]);
        }
        mesh_check(tris, k, NX, NY, NP, md_name(sp << 1), mesh, cracks, folds);
        (*meshes)++; *ntri += k; free(tris);
    }
}
// Squares of two triangles: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1). A patch's corners w, u, v (counter-clockwise in its domain) are the triangle's
// corners from r on, in order (rotations) or reversed (reflections, which reverse its triangles); its edges take the levels of the mesh edges they lie on
static void part_b_tris(int sp, int *meshes, int *cracks, int *folds, long *ntri) {
    enum { NX = 10, NY = 10, NP = NX * NY * 2 };
    for (int mesh = 0; mesh < 12; mesh++) {
        h16 hE[NY + 1][NX], vE[NY][NX + 1], dE[NY][NX], lev[NP * 6]; int g[NP][3][2], refl[NP];
        for (int y = 0; y <= NY; y++) for (int x = 0; x < NX; x++) hE[y][x] = mesh_level(mesh);
        for (int y = 0; y < NY; y++) for (int x = 0; x <= NX; x++) vE[y][x] = mesh_level(mesh);
        for (int y = 0; y < NY; y++) for (int x = 0; x < NX; x++) dE[y][x] = mesh_level(mesh);
        for (int p = 0; p < NP; p++) {
            int cell = p / 2, x = cell % NX, y = cell / NX, t = p % 2;
            int c[3][2] = {{x, y}, {x + 1, y + t}, {x + 1 - t, y + 1}};   // t 0: (0,0) (1,0) (1,1); t 1: (0,0) (1,1) (0,1)
            if (t) { c[1][0] = x + 1; c[1][1] = y + 1; c[2][0] = x; c[2][1] = y + 1; }
            int r = mesh % 4 == 0 ? 0 : (int)(rnd() % 3), f = mesh % 4 == 0 ? 0 : (int)(rnd() % 2); refl[p] = f;
            for (int i = 0; i < 3; i++) { int j = f ? (r + 3 - i) % 3 : (r + i) % 3; g[p][i][0] = c[j][0]; g[p][i][1] = c[j][1]; }   // g[p]: the global corners of w, u, v
            h16 *L = lev + p * 6;
            // L[0]: u = 0, between v and w; L[1]: v = 0, between w and u; L[2]: w = 0, between u and v
            static const int ends[3][2] = {{2, 0}, {0, 1}, {1, 2}};
            for (int e = 0; e < 3; e++) {
                const int *a = g[p][ends[e][0]], *b = g[p][ends[e][1]];
                L[e] = a[1] == b[1] ? hE[a[1]][a[0] < b[0] ? a[0] : b[0]] : a[0] == b[0] ? vE[a[1] < b[1] ? a[1] : b[1]][a[0]] : dE[a[1] < b[1] ? a[1] : b[1]][a[0] < b[0] ? a[0] : b[0]];
            }
            L[3] = (h16)(rndf() * 50); L[5] = (h16)(rndf() * 50);   // not a triangle domain's: ignored
            L[4] = mesh % 2 == 0 ? (h16)fminf(fminf((float)L[0], (float)L[1]), (float)L[2]) : mesh_level(mesh);
        }
        Tri *pt[NP]; int cnt[NP]; gpu_tris(lev, NP, 64, MD_TRI | sp << 1, pt, cnt);
        int n = 0; for (int p = 0; p < NP; p++) n += (cnt[p] < 4096 ? cnt[p] : 4096) * 2;
        Tri *tris = malloc(sizeof(Tri) * (n + 1)); int k = 0;
        for (int p = 0; p < NP; p++) {
            int m = (cnt[p] < 4096 ? cnt[p] : 4096) * 2;
            for (int i = 0; i < m; i++) {
                P2 v[3] = {pt[p][i].a, pt[p][i].b, pt[p][i].c};
                for (int j = 0; j < 3; j++) { double u = (float)v[j].x, vv = (float)v[j].y, w = (float)v[j].w;
                    v[j] = (P2){u * g[p][1][0] + vv * g[p][2][0] + w * g[p][0][0], u * g[p][1][1] + vv * g[p][2][1] + w * g[p][0][1], 0}; }
                tris[k++] = refl[p] ? (Tri){v[0], v[2], v[1]} : (Tri){v[0], v[1], v[2]};
            }
            free(pt[p]);
        }
        mesh_check(tris, k, NX, NY, NX * NY, md_name(MD_TRI | sp << 1), mesh, cracks, folds);
        (*meshes)++; *ntri += k; free(tris);
    }
}
static void part_b(void) {
    int meshes = 0, cracks = 0, folds = 0; long ntri = 0;
    for (int sp = 0; sp < 3; sp++) { part_b_quads(sp, &meshes, &cracks, &folds, &ntri); part_b_tris(sp, &meshes, &cracks, &folds, &ntri); }
    printf("B. meshes: %d meshes (10x10 quads in all 8 orientations, 10x10 squares of two triangles in all 6; every spacing; half with inside = min of edges), %ld triangles: %d with cracks, %d with folds / overlaps\n", meshes, ntri, cracks, folds);
}

// ---- C: the expansion kernel ----
enum { GUARD = 4096 };
static uint32_t expand(h16 *lev, int np, int cap, int lmax, int md, int fix, id<MTLBuffer> *list, uint32_t st[64], int *guard_ok) {
    id<MTLBuffer> bl = buf(lev, np * 6 * sizeof(h16)), b = buf(NULL, (size_t)(cap + GUARD) * 4);
    uint32_t *lp = b.contents; for (int i = 0; i < cap + GUARD; i++) lp[i] = 0xDEADBEEF;
    uint32_t ka[12] = {0, 1, 0, 7, 0, (uint32_t)cap, (uint32_t)np, (uint32_t)lmax, (uint32_t)md, (uint32_t)fix};
    id<MTLBuffer> ba = buf(ka, 48), bs = buf(NULL, 256); memset(bs.contents, 0, 256);
    run1(g_expand, @[bl, b, ba, bs], NULL, 0, NULL, 0, MTLSizeMake(np, 1, 1), MTLSizeMake(64, 1, 1), 1);
    memcpy(st, bs.contents, 256); *guard_ok = 1;
    for (int i = cap; i < cap + GUARD; i++) if (lp[i] != 0xDEADBEEF) { *guard_ok = 0; break; }
    memcpy(lev, bl.contents, np * 6 * sizeof(h16));   // a fixed level is written into the factors, for the domain
    *list = b; return ((uint32_t *)ba.contents)[0];
}
static void part_c(void) {
    static const int runs[4][3] = {{64, 20000, 4}, {16, 20000, 1}, {64, 1000, 5}, {64, 4000, 3}};   // lmax, patches, md
    static const float culls[] = {0, -1, NAN};
    for (int r = 0; r < 5; r++) {
        int fix = r == 4 ? 6 : 0, lmax = runs[r % 4][0], np = runs[r % 4][1], md = r == 4 ? MD_TRI : runs[r][2];
        h16 *lev = malloc(np * 6 * sizeof(h16)), *orig = malloc(np * 6 * sizeof(h16)); int *want = malloc(np * sizeof(int)), *seen = calloc(np, sizeof(int)); long total = 0; int ncull = 0, nonempty = 0;
        for (int p = 0; p < np; p++) {
            for (int j = 0; j < 6; j++) lev[p * 6 + j] = rand_level();
            if (rnd() % 10 == 0) { int e = (int)(rnd() % ((md & MD_TRI) ? 3 : 4)); lev[p * 6 + e] = (h16)PICK(culls); }
            int c = is_culled(lev + p * 6, md); ncull += c;
            h16 fl[6]; for (int j = 0; j < 6; j++) fl[j] = fix ? (h16)fix : lev[p * 6 + j];
            want[p] = c ? 0 : ref_count(fl, lmax, md); total += want[p]; nonempty += want[p] > 0;
        }
        memcpy(orig, lev, np * 6 * sizeof(h16));
        id<MTLBuffer> lb; uint32_t st[64]; int guard_ok;
        uint32_t v = expand(lev, np, (int)total + 100, lmax, md, fix, &lb, st, &guard_ok);
        if (v != total * 6) fail("expand %s lmax %d fix %d: vertex count %u, expected %ld", md_name(md), lmax, fix, v, total * 6);
        if ((int)st[1] != ncull || st[2] != 0) fail("expand %s lmax %d: culled %u of %d, dropped %u", md_name(md), lmax, st[1], ncull, st[2]);
        if (fix) { int ok = 1; for (int p = 0; p < np; p++) for (int j = 0; j < 6; j++) { h16 w = is_culled(orig + p * 6, md) ? orig[p * 6 + j] : (h16)fix; uint16_t x, y; memcpy(&x, &lev[p * 6 + j], 2); memcpy(&y, &w, 2); if (x != y) ok = 0; }   // bits: a NaN factor stays NaN
            if (!ok) fail("expand fix %d: the factors are not the fixed level (culled patches keep theirs)", fix); }
        uint8_t *bits = calloc(((size_t)np << 12) / 8 + 1, 1); int bad = 0, dup = 0; const uint32_t *l = lb.contents;
        for (long i = 0; i < total; i++) { uint32_t e = l[i]; int p = (int)(e >> 12), x = (int)(e & 4095);
            if (e == 0xDEADBEEF || p >= np || x >= want[p]) { bad++; continue; }
            seen[p]++; if (bits[e >> 3] & (1 << (e & 7))) dup++; bits[e >> 3] |= 1 << (e & 7); }
        int counts_ok = memcmp(seen, want, np * sizeof(int)) == 0;
        if (bad || !counts_ok || dup || !guard_ok) fail("expand %s lmax %d: bad %d, per-patch counts %s, duplicates %d, guard %s", md_name(md), lmax, bad, counts_ok ? "ok" : "wrong", dup, guard_ok ? "intact" : "overwritten");
        if (!fix) {
            int cap = (int)(total / 3); memset(seen, 0, np * sizeof(int)); memcpy(lev, orig, np * 6 * sizeof(h16));
            uint32_t pv = expand(lev, np, cap, lmax, md, 0, &lb, st, &guard_ok), acc = pv / 6; l = lb.contents;
            for (uint32_t i = 0; i < acc && i < (uint32_t)cap; i++) if ((l[i] >> 12) < (uint32_t)np) seen[l[i] >> 12]++;
            int complete = 1, accepted = 0; for (int p = 0; p < np; p++) { if (seen[p] && seen[p] != want[p]) complete = 0; accepted += seen[p] > 0; }
            if ((int)acc > cap || !complete || !guard_ok || accepted + (int)st[2] != nonempty)
                fail("expand budget %s lmax %d: accepted %u of cap %d, complete %d, guard %d, %d + %u dropped != %d", md_name(md), lmax, acc, cap, complete, guard_ok, accepted, st[2], nonempty);
            printf("C. expansion kernel %s lmax %d: %d patches (%d culled) -> %ld entries exact; budget %d: %d patches whole, %u dropped whole, guard %s\n",
                   md_name(md), lmax, np, ncull, total, cap, accepted, st[2], guard_ok ? "intact" : "overwritten");
        } else printf("C. expansion kernel %s, every level %d: %d patches (%d culled) -> %ld entries exact, the factors rewritten\n", md_name(md), fix, np, ncull, total);
        free(bits); free(lev); free(orig); free(want); free(seen);
    }
}

// ---- D: folds for game-like factors ----
static void part_d(void) {
    static const char *modes[] = {"all equal", "inside = max of edges", "inside = mean of edges", "inside = min of edges (The Witcher 3's hulls)", "fractional edges, inside = min"};
    enum { N = 2000 };
    int bad = 0;
    for (int md = 0; md < 8; md++) {
        if (md_sp(md) == 3) continue;
        int tri = md & MD_TRI, ne = tri ? 3 : 4;
        for (int m = 0; m < 5; m++) {
            h16 *lev = malloc(N * 6 * sizeof(h16)); int is_min = m >= 3;
            for (int p = 0; p < N; p++) {
                float e[4]; for (int i = 0; i < 4; i++) e[i] = m == 4 ? rndf() * 15 + 1 : PICK(k_pow2);
                if (m == 0) e[1] = e[2] = e[3] = e[0];
                float a, b;
                if (is_min) a = b = tri ? fminf(fminf(e[0], e[1]), e[2]) : fminf(fminf(e[0], e[1]), fminf(e[2], e[3]));
                else if (m == 2) { a = tri ? (e[0] + e[1] + e[2]) / 3 : (e[1] + e[3]) / 2; b = (e[0] + e[2]) / 2; }
                else { a = tri ? fmaxf(fmaxf(e[0], e[1]), e[2]) : fmaxf(e[1], e[3]); b = fmaxf(e[0], e[2]); }
                for (int i = 0; i < 4; i++) lev[p * 6 + i] = (h16)e[i];
                lev[p * 6 + 4] = (h16)a; lev[p * 6 + 5] = (h16)b;
            }
            Tri **tris = malloc(N * sizeof(Tri *)); int *cnt = malloc(N * sizeof(int)), folded = 0;
            gpu_tris(lev, N, 64, md, tris, cnt);
            for (int p = 0; p < N; p++) { Topo T = topology(tris[p], (cnt[p] < 4096 ? cnt[p] : 4096) * 2, exact_key, NULL);
                if (T.neg || T.non_manifold || fabs(T.abs_area - (tri ? 0.5 : 1)) > 1e-5) folded++; free(T.bnd); free(tris[p]); }
            if (folded) fail("D %s, %s: %d folded patches", md_name(md), modes[m], folded);
            bad += folded; (void)ne;
            free(tris); free(cnt); free(lev);
        }
    }
    printf("D. game-like factors (%s; every domain and spacing, %d patches each): %d patches with folded / overlapping triangles\n", "all equal, inside = max / mean / min of edges, fractional edges", N, bad);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: t_tess <tess_test.metal> [seed]\n"); return 2; }
        if (argc > 2) g_rng = strtoull(argv[2], NULL, 10);
        g_dev = MTLCreateSystemDefaultDevice(); g_q = [g_dev newCommandQueue];
        NSError *e = nil; NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&e];
        if (!src) { fprintf(stderr, "t_tess: cannot read %s\n", argv[1]); return 2; }
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) { fprintf(stderr, "t_tess: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> __strong *ps[3] = {&g_tris, &g_cull, &g_expand}; const char *names[3] = {"t_tris", "t_cull", "dm_tess_expand"};
        for (int i = 0; i < 3; i++) {
            id<MTLFunction> f = [lib newFunctionWithName:@(names[i])];
            if (!f) { fprintf(stderr, "t_tess: %s is not in the fixture\n", names[i]); return 1; }
            *ps[i] = [g_dev newComputePipelineStateWithFunction:f error:&e];
            if (!*ps[i]) { fprintf(stderr, "t_tess: %s: %s\n", names[i], e.localizedDescription.UTF8String); return 1; }
        }
        part_a(); part_b(); part_c(); part_d();
        if (g_fail) printf("%d FAILURES\n", g_fail); else printf("ALL PASSED\n");
        return g_fail ? 1 : 0;
    }
}
