// The blend-zero word's rule, checked over its finite per-target domain in full, and the check that guards its use. The invariant: a pipeline made with pso_key.bz_off (the pixel shader's guard
// specialized away) is never drawn while the draw's blend-zero word is not 0. It is the two halves below; only (a)'s per-target part is exhaustive, the rest are seeded samples.
//  (a) the rule: nvn_mtl.m bz_word_of against a reference written here from the specification alone (dmsh_rewrite.py blend_zero, docs/reference/shaders.md "A NaN colour under a zero blend weight"):
//      per colour target, the word's bits are set iff the target's format is a floating-point one AND blending is on for the target AND the colour equation is not MIN / MAX AND the destination factor is
//      not SRC_COLOR, ONE_MINUS_SRC_COLOR or a SRC1 one AND the source factor is SRC_ALPHA (bit 1) or ONE_MINUS_SRC_ALPHA (bit 2); the bits sit at DMSH_BLEND_ZERO_BITS * the logical output. Checked on EVERY
//      per-target combination (each format the host binds, each slot, blending on / off, both 7-bit factors in full, each equation, each advanced mode: exhaustive), then on the composition across
//      targets (the full word is the OR of the targets' own words, with and without a merged pass's map), over seeded random 8-target states (a sample, not a proof). The advanced-mode coefficients
//      follow NV_blend_equation_advanced; when the host can take a mode as fixed-function factors is the host's own rule (adv_blend), restated here, not independent of it
//  (b) the key and the draw evaluate it on the same state: pso_key_fill's key (normalized: factors of a disabled target dropped, blending of an absent target dropped) and pso_key_bz_off give
//      bz_off == has_bz && the reference word of the state as the draw has it == 0, for a large seeded set of states, merged or not
// And bz_bind's runtime check of the invariant (bz_audit): the line it writes when the invariant is broken, and its rate limit.
// make GAME=<id> check_t_blend_zero_rule (./t_blend_zero_rule <scratch dir>)
#include "../nvn/nvn_mtl.m"
#include <fcntl.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SC = NVN_BLEND_FUNC_SRC_COLOR, ISC = NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR, SA = NVN_BLEND_FUNC_SRC_ALPHA, ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
       S1C = NVN_BLEND_FUNC_SRC1_COLOR, IS1C = NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR, S1A = NVN_BLEND_FUNC_SRC1_ALPHA, IS1A = NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA,
       ADD = NVN_BLEND_EQUATION_ADD, MINEQ = NVN_BLEND_EQUATION_MIN, MAXEQ = NVN_BLEND_EQUATION_MAX };

// ---- the reference, from the specification
// A colour format is a floating-point one by NVN's own names for it (every format of nvn.h that is a float colour format; RGB9E5F is not one: it cannot store NaN); the host binds the Metal format the
// format table gives it, so that is what a pass's cfmt holds
static int nvn_float_colour(int f) {
    switch (f) {
        case NVN_FORMAT_R16F: case NVN_FORMAT_R32F: case NVN_FORMAT_RG16F: case NVN_FORMAT_RG32F: case NVN_FORMAT_RGB16F: case NVN_FORMAT_RGB32F: case NVN_FORMAT_RGBA16F: case NVN_FORMAT_RGBA32F:
        case NVN_FORMAT_R11G11B10F: case NVN_FORMAT_RGBX16F: case NVN_FORMAT_RGBX32F: return 1;
        default: return 0;
    }
}
static uint32_t g_fm[256]; static int g_ffl[256], g_nfm;   // the distinct Metal formats the table gives, and whether each is a float colour format
static int ref_is_float(uint32_t m) { for (int i = 0; i < g_nfm; i++) if (g_fm[i] == m) return g_ffl[i]; return 0; }   // a format the host cannot bind is none
// The advanced modes the host takes for fixed-function factors (NV_blend_equation_advanced's Porter-Duff operators, each as its two coefficients; A = the source alpha, D = the destination alpha;
// the rest of the modes need the destination colour in the shader). A coefficient is 'z' zero, 'o' one, 'a' A, 'b' 1 - A, 'c' D, 'd' 1 - D
static int pd_modes(int mode, char *src, char *dst) {
    static const struct { int mode; char s, d; } t[] = {
        {NVN_BLEND_ADVANCED_MODE_ZERO, 'z', 'z'}, {NVN_BLEND_ADVANCED_MODE_SRC, 'o', 'z'}, {NVN_BLEND_ADVANCED_MODE_DST, 'z', 'o'}, {NVN_BLEND_ADVANCED_MODE_SRC_OVER, 'o', 'b'},
        {NVN_BLEND_ADVANCED_MODE_DST_OVER, 'd', 'o'}, {NVN_BLEND_ADVANCED_MODE_SRC_IN, 'c', 'z'}, {NVN_BLEND_ADVANCED_MODE_DST_IN, 'z', 'a'}, {NVN_BLEND_ADVANCED_MODE_SRC_OUT, 'd', 'z'},
        {NVN_BLEND_ADVANCED_MODE_DST_OUT, 'z', 'b'}, {NVN_BLEND_ADVANCED_MODE_SRC_ATOP, 'c', 'b'}, {NVN_BLEND_ADVANCED_MODE_DST_ATOP, 'd', 'a'}, {NVN_BLEND_ADVANCED_MODE_XOR, 'd', 'b'},
        {NVN_BLEND_ADVANCED_MODE_PLUS, 'o', 'o'}, {NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED, 'o', 'o'}, {NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA, 'o', 'o'},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (t[i].mode == mode) { *src = t[i].s; *dst = t[i].d; return 1; }
    return 0;
}
// The word bits of one output's blend state (1: the source colour's weight is zero where alpha is 0, 2: where alpha is 1, 0: neither or not to be touched), the target's format a float one
static unsigned ref_bits(blend_st b) {
    const int src = (int)b.src_rgb, dst = (int)b.dst_rgb, eq = (int)b.eq_rgb;
    char ps, pdst;
    // an advanced mode replaces the factors by its operator's coefficients and the equation by a sum, when the host can (not an overlap mode; a source that is not premultiplied is multiplied by its
    // alpha, which only a coefficient of one or zero admits); otherwise the state's own factors stand
    if (b.adv && !b.adv_ovl && pd_modes((int)b.adv, &ps, &pdst) && !(b.adv_npm && ps != 'o' && ps != 'z')) {
        // (the destination coefficients read alpha only, never the source colour, and the equation is a sum)
        char w = b.adv_npm && ps == 'o' ? 'a' : ps;
        return w == 'a' ? 1u : w == 'b' ? 2u : 0u;
    }
    if (eq == MINEQ || eq == MAXEQ) return 0;
    if (dst == SC || dst == ISC || dst == S1C || dst == IS1C || dst == S1A || dst == IS1A) return 0;
    return src == SA ? 1u : src == ISA ? 2u : 0u;
}
// The whole word of a state: per logical output, over the physical targets that output feeds (a target with no format takes none; a map entry past 7 feeds nothing)
static uint32_t ref_word(uint32_t en, const blend_st bl[8], const uint32_t cf[8], const uint8_t *lm) {
    uint32_t w = 0;
    for (int out = 0; out < 8; out++) {
        if (!(en >> out & 1)) continue;
        for (int t = 0; t < 8; t++) {
            int fed = lm ? lm[t] : t;
            if (!cf[t] || fed != out || !ref_is_float(cf[t])) continue;
            w |= ref_bits(bl[out]) << (DMSH_BLEND_ZERO_BITS * out);
        }
    }
    return w;
}

// ---- stderr capture (the checks read the lines the host writes)
static int g_save_fd = -1; static char g_cap_path[1024];
static void cap_begin(void) {
    fflush(stderr); g_save_fd = dup(2); int fd = open(g_cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (g_save_fd < 0 || fd < 0) { fprintf(stderr, "cannot capture stderr into %s\n", g_cap_path); abort(); }
    dup2(fd, 2); close(fd);
}
static NSString *cap_end(int echo) {
    fflush(stderr); dup2(g_save_fd, 2); close(g_save_fd); g_save_fd = -1;
    NSString *s = [NSString stringWithContentsOfFile:@(g_cap_path) encoding:NSUTF8StringEncoding error:nil] ?: @"";
    if (echo) fputs(s.UTF8String, stderr);
    return s;
}
static int count_of(NSString *s, NSString *what) { return (int)[[s componentsSeparatedByString:what] count] - 1; }

// ---- the seeded generator
static uint64_t g_rng;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static unsigned pick(unsigned n) { return (unsigned)(rnd() % n); }
static const int FACS[] = {0, Z, O, SC, ISC, SA, ISA, NVN_BLEND_FUNC_DST_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA, NVN_BLEND_FUNC_DST_COLOR, NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR, NVN_BLEND_FUNC_SRC_ALPHA_SATURATE,
    S1C, IS1C, S1A, IS1A, NVN_BLEND_FUNC_CONSTANT_COLOR, NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR, NVN_BLEND_FUNC_CONSTANT_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA, 127};
enum { NFACS = sizeof FACS / sizeof *FACS };
static blend_st rand_blend(void) {
    blend_st b = {0};
    b.target = rnd() & 7; b.src_a = rnd() & 127; b.dst_a = rnd() & 127; b.eq_a = rnd() & 7;
    unsigned r = pick(8);
    b.src_rgb = r == 0 ? (rnd() & 127) : r < 4 ? (r & 1 ? SA : ISA) : (unsigned)FACS[pick(NFACS)];
    b.dst_rgb = pick(8) == 0 ? (rnd() & 127) : (unsigned)FACS[pick(NFACS)];
    b.eq_rgb = pick(8) == 0 ? (rnd() & 7) : 1 + pick(5);
    if (pick(6) == 0) { b.adv = pick(3) ? 5 + pick(0x10) : (rnd() & 63); b.adv_npm = rnd() & 1; b.adv_ovl = pick(4) == 0 ? 1 + pick(3) : 0; b.adv_dst_free = rnd() & 1; }
    return b;
}
static uint32_t rand_fmt(void) { return pick(3) == 0 ? 0 : pick(2) ? g_fm[pick((unsigned)g_nfm)] : (pick(2) ? MTLPixelFormatRGBA16Float : MTLPixelFormatRG11B10Float); }

static uint64_t cases, bad;
#define EXPECT(got, want, ...) do { cases++; if ((got) != (want)) { bad++; if (bad <= 12) { fprintf(stderr, "COUNTEREXAMPLE: got %#x, reference %#x: ", (unsigned)(got), (unsigned)(want)); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } } while (0)

static void format_table(void) {
    for (int f = 1; f < 0xA0; f++) {
        uint32_t m; if (!nvn_fmt_to_mtl(f, &m, NULL) || !m) continue;
        int i = 0; for (; i < g_nfm; i++) if (g_fm[i] == m) break;
        if (i == g_nfm) { g_fm[g_nfm] = m; g_ffl[g_nfm++] = nvn_float_colour(f); }
        else if (g_ffl[i] != nvn_float_colour(f)) { CHECK(0); fprintf(stderr, "Metal format %u is the table's for NVN format %#x and for a format of the other kind\n", m, f); }   // a Metal format two NVN formats give is one kind
    }
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    snprintf(g_cap_path, sizeof g_cap_path, "%s/stderr.txt", argv[1]);
    state_reset(); memset(&g_pass, 0, sizeof g_pass); g_pso_lmap = NULL;
    int on = bz_on();
    const uint64_t seed = getenv("DM_T_SEED") ? strtoull(getenv("DM_T_SEED"), NULL, 0) : 0x9E3779B97F4A7C15ull; g_rng = seed;
    printf("blend zero rule (%s): seed %#llx (DM_T_SEED)\n", on ? "NVIDIA's rule" : "DM_NVN_BLEND_ZERO=0, every word 0", (unsigned long long)seed);
    format_table();
    int nfl = 0; for (int i = 0; i < g_nfm; i++) nfl += g_ffl[i];
    CHECK(g_nfm > 40 && nfl == 7);   // the table's colour formats: seven float ones (R16F RG16F RGBA16F R32F RG32F RGBA32F R11G11B10F; RGBX16F / RGBX32F share RGBA's), none else

    // ---- (a1) every per-target combination: format (those the table gives, and none) x slot x blend on / off x source factor x destination factor x equation, both factors over their whole 7 bits.
    // Blending off is the enable bit alone (the factors cannot matter): over the factors NVN names and the bounds of the field, with every equation
    uint64_t c0 = cases;
    {
        static const int named[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 18, 19, 97, 98, 99, 100, 127};
        for (int fi = -1; fi < g_nfm; fi++) {
            uint32_t m = fi < 0 ? 0 : g_fm[fi]; int fl = fi >= 0 && g_ffl[fi];
            for (int l = 0; l < 8; l++) {
                uint32_t cf[8] = {0}; cf[l] = m; blend_st bl[8]; memset(bl, 0, sizeof bl);
                for (int en = 0; en < 2; en++) {
                    int n = en ? 128 : (int)(sizeof named / sizeof *named);
                    for (int si = 0; si < n; si++) for (int di = 0; di < n; di++) for (int eq = 0; eq < 8; eq++) {
                        int s = en ? si : named[si], d = en ? di : named[di];
                        bl[l] = (blend_st){.target = (uint64_t)l, .src_rgb = (uint64_t)s, .dst_rgb = (uint64_t)d, .src_a = O, .dst_a = Z, .eq_rgb = (uint64_t)eq, .eq_a = ADD};
                        uint32_t got = bz_word_of(en ? 1u << l : 0, bl, cf, NULL);
                        uint32_t want = on && en && m && fl ? ref_bits(bl[l]) << (DMSH_BLEND_ZERO_BITS * l) : 0;
                        EXPECT(got, want, "format %u slot %d blend %s src %d dst %d eq %d", m, l, en ? "on" : "off", s, d, eq);
                    }
                }
            }
        }
    }
    printf("  per-target, non-advanced: %llu cases (%d formats + none, 8 slots, blending on / off, 128 x 128 factors, 8 equations)\n", (unsigned long long)(cases - c0), g_nfm);
    // the formats the table does not give are no target the host binds: no word (the float names of Metal included)
    c0 = cases;
    for (uint32_t m = 0; m < 700; m++) {
        if (ref_is_float(m)) continue;
        uint32_t cf[8] = {m}; blend_st bl[8]; memset(bl, 0, sizeof bl);
        for (int s = 0; s < 128; s++) for (int d = 0; d < 128; d++) { bl[0] = (blend_st){.src_rgb = (uint64_t)s, .dst_rgb = (uint64_t)d, .eq_rgb = ADD}; EXPECT(bz_word_of(1, bl, cf, NULL), 0u, "format %u src %d dst %d", m, s, d); }
    }
    printf("  every other Metal pixel format value 0..699: %llu cases, word 0\n", (unsigned long long)(cases - c0));
    // ---- (a2) the advanced modes: mode x premultiplied x overlap x normalized dst x equation x factors (the state's own, which stand when the host cannot make the mode out of factors), on a float and a
    // normalized target, in the first and the last slot
    c0 = cases;
    {
        static const int named[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 18, 19, 97, 98, 99, 100, 127};
        const uint32_t fmts[2] = {MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm};
        cap_begin();   // a mode the host cannot make of factors writes a red line, once each: not echoed
        for (int f = 0; f < 2; f++) for (int li = 0; li < 2; li++) {
            int l = li ? 7 : 0; uint32_t cf[8] = {0}; cf[l] = fmts[f]; blend_st bl[8]; memset(bl, 0, sizeof bl);
            for (int adv = 0; adv < 64; adv++) for (int npm = 0; npm < 2; npm++) for (int ovl = 0; ovl < 4; ovl++) for (int fr = 0; fr < 2; fr++)
              for (unsigned si = 0; si < sizeof named / sizeof *named; si++) for (unsigned di = 0; di < sizeof named / sizeof *named; di++) for (int eq = 0; eq < 8; eq++) {
                bl[l] = (blend_st){.target = (uint64_t)l, .src_rgb = (uint64_t)named[si], .dst_rgb = (uint64_t)named[di], .eq_rgb = (uint64_t)eq, .adv = (uint64_t)adv, .adv_npm = (uint64_t)npm, .adv_ovl = (uint64_t)ovl, .adv_dst_free = (uint64_t)fr};
                uint32_t got = bz_word_of(1u << l, bl, cf, NULL);
                uint32_t want = on && f == 0 ? ref_bits(bl[l]) << (DMSH_BLEND_ZERO_BITS * l) : 0;
                EXPECT(got, want, "advanced mode %d npm %d ovl %d, src %d dst %d eq %d, format %u slot %d", adv, npm, ovl, named[si], named[di], eq, fmts[f], l);
            }
        }
        (void)cap_end(0);
    }
    printf("  per-target, advanced modes: %llu cases (64 modes x premultiplied x 4 overlaps x normalized dst x 21 x 21 factors x 8 equations, float / normalized, first / last slot)\n", (unsigned long long)(cases - c0));
    // ---- (a3) composition across targets: random 8-target states, with and without a merged pass's map: the full word is the reference's, and the OR of the targets' own words
    c0 = cases; uint64_t nonzero = 0, merged_n = 0, nz_off[2] = {0, 0};
    for (int it = 0; it < 3000000; it++) {
        uint32_t cf[8]; blend_st bl[8]; uint8_t lm[8]; int merged = rnd() & 1; uint32_t en = (uint32_t)rnd() & 0xff;
        for (int i = 0; i < 8; i++) { cf[i] = rand_fmt(); bl[i] = rand_blend(); lm[i] = pick(7) == 0 ? 0xff : (uint8_t)pick(8); }
        const uint8_t *lmp = merged ? lm : NULL;
        uint32_t got = bz_word_of(en, bl, cf, lmp), want = on ? ref_word(en, bl, cf, lmp) : 0, orw = 0;
        for (int i = 0; i < 8; i++) {   // one target alone: physical target i, with its own enable bit (logical output i when not merged, lm[i] when merged)
            uint32_t one[8] = {0}; one[i] = cf[i];
            orw |= bz_word_of(merged ? en : en & (1u << i), bl, one, lmp);
        }
        EXPECT(got, want, "composition: blend_en %#x merged %d", en, merged);
        EXPECT(got, orw, "composition (OR of the targets' own words): blend_en %#x merged %d", en, merged);
        nonzero += got != 0; merged_n += merged; nz_off[merged] += got != 0;
    }
    printf("  composition: %llu cases over 3000000 seeded random 8-target states (%llu merged; %llu / %llu of the plain / merged ones with a word != 0)\n", (unsigned long long)(cases - c0), (unsigned long long)merged_n, (unsigned long long)nz_off[0], (unsigned long long)nz_off[1]);
    if (on) CHECK(nonzero > 100000 && nz_off[1] > 10000);   // not vacuous

    // ---- (b) the key: pso_key_fill's key (and the merged path as pso_get_ completes it) against the reference of the state as the draw has it; the draw's own word (bz_word with the map it keeps)
    c0 = cases; uint64_t off_n = 0, live_n = 0, draw_checked = 0;
    {
        NvShader *vs = [NvShader new], *fs = [NvShader new]; vs->stage = 0; fs->stage = 1; snprintf(vs->key, sizeof vs->key, "vkey"); snprintf(fs->key, sizeof fs->key, "pkey");
        for (int it = 0; it < 600000; it++) {
            uint32_t cf[8]; uint8_t lm[8]; uint32_t en = (uint32_t)rnd() & 0xff; int merged = it & 1;
            memset(&S.color, 0, sizeof S.color); memset(S.blend, 0, sizeof S.blend); S.color.logic_op = NVN_LOGIC_OP_COPY;
            for (int i = 0; i < 8; i++) { cf[i] = rand_fmt(); S.blend[i] = rand_blend(); lm[i] = pick(7) == 0 ? 0xff : (uint8_t)pick(8); }
            S.color.blend_en = en; memcpy(g_pass.cfmt, cf, sizeof cf);
            uint32_t rw = on ? ref_word(en, S.blend, cf, merged ? lm : NULL) : 0;
            for (int hb = 0; hb < 2; hb++) {
                fs->has_bz = hb; pso_key k;
                if (merged) {   // as pso_get_ makes it for a merged pass's draw
                    g_pso_lmap = lm; pso_key_fill(&k, vs, fs, cf, 0, 0, 1, 1); g_pso_lmap = NULL;
                    CHECK(k.bz_off == 0);   // no map in the key yet: the guard stays
                    memcpy(k.lmap, lm, 8); k.bz_off = pso_key_bz_off(&k, fs, 1);
                } else pso_key_fill(&k, vs, fs, cf, 0, 0, 1, 0);
                EXPECT((uint32_t)k.bz_off, (uint32_t)(hb && rw == 0), "key bz_off, has_bz %d merged %d blend_en %#x word %#x", hb, merged, en, rw);
                {   // the draw's word, with the map the draw keeps (g_draw_lmap) for a merged pass: the key's flag is never set where it is not 0, and is where it is
                    uint32_t dw = bz_word(merged ? lm : NULL);
                    EXPECT(k.bz_off && dw != 0, 0, "INVARIANT: pipeline with bz_off drawn with word %#x, blend_en %#x, merged %d", dw, en, merged);
                    if (hb) EXPECT((uint32_t)k.bz_off, (uint32_t)(dw == 0), "key bz_off vs the draw's word %#x", dw);
                    draw_checked++;
                }
                if (hb) { off_n += k.bz_off; live_n += !k.bz_off; }
            }
        }
        fs->has_bz = 0;
    }
    memset(&S.color, 0, sizeof S.color); memset(S.blend, 0, sizeof S.blend); memset(g_pass.cfmt, 0, sizeof g_pass.cfmt);
    printf("  key: %llu cases over 600000 seeded random states (half merged; has_bz on / off); %llu of the has_bz keys compile the guard out, %llu keep it; %llu draws' words checked (the map the draw keeps for a merged one)\n",
           (unsigned long long)(cases - c0), (unsigned long long)off_n, (unsigned long long)live_n, (unsigned long long)draw_checked);
    if (on) CHECK(off_n > 10000 && live_n > 10000);

    // ---- the runtime check (bz_audit): the line, and its rate (the first three, then each power of two)
    {
        NvShader *fs = [NvShader new]; fs->stage = 1; fs->has_bz = 1; snprintf(fs->key, sizeof fs->key, "pforced");
        memset(&S.color, 0, sizeof S.color); memset(S.blend, 0, sizeof S.blend); memset(g_pass.cfmt, 0, sizeof g_pass.cfmt);
        S.color.blend_en = 1; S.blend[0] = (blend_st){.src_rgb = SA, .dst_rgb = ISA, .eq_rgb = ADD}; g_pass.cfmt[0] = MTLPixelFormatRGBA16Float;
        uint32_t w = bz_word(NULL);   // a float target with SRC_ALPHA: this draw needs the guard
        if (on) {
            CHECK(w == 1);
            cap_begin(); for (int i = 0; i < 9; i++) bz_audit(fs, w); NSString *s = cap_end(0);
            int n = count_of(s, @"[RED] blend-zero guard compiled out but this draw needs it: fragment pforced, word 0x1, blend_en 0x1, formats 115 0 0 0 0 0 0 0, out0 src 5 dst 6 eq 1 (");
            CHECK(n == 5);   // draws 1, 2, 3, 4 and 8 of the nine
            CHECK(count_of(s, @"[RED] blend-zero guard") == 5 && count_of(s, @"(8 draws so far)") == 1);
        }
        memset(&S.color, 0, sizeof S.color); memset(S.blend, 0, sizeof S.blend); memset(g_pass.cfmt, 0, sizeof g_pass.cfmt);
    }
    printf("blend zero rule (%s): %llu cases, %llu counterexamples; %d checks, %d failed\n", on ? "NVIDIA's rule" : "DM_NVN_BLEND_ZERO=0", (unsigned long long)cases, (unsigned long long)bad, checks, failed);
    return failed != 0 || bad != 0;
} }
