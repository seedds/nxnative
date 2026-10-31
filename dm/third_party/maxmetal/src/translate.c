// Maxwell (SM 5.x) shader binary -> Metal Shading Language, or Vulkan GLSL (key->glsl; contract in maxmetal.h).
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine with explicit SSY/PBK/PCNT stacks; NVN output (key->nvn) is
// structured if / while blocks where the stacks resolve statically (nvn_structure).
//
// GLSL: the instruction body is generated in the MSL spelling and converted by glsl_rewrite() (a token-aware
// pass: as_type<T>() -> as_u/as_f overloads, float4 -> vec4, ulong -> uint64_t, char()/short() casts -> sx8/sx16,
// in.x / out.x stage I/O -> GLSL globals and built-ins), helpers with MSL names (saturate, fmin, clz, mulhi, ...)
// are defined in the GLSL prelude, and the structurally different constructs (texture lookups, global memory,
// half-float packing, subgroup shuffles, barriers, discard, declarations and entry point) are emitted per dialect.
// The MSL text is byte-identical to the MSL-only translator (tests/test_translate.c checks it).
#if defined(_WIN32)
#include <malloc.h>  // alloca
#endif
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maxmetal/maxmetal.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;
uint32_t maxwell_gmem_chunk_shift;  // 0: one buffer for the whole heap (GLSL; maxmetal.h)
bool maxwell_gmem_lazy;
bool maxwell_gmem_fs_miss;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
const uint8_t *maxwell_dbg_vs;

#define RZ 255
#define LMEM_WORDS 256  // local memory per invocation (1 KB)
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

// The per-draw tint's recolour mode (ltint[0].w < 0): an HSV change of the
// output colour, in its gamma encoding: for colours whose hue is within ltint[1].y of ltint[1].x (>= 0.5: any) and
// saturation >= ltint[1].z, the hue goes to ltint[1].x + ltint[0].x + (its distance from ltint[1].x) * ltint[1].w
// (turns: .w = 1 a plain shift, < 1 also pulls the hues together), saturation * .y, value * .z (kept <= 1 unless it
// was above). The shading stays: only the colour moves. ltint[2] / [3] (ltint[2].w < 0): a second such change with
// its own hue window, made from the same colour (a draw blending two recoloured maps, e.g. grass and sand: each keeps
// its own; where both windows take a colour, the two results are averaged by weight). `c` is the colour target's
// expression ("c0" / "o.c0"), v3 / v4 the vector types ("vec3" / "float3").
static void emit_recolour_window(Str *s, const char *v3, const char *v4, int k, const char *r, const char *m) {
    // (rc_hh0 / rc_ss0 / rc_vv0: the colour's HSV) -> r: the changed colour (linear), m: its weight
    sp(s, "    float rc_sd%d = fract(rc_hh0 - ltint[%d].x + 0.5f) - 0.5f, rc_dh%d = abs(rc_sd%d);\n", k, k + 1, k, k);
    sp(s, "    float %s = (ltint[%d].y >= 0.5f ? 1.0f : 1.0f - smoothstep(ltint[%d].y * 0.6f, ltint[%d].y, rc_dh%d)) *\n"
          "              smoothstep(ltint[%d].z * 0.5f, ltint[%d].z + 1e-4f, rc_ss0);\n", m, k + 1, k + 1, k + 1, k, k + 1, k + 1);
    sp(s, "    float rc_hh%d = fract(ltint[%d].x + ltint[%d].x + rc_sd%d * ltint[%d].w), rc_ss%d = clamp(rc_ss0 * ltint[%d].y, 0.0f, 1.0f);\n",
       k + 1, k + 1, k, k, k + 1, k + 1, k);
    sp(s, "    float rc_vv%d = min(rc_vv0 * ltint[%d].z, max(rc_vv0, 1.0f));\n", k + 1, k);
    sp(s, "    %s rc_k%d = clamp(abs(fract(%s(rc_hh%d) + %s(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - %s(3.0f)) - %s(1.0f), "
          "%s(0.0f), %s(1.0f));\n", v3, k, v3, k + 1, v3, v3, v3, v3, v3);
    sp(s, "    %s %s = pow(rc_vv%d * mix(%s(1.0f), rc_k%d, %s(rc_ss%d)), %s(2.2f));\n", v3, r, k + 1, v3, k, v3, k + 1, v3);
    (void)v4;
}

static void emit_recolour(Str *s, const char *c, const char *v3, const char *v4) {
    sp(s, "  else if (ltint[0].w < 0.0f) {\n");
    sp(s, "    %s rc_g = pow(max(%s.rgb, %s(0.0f)), %s(1.0f / 2.2f));\n", v3, c, v3, v3);
    sp(s, "    %s rc_K = %s(0.0f, -1.0f / 3.0f, 2.0f / 3.0f, -1.0f);\n", v4, v4);
    sp(s, "    %s rc_p = mix(%s(rc_g.b, rc_g.g, rc_K.w, rc_K.z), %s(rc_g.g, rc_g.b, rc_K.x, rc_K.y), %s(step(rc_g.b, rc_g.g)));\n", v4, v4, v4, v4);
    sp(s, "    %s rc_q = mix(%s(rc_p.x, rc_p.y, rc_p.w, rc_g.r), %s(rc_g.r, rc_p.y, rc_p.z, rc_p.x), %s(step(rc_p.x, rc_g.r)));\n", v4, v4, v4, v4);
    sp(s, "    float rc_d = rc_q.x - min(rc_q.w, rc_q.y);\n");
    sp(s, "    float rc_hh0 = abs(rc_q.z + (rc_q.w - rc_q.y) / (6.0f * rc_d + 1e-6f)), rc_ss0 = rc_d / (rc_q.x + 1e-6f), rc_vv0 = rc_q.x;\n");
    emit_recolour_window(s, v3, v4, 0, "rc_r", "rc_m");
    sp(s, "    if (ltint[2].w < 0.0f) {\n");
    emit_recolour_window(s, v3, v4, 2, "rc_r2", "rc_m2");
    sp(s, "      float rc_w = rc_m + rc_m2;\n");
    sp(s, "      if (rc_w > 0.0f) { rc_r = (rc_r * rc_m + rc_r2 * rc_m2) / rc_w; rc_m = min(rc_w, 1.0f); }\n    }\n");
    sp(s, "    %s.rgb = mix(%s.rgb, rc_r, %s(rc_m));\n  }\n", c, c, v3);
}

// (emit_insn) a part of an XMAD multiply: a / breg the registers it read (breg RZ: B is b's text), av / bv their
// versions (Ctx.ver: a register's writes so far); xs: part 1's copies of a, b and c (xs<n>, xs<n+1>, xs<n+2>), made
// before it
typedef struct { uint8_t kind, a, breg; uint32_t av, bv; int xs; char b[80]; } XmPart;

typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane;
    bool uses_helper;            // (NVN fragment) SR_THREAD_KILL: whether the invocation is a helper, as on entry
    bool uses_point_coord;
    bool uses_rscale;  // resolution-scale table (fragment/compute only)
    uint8_t feat;      // key->features (MAXMETAL_FEAT_*); flip_x implies MAXMETAL_FEAT_RSCALE
    uint32_t flat_mask;           // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;  // fragment inputs using pass (linear) interpolation
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];           // texture index -> Metal slot + 1
    bool tex_weak[64];            // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;  // program uses the pc state machine
    int ipa_mode;  // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;   // its multiplier register (mode 1: the result is the "pass" value attr/w times this register)
    // GLSL (fields kept last: the MSL path's layout, including its out-of-range reg_used writes, is unchanged)
    bool glsl;               // emit Vulkan GLSL: dialect-specific constructs here, the rest via glsl_rewrite()
    bool uses_gmem_store;    // STG present: the gmem buffer is not readonly
    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)
    // (key->nvn) the NVN binding contract
    const NvnKey *nvn;
    const char *pfx;             // binding name prefix: vp / fp / cp
    uint8_t drv_read[2][0xA00 / 16];  // driver banks c0 / c2: vec4s read with a constant index
    bool drv_dyn[2];             // ... read with a computed index (the renderer cannot fill those)
    uint32_t sslot, sslot_store; // c0 storage-buffer slots whose address the code reads / that it stores through
    uint32_t uslot, uslot_store; // (compute) c0 uniform-buffer table slots, the same
    uint32_t shared_words;       // compute shared memory (32-bit words) the code addresses with constants
    bool shared_dyn, uses_shared;
    uint8_t tex_nvn[64];         // NVN texture slot of each texture
    uint8_t wstate[256];         // (fragment) the register holds gl_FragCoord.w (1) / 1 / gl_FragCoord.w (2): the
                                 // value as last written in this block (Ryujinx EliminateMultiplyByFragmentCoordW),
                                 // or on entry to it from every path (nvn_structure: swstate)
    uint32_t wdef[256];          // ... the pc of the IPA / MUFU.RCP that wrote it
    struct { uint8_t src, k; } qshf[256];  // (NVN fragment) the register holds SHFL.BFLY r<src>, k (1 / 2, c 0x1c03) as
                                 // written in this block, k 0: not (Ryujinx MatchDdxOrDdy); qnew: the one emit_insn wrote
    int qnew_d, qnew_src, qnew_k;
    // the register holds XMAD a.lo * b.lo + c (1) / XMAD.MRG a.lo * b.hi (2) as written in this block: the multiply
    // idioms emit_insn folds (XmPart)
    XmPart xm[256], xnew;        // xnew: the one emit_insn wrote (kind 0: none), xnew_d its register
    int xnew_d, nxs;
    uint32_t ver[256];
    bool structured;             // the body is structured control flow (nvn_structure), not the pc state machine
    // (NVN: nvn_gflow) while the blocks are emitted, each one's register writes and global memory accesses; gpred: the
    // predicate of the instruction being emitted (gflow_pred)
    bool gtrack, itrack;  // itrack: (state machine) the register writes and image ops only, for nvn_iflow
    struct GEv *gev;
    int ngev, cap_gev, gblk, ngacc, gpred;
    // (NVN) storage buffers whose address the code loads from a user uniform block, as Ryujinx's sbuf_ubo: constant buffer
    // xcb[i] (3-16), byte offset xoff[i] of the address's low word (nvn_gflow's origin bit 33 + i); xused: those an
    // access goes to
    int nx;
    uint8_t xcb[16];
    uint16_t xoff[16];
    uint32_t xused;
    // (NVN) bindless image loads / stores (SULD / SUST .B): each one's access id (its handle's origin: an image slot's
    // c2 entry, nvn_gflow bit 49 + slot) and image view (msl element type, format suffix); the views bound (slot,
    // suffix, type, read / written)
    struct { int id; const char *elem, *fmt; bool store; } iop[64];
    int niop;
    struct { int slot; const char *elem, *fmt; bool read, write; } img[16];
    int nimg;
} Ctx;

static bool nvn_on(const Ctx *c) { return c->nvn != NULL; }
// the resolution-scale table: MAXMETAL_FEAT_RSCALE; NVN output always reads it, as an identity (its prelude's rscale)
static bool rscale_on(const Ctx *c) { return c->nvn || (c->feat & MAXMETAL_FEAT_RSCALE); }
static bool tex_is_depth(const Ctx *c, int slot) { return nvn_on(c) && c->nvn->tex_depth[c->tex_nvn[slot]]; }

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

void (*maxwell_probe_fail)(const char *msg);

static void fail(Ctx *c, const char *fmt, ...) {
    if (maxwell_probe_fail) {  // (probe) report every failure and keep translating
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        maxwell_probe_fail(msg);
        return;
    }
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) { sprintf(b, "as_type<float>(%s)", e); return b; }

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (idx == 0) {  // a storage buffer binding: which slot (any, when indexed with a register)
        if (dyn) c->info->ssbo_read = 0xFFFF;
        else if (off >= 0x110 && off < 0x210) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x110) >> 4));
        else if (off >= 0x510 && off < 0x610) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x510) >> 4));
    }
    if (off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    if (nvn_on(c)) {
        // SPIRV-Cross's access form, which the renderer scans for the words it reads: a constant index is
        // "<pfx>_c<n>)._m0[<digits>]", a computed one starts with something else
        if (idx > 16) { fail(c, "constant buffer %d", idx); strcpy(buf, "0u"); return buf; }
        if (idx == 0 || idx == 2) {
            int bank = idx == 2;
            if (dyn || off < 0 || off >= 0xA00) c->drv_dyn[bank] = true;
            else c->drv_read[bank][off >> 4] = 1;
            if (idx == 0 && !dyn && c->stage == STAGE_COMPUTE && off >= 0x210 && off < 0x310) c->uslot |= 1u << ((off - 0x210) >> 4);
            else if (idx == 0 && !dyn) {
                int64_t base = c->stage == STAGE_VERTEX ? 0x110 : c->stage == STAGE_FRAGMENT ? 0x510 : 0x310;
                if (off >= base && off < base + 0x100) c->sslot |= 1u << ((off - base) >> 4);
            }
        }
        if (dyn)  // unchecked, as the Ryujinx path's: past the bank the GPU reads 0 there too (a guarded load is a branch,
                  // Metal does not speculate it; a clamp and a select, two more operations per load)
            sprintf(buf, "(*spvDescriptorSet0.%s_c%d)._m0[(((%s) + %lldu) >> 4u)][(((%s) + %lldu) >> 2u) & 3u]",
                    c->pfx, idx, dyn, (long long)off, dyn, (long long)off);
        else
            sprintf(buf, "(*spvDescriptorSet0.%s_c%d)._m0[%lldu][%lld]", c->pfx, idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
        return buf;
    }
    if (dyn) sprintf(buf, "cbr(cb%d, (%s) + %lldu)", idx, dyn, (long long)off);
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
    case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;  // sign-extend 20 bits
        sprintf(buf, "0x%08xu", v);
        return buf;
    }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// FP64 as Ryujinx's DoubleToFloat lowering for Metal (no doubles): a register pair / constant pair / immediate (the high
// word's top 20 bits) read as a float (maxmetal_d2f), a float result written as a register pair (maxmetal_f2d)
static const char *d64_pair(Ctx *c, int lo, char *buf) {
    char a[16], b[16];
    sprintf(buf, "as_type<float>(maxmetal_d2f(%s, %s))", reg(c, lo, a), reg(c, lo | 1, b));
    return buf;
}
static const char *d64_b(Ctx *c, uint64_t w, char *buf) {
    char lo[512], hi[512];
    int slot = (int)bits(w, 34, 5); int64_t off = bits(w, 20, 14) * 4;
    switch (b_form(w)) {
    case B_REG: return d64_pair(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: sprintf(buf, "as_type<float>(maxmetal_d2f(%s, %s))", cbuf_ref(c, slot, off, NULL, lo), cbuf_ref(c, slot, off + 4, NULL, hi)); break;
    default: sprintf(buf, "as_type<float>(maxmetal_d2f(0u, 0x%08xu))", ((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19)) << 12);
    }
    return buf;
}
static void d64_set(Ctx *c, int d, const char *f) {
    sp(&c->body, "    { uint2 dv = maxmetal_f2d(as_type<uint>(%s));\n", f);
    if (d != RZ) { c->reg_used[d] = c->reg_used[d | 1] = 1; c->wstate[d] = c->wstate[d | 1] = 0;
                   sp(&c->body, "      r%d = dv.x; r%d = dv.y;\n", d, d | 1); }
    sp(&c->body, "    }\n");
}

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

// video ops (VMNMX / VMAD / VSETP): A and B selected and extended as Ryujinx's InstEmitAluHelper.Extend (U8B0..3 / U16H0..1
// zero-, S8B0..3 / S16H0..1 sign-extended, U32 / S32 as they are; select bits 36-38 / 28-30, signs 48 / 49); B an
// immediate (bits 20-35, sign-extended when B's select is signed) without .BVIDEO (bit 50)
static void video_sel(int sel, const char *src, char *out) {
    int s = sel & 7;
    bool sg = sel & 8;
    if (s < 4) sprintf(out, sg ? "uint(int(%s << %du) >> 24)" : "((%s >> %du) & 0xFFu)", src, sg ? 24 - 8 * s : 8 * s);
    else if (s < 6) sprintf(out, sg ? "uint(int(%s << %du) >> 16)" : "((%s >> %du) & 0xFFFFu)", src, sg ? 16 - 16 * (s - 4) : 16 * (s - 4));
    else sprintf(out, "%s", src);
}
static void video_ab(Ctx *c, uint64_t w, char *ea, char *eb) {
    char ra[16], rb[16];
    int bsel = (int)(bits(w, 28, 3) | (bits(w, 49, 1) << 3));
    video_sel((int)(bits(w, 36, 3) | (bits(w, 48, 1) << 3)), reg(c, (int)bits(w, 8, 8), ra), ea);
    if (bits(w, 50, 1)) video_sel(bsel, reg(c, (int)bits(w, 20, 8), rb), eb);
    else sprintf(eb, "0x%08xu", (uint32_t)((bsel & 8) ? (int)(int16_t)bits(w, 20, 16) : (int)bits(w, 20, 16)));
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    c->wstate[r] = 0;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

static const char *fresult(const char *e, bool sat, char *buf) {
    if (sat) sprintf(buf, "as_type<uint>(saturate(%s))", e);
    else sprintf(buf, "as_type<uint>(%s)", e);
    return buf;
}
#define FRES(e, sat) fresult((e), (sat), alloca(700))

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan",
                                      "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 15: strcpy(out, "true"); break;
    case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
    case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
    // unordered: the ordered complement's negation (`!=` is unordered already), one compare where `|| isnan(a) ||
    // isnan(b)` is three on the GPU
    case 9: sprintf(out, "(!(%s >= %s))", a, b); break;
    case 10: sprintf(out, "(!((%s < %s) || (%s > %s)))", a, b, a, b); break;
    case 11: sprintf(out, "(!(%s > %s))", a, b); break;
    case 12: sprintf(out, "(!(%s <= %s))", a, b); break;
    case 13: sprintf(out, "(%s != %s)", a, b); break;
    case 14: sprintf(out, "(!(%s < %s))", a, b); break;
    default: sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// Condition-code test (Ryujinx Condition: zero / sign flags; the overflow flag is not kept, the unordered forms test
// as the ordered ones)
static const char *cc_test(Ctx *c, int test) {
    c->uses_cc = true;
    switch (test) {
    case 0: return "false";
    case 15: return "true";
    case 2: case 10: return "ccz";
    case 5: case 13: return "!ccz";
    case 1: case 9: return "ccs";
    case 6: case 14: return "!ccs";
    case 4: case 12: return "(!ccz && !ccs)";
    case 3: case 11: return "(ccz || ccs)";
    default: fail(c, "cc test %d", test); return "false";
    }
}

// .X integer compare: the high words, with the low words' result in CF / ZF (Ryujinx GetIntComparisonExtended)
static void icmp_ext(Ctx *c, char *out, int op, const char *a, const char *b, bool is_signed) {
    char lt[300], gt[300], eq[300];
    c->uses_cc = true;
    sprintf(lt, is_signed ? "(int(%s) < int(%s))" : "(%s < %s)", a, b);
    sprintf(gt, is_signed ? "(int(%s) > int(%s))" : "(%s > %s)", a, b);
    sprintf(eq, "(%s == %s)", a, b);
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 7: strcpy(out, "true"); break;
    case 1: sprintf(out, "(%s || (%s && cc == 0u))", lt, eq); break;
    case 2: sprintf(out, "(%s && ccz)", eq); break;
    case 3: sprintf(out, "(%s || (%s && (ccz || cc == 0u)))", lt, eq); break;
    case 4: sprintf(out, "(%s || (%s && !ccz && cc != 0u))", gt, eq); break;
    case 5: sprintf(out, "(!%s || !ccz)", eq); break;
    default: sprintf(out, "(%s || (%s && cc != 0u))", gt, eq); break;
    }
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (idx) { fail(c, "indexed attribute load"); return; }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind == 0) sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) { c->uses_iid = true; strcpy(e, "iid"); }
        else if (addr == 0x2FC) { c->uses_vid = true; strcpy(e, "vid"); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp == 0 && c->key && c->key->flip_x) {  // mirror mode's composite: x mirrored (rscale[65].x = width)
                c->uses_rscale = true;
                strcpy(e, "as_type<uint>(rscale[65][0] - (in.pos[0] - rscale[0][2]) / rscale[0][0])");
            } else if (comp < 2 && rscale_on(c)) {  // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && (c->key->vs_out_mask || c->key->glsl) && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");  // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr. "multiply" is the
                // pass value times a register - usually w (from the pass of position w: Metal's attr again), but a
                // shader may pass its own (the Mysterious Woods mist: 1/(v4.w * pos.w), a projective screen lookup)
                if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->nvn && c->ipa_mul != RZ && c->wstate[c->ipa_mul] == 2 && !(c->noperspective_mask & (1u << a)))
                    sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);  // (NVN) x * w * (1 / w) is x, as Ryujinx folds it
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char mb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                } else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) { c->uses_front_facing = true; strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)"); }
        else if (addr == 0x2E0 || addr == 0x2E4) { c->uses_point_coord = true; sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->glsl && c->stage != STAGE_VERTEX) {  // no vertex outputs in this stage (MSL: undeclared `out`)
        if (addr >= 0x80 && addr < 0x280) c->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
        if (addr == 0x6C) c->info->writes_point_size = true;
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->info->writes_point_size = true;
        sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    (void)c;
    char t[300];
    sprintf(t, "hx(%s, %du)", v, swz);
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char t[2048], e[1700];
    if (sat) sprintf(e, "saturate(%s)", expr); else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;  // 0 add, 1 mul
    int form = -1;  // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }  // HMUL2 reg: the negate bit is A's at 31
                bv = R((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1); swb = 0;
                sprintf(ib, "0x%08xu", himm(w)); bv = ib;
            }
        }
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)", HS(R(a), swa, nega, absa), HS(bv, swb, negb, absb));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;  // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;  // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;  // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;  // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {  // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            break;
        case 1:  // b = cbuf, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            break;
        case 2:  // b = reg (field at 39), c = cbuf
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
            break;
        case 3:  // b = imm, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            sprintf(ib, "0x%08xu", himm(w)); bv = ib; cv = R((int)bits(w, 39, 8));
            break;
        default:  // 32I: c = d
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = R(d);
            break;
        }
        sprintf(t, "fma(%s, %s, %s)", HS(R(a), swa, false, false), HS(bv, swb, negb, false), HS(cv, swc, negc, false));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (reg 5d20 / 5d18, imm 7e0x / 7c0x, cbuf 7e8x / 7c8x: fields per Ryujinx's InstHset(p)2*)
    form = (op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18 ? 0 : (op & 0xfc80) == 0x7c00 ? 1 : (op & 0xfc80) == 0x7c80 ? 2 : -1;
    if (form >= 0) {
        bool setp = form ? (op & 0xfe00) == 0x7e00 : (op & 0xfff8) == 0x5d20;
        const char *x = HS(R(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1)), *y;
        if (form == 0) y = HS(R((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        else if (form == 1) { sprintf(ib, "0x%08xu", himm(w)); y = HS(ib, 0, false, false); }
        else y = HS(cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb), 1, bits(w, 56, 1), setp && bits(w, 54, 1));
        int cop = (int)bits(w, form ? 49 : 35, 4), bop = (int)bits(w, 45, 2);
        bool flag = bits(w, form ? 53 : 49, 1);  // HSETP2 .H_AND / HSET2 .BF
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            if (flag) sp(&c->body, "      l = l && h; h = !l;\n");  // .H_AND: the halves ANDed, then each with the predicate (Ryujinx EmitHsetp2)
            if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop)); }
            if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop)); }
        } else {
            bool bf = flag;
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n"
                                : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n", d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            c->tex_weak[s] = false;
        } else if (c->glsl && type >= 0 && (c->info->tex[s].type != type || c->info->tex[s].shadow != shadow)) {
            fail(c, "texture %u used as two types", index);  // one GLSL sampler type per binding
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    if (nvn_on(c)) {
        // NVN texture handles in driver bank c2: BindTexture's combined table at 0x20 + 8 * slot (the game's), separate
        // textures at 0x168 + 8 * slot (sampled with a separate sampler: not translated here)
        uint32_t byte = index * 4;
        if (byte >= 0x20 && byte < 0x120 && (byte - 0x20) % 8 == 0) c->tex_nvn[s] = (uint8_t)((byte - 0x20) / 8);
        else { fail(c, "texture handle at c2+%#x", byte); return 0; }
        int8_t rq = c->nvn->tex_type[c->tex_nvn[s]];
        // a lookup that names no type (a size query first): the request's answer, as Ryujinx's QuerySamplerType, until a
        // lookup that names one (the slot stays weak: still weak at the end, the translation depended on the answer)
        if (weak && rq >= 0) { type = rq; array = rq == TEXTYPE_2D_ARRAY || rq == TEXTYPE_CUBE_ARRAY || rq == TEXTYPE_1D_ARRAY; }
    }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    return s;
}

static const char *tex_type_name(int t) {
    switch (t) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_2D_ARRAY: return "2d_array";
    case TEXTYPE_CUBE_ARRAY: return "cube_array";
    case TEXTYPE_1D_ARRAY: return "1d_array";
    default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    char dcall[2600];
    if (tex_is_depth(c, slot) && !strstr(call, "_compare(")) {
        // (NVN) a depth-format texture read without a compare: Metal's depth texture gives the depth alone, NVN's fetch
        // (d, 0, 0, 1); the game binds a trilinear sampler, but on the Switch the read lands on one texel, so point sampling
        // (as the Ryujinx path's depth sampling). A depth gather has only the depth component.
        char pre[16], ps[16];
        snprintf(pre, sizeof pre, "t%d.", slot);
        snprintf(ps, sizeof ps, "(s%d,", slot);
        const char *g = strstr(call, ".gather(");
        if (g) {
            const char *cm = strstr(call, ", component::");
            if (cm && cm[13] != 'x') { fail(c, "gather of a non-x component on a depth texture"); return; }
            snprintf(dcall, sizeof dcall, "%.*s)", cm ? (int)(cm - call) : (int)strlen(call) - 1, call);
        } else if (strncmp(call, pre, strlen(pre)) == 0 && strstr(call, ".sample(") == call + strlen(pre) - 1 &&
                   strncmp(call + strlen(pre) + 6, ps, strlen(ps)) == 0) {
            snprintf(dcall, sizeof dcall, "float4(t%d.sample(maxmetal_nearest,%s, 0.0f, 0.0f, 1.0f)", slot, call + strlen(pre) + 6 + strlen(ps));
        } else {
            snprintf(dcall, sizeof dcall, "float4(%s, 0.0f, 0.0f, 1.0f)", call);
        }
        call = dcall;
    }
    sp(&c->body, "    { float4 t = %s;\n", call);
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++) if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) { if (i < n) sprintf(v[i], "t[%d]", comps[i]); else strcpy(v[i], "0.0f"); }
        const char *pk = c->glsl ? "      r%d = packHalf2x16(vec2(%s, %s));\n" : "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) { c->reg_used[d0] = 1; sp(&c->body, pk, d0, v[0], v[1]); }
        if (d1 != RZ) { c->reg_used[d1] = 1; sp(&c->body, pk, d1, v[2], v[3]); }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = as_type<uint>(t[%d]);\n", r, comp); }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static char bufs[4][1024];
    static int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX || !rscale_on(c)) { snprintf(o, 1024, "%s", inner); return o; }
    c->uses_rscale = true;
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// (NVN) the key says the GPU clamps a sampling instruction's array layer as Maxwell does (NvnKey.hw_layer_clamp)
static bool hw_layer_clamp(const Ctx *c) { return c->nvn && c->nvn->hw_layer_clamp && !c->glsl; }

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    if (c->glsl) sprintf(buf, "min(%s, uint(tsize%d(0).z) - 1u)", r, slot);
    else if (hw_layer_clamp(c)) sprintf(buf, "%s", r);
    else sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// 2D normalized coordinates: a scaled texture whose contents occupy its top-left part (Dynamic resolution) is sampled
// there (prelude uvs(); rscale[1 + slot].zw). Vertex shaders have no rscale table, nor
// does any shader without MAXMETAL_FEAT_RSCALE: their lookups stay as they are.
static const char *uv2(Ctx *c, int slot, const char *x, const char *y, char *buf) {
    // (NVN) a NaN coordinate samples at 0, as the Ryujinx path (CodeGen/Spirv/Instructions.cs: pCoords)
    if (c->nvn) { snprintf(buf, 700, "maxmetal_nz(float2(%s, %s))", x, y); return buf; }
    if (c->stage == STAGE_VERTEX || !rscale_on(c)) { snprintf(buf, 700, "%s2(%s, %s)", c->glsl ? "vec" : "float", x, y); return buf; }
    c->uses_rscale = true;
    snprintf(buf, 700, "uvs(%s2(%s, %s), rscale[%d])", c->glsl ? "vec" : "float", x, y, slot + 1);
    return buf;
}
#define UV2(slot, x, y) uv2(c, (slot), (x), (y), alloca(700))

// 1D / 3D / cube coordinates, NaN components 0 (NVN, as uv2)
static const char *crd(Ctx *c, const char *vec, const char *x, const char *y, const char *z, char *buf) {
    char v[640];
    if (!y) snprintf(v, sizeof v, "%s", x); else snprintf(v, sizeof v, "%s(%s, %s, %s)", vec, x, y, z);
    if (c->nvn) snprintf(buf, 700, "maxmetal_nz(%s)", v); else snprintf(buf, 700, "%s", v);
    return buf;
}
#define C1(x) crd(c, NULL, (x), NULL, NULL, alloca(700))
#define C3(x, y, z) crd(c, "float3", (x), (y), (z), alloca(700))

// GLSL texture lookup on slot `slot` (its type is final here: a conflicting use fails in tex_slot).
// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
// GLSL offsets must be constants, so register offsets move the coordinates by offset / size of the level
// (level 0 for implicit-LOD lookups). Non-fragment stages have no implicit LOD: level 0 (+ bias).
static void glsl_sample(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref,
                        int lod_kind, const char *lod, const char *off) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }  // e.g. a type conflict: tex[slot] is not this lookup's type
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (c->stage != STAGE_FRAGMENT && lod_kind != 1) {
        if (lod_kind == 0) lod = "0.0f";
        lod_kind = 1;
    }
    if (t->shadow && type == TEXTYPE_3D) { fail(c, "3d shadow texture"); strcpy(out, "vec4(0.0f)"); return; }
    if (layer && type != TEXTYPE_1D_ARRAY && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE_ARRAY) {
        fail(c, "array lookup on a 3d texture");  // TEX .3D with the array bit
        strcpy(out, "vec4(0.0f)");
        return;
    }
    char L[600], xs[1600], P[2000];
    if (lod_kind == 1) snprintf(L, sizeof L, "int(%s)", lod); else strcpy(L, "0");
    if (n == 1) {
        if (off) snprintf(xs, sizeof xs, "(%s + float((%s).x) / float(tsize%d(%s).x))", co[0], off, slot, L);
        else snprintf(xs, sizeof xs, "%s", co[0]);
    } else if (n == 2) {
        if (off) snprintf(xs, sizeof xs, "(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy))", co[0], co[1], off, slot, L);
        else snprintf(xs, sizeof xs, "vec2(%s, %s)", co[0], co[1]);
        if (c->stage != STAGE_VERTEX && rscale_on(c)) { char w[1700]; snprintf(w, sizeof w, "uvs(%s, rscale[%d])", xs, slot + 1); strcpy(xs, w); c->uses_rscale = true; }
    } else {
        if (off && !cube) snprintf(xs, sizeof xs, "vec3(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy), %s)", co[0], co[1], off, slot, L, co[2]);
        else snprintf(xs, sizeof xs, "vec3(%s, %s, %s)", co[0], co[1], co[2]);  // cube lookups take no offset
    }
    const char *extra = NULL;  // separate compare argument (cube array shadow)
    if (!t->shadow) {
        if (layer) snprintf(P, sizeof P, "vec%d(%s, float(%s))", n + 1, xs, layer);
        else snprintf(P, sizeof P, "%s", xs);
    } else if (type == TEXTYPE_CUBE_ARRAY) {
        snprintf(P, sizeof P, "vec4(%s, float(%s))", xs, layer ? layer : "0u");
        extra = dref;
    } else if (type == TEXTYPE_1D) {
        snprintf(P, sizeof P, "vec3(%s, 0.0f, %s)", xs, dref);
    } else if (layer) {
        snprintf(P, sizeof P, "vec%d(%s, float(%s), %s)", n + 2, xs, layer, dref);
    } else {
        snprintf(P, sizeof P, "vec%d(%s, %s)", n + 1, xs, dref);
    }
    char e[2000], ex[300] = "";
    if (extra) snprintf(ex, sizeof ex, ", %s", extra);
    if (lod_kind == 0) snprintf(e, sizeof e, "texture(t%d, %s%s)", slot, P, ex);
    else if (lod_kind == 1) snprintf(e, sizeof e, "textureLod(t%d, %s%s, %s)", slot, P, ex, lod);
    else snprintf(e, sizeof e, "texture(t%d, %s%s, %s)", slot, P, ex, lod);
    if (t->shadow) sprintf(out, "vec4(%s)", e);
    else strcpy(out, e);
}

// TEXS / TLDS in GLSL (same slot allocation order and result layout as the MSL switch in emit_texs)
static bool glsl_texs(Ctx *c, uint64_t w, bool fetch, int *slot_out, char *call, const char *A, const char *A1,
                      const char *Bv, const char *B1) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int slot;
    const char *co[3];
    char fa[64], fa1[64], fb[64], fb1[64];
    sprintf(fa, "as_type<float>(%s)", A); sprintf(fa1, "as_type<float>(%s)", A1);
    sprintf(fb, "as_type<float>(%s)", Bv); sprintf(fb1, "as_type<float>(%s)", B1);
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false); co[0] = fa;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb, NULL); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 0, NULL, NULL); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb1, 1, fb, NULL); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 1, "0.0f", NULL); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 0, NULL, NULL); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 1, "0.0f", NULL); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, A, fb1, 1, "0.0f", NULL); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb1, NULL); break;
        default: fail(c, "texs info %d", info); return false;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), 0)", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), int(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "texelFetch(t%d, ivec3(%s, %s, %s), 0)", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s), int(%s)), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return false;
        }
    }
    *slot_out = slot;
    return true;
}

static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (c->glsl) {
        char gcall[4096];
        if (!glsl_texs(c, w, fetch, &slot, gcall, A, A1, Bv, B1)) return;
        write_tex_result(c, slot, gcall, mask, d0, d1, true, !bits(w, 59, 1));
        return;
    }
    // (NVN) outside fragment functions implicit derivatives are other lanes' (compute: a quad of lanes that may have
    // branched apart); as Ryujinx (no implicit LOD outside the fragment stage): level 0 (1D textures have one level)
    const char *L0 = nvn_on(c) && c->stage != STAGE_FRAGMENT ? ", level(0)" : "";
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, C1(F(A))); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s%s)", slot, slot, UV2(slot, F(A), F(Bv)), L0); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(0))", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(%s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s%s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv), L0); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(%s)))", slot, slot, UV2(slot, F(A), F(A1)), F(B1), F(Bv)); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(0)))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s%s)", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot), L0); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s, level(0))", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
            sprintf(call, nvn_on(c) ? "float4(t%d.sample_compare(s%d, %s, %s, maxmetal_d16ref(%s), level(0)))"
                                    : "float4(t%d.sample_compare(s%d, %s, %s, %s, level(0)))", slot, slot, UV2(slot, F(A1), F(Bv)), A, F(B1)); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, %s%s)", slot, slot, C3(F(A), F(A1), F(Bv)), L0); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(0))", slot, slot, C3(F(A), F(A1), F(Bv))); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, %s%s)", slot, slot, C3(F(A), F(A1), F(Bv)), L0); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(%s))", slot, slot, C3(F(A), F(A1), F(Bv)), F(B1)); break;
        default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s))", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D_MS, false, false);   // multisample: gpr20 the sample
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A1, Bv), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));  // 0xd0/0xd2: .F16 results
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20.
static void emit_tex(Ctx *c, uint64_t w, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = bindless ? 0 : (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);  // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    if (bindless) { fail(c, "bindless tex"); return; }
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
             : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
             : target == 2 ? TEXTYPE_3D
                           : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    if (c->glsl) {  // same register layout as below
        char cs[3][40], lay[96], lodv[48], offv[96], drv[48], gcall[4096];
        const char *co[3], *layp = NULL, *lodp = NULL, *offp = NULL, *drp = NULL;
        int ar2 = a, p2 = b, lk = 0;
        if (array) { sprintf(lay, "min(uint(r%d), uint(tsize%d(0).z) - 1u)", a, slot); c->reg_used[a] = 1; ar2 = a + 1; layp = lay; }
        for (int i = 0; i < ncoord; i++) { sprintf(cs[i], "as_type<float>(r%d)", ar2 + i); c->reg_used[ar2 + i] = 1; co[i] = cs[i]; }
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            c->reg_used[p2] = 1;
            sprintf(lodv, "as_type<float>(r%d)", p2);
            lodp = lodv;
            lk = lod_mode == 2 || lod_mode == 6 ? 2 : 1;
            p2++;
        } else if (lod_mode == 1) {
            lk = 1;
            lodp = "0.0f";
        }
        if (aoffi) { c->reg_used[p2] = 1; sprintf(offv, "int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", p2, p2); offp = offv; p2++; }
        if (dc) { c->reg_used[p2] = 1; sprintf(drv, "as_type<float>(r%d)", p2); drp = drv; p2++; }
        glsl_sample(c, gcall, slot, co, layp, drp, lk, lodp, offp);
        write_tex_result(c, slot, gcall, mask, d, RZ, false, false);
        return;
    }
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) {
        if (hw_layer_clamp(c)) sprintf(layer, "uint(r%d)", a);
        else sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot);
        c->reg_used[a] = 1; ar = a + 1;
    }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = b;
    char lod[96] = "", dref[64] = "", off[128] = "";
    // (as emit_texs' L0; a bias is the level)
    bool nofrag = nvn_on(c) && c->stage != STAGE_FRAGMENT && type != TEXTYPE_1D && type != TEXTYPE_1D_ARRAY;
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, (lod_mode == 2 || lod_mode == 6) && !nofrag ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1 || nofrag) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        if (type == TEXTYPE_3D)
            sprintf(off, ", int3(int(r%d << 28) >> 28, int(r%d << 24) >> 28, int(r%d << 20) >> 28)", pr, pr, pr);
        else if (type != TEXTYPE_CUBE && type != TEXTYPE_CUBE_ARRAY)  // (Metal takes no offset for cube lookups)
            sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) {
        c->reg_used[pr] = 1;
        // (NVN) Maxwell rounds a 2D-array compare's reference to the 16-bit depth first (cascaded shadow maps; as the
        // Ryujinx path's depth sampling, which rounds it in compares at an explicit level only)
        sprintf(dref, nvn_on(c) && type == TEXTYPE_2D_ARRAY && strstr(lod, "level(") ? "maxmetal_d16ref(as_type<float>(r%d))"
                                                                                      : "as_type<float>(r%d)", pr);
        pr++;
    }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char cexpr[420], call[1024];
    if (ncoord == 2 && c->stage != STAGE_VERTEX && !c->nvn && rscale_on(c)) { snprintf(cexpr, sizeof cexpr, "uvs(float2(%s), rscale[%d])", coords, slot + 1); c->uses_rscale = true; }
    else snprintf(cexpr, sizeof cexpr, "%s(%s)", vec, coords);
    if (c->nvn) { char nz[440]; snprintf(nz, sizeof nz, "maxmetal_nz(%s)", cexpr); strcpy(cexpr, nz); }
    if (dc)
        sprintf(call, "float4(t%d.sample_compare(s%d, %s%s%s, %s%s%s))", slot, slot, cexpr, array ? ", " : "",
                layer, dref, lod, off);
    else
        sprintf(call, "t%d.sample(s%d, %s%s%s%s%s)", slot, slot, cexpr, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// (NVN) Global memory goes through the storage buffers that the c0 table names (LDG / STG / RED). The renderer binds
// slot k's buffer at the address of its c0 entry {address, size}. An access goes to the slot whose range holds its
// address, among every slot the code reads an address from; Ryujinx's GlobalToStorage pass finds the same slots by
// data flow. An address in none of them loads 0 and stores nothing. MAXMETAL_GFIND (nvn_finish) is that search: it sets
// gp (the slot's words), go (the byte offset in it) and gok (found).
// The code (structured, or without branches) tracks the slots by data flow too (nvn_gflow): an access whose address's
// high word can only come from one slot's c0 entry goes there, its offset the low words' difference (MAXMETAL_GAT), as
// Ryujinx's single-target access (constant offsets fold to constant indices), unchecked as that (gok true): a test of
// the slot's size made small compute dispatches 1.2x the Ryujinx path's GPU time. From a few slots it tests
// just those (MAXMETAL_GIN), as Ryujinx's multi-target access; the rest search every slot (MAXMETAL_GFIND). Until the flow is
// known the access is MAXMETAL_GACC(ga, <its number>).

// (nvn_gflow) in a block: a register write (kind 0: the register's origin is mask, or src's when src >= 0; cond: it
// may not happen; pred >= 0: it happens when that predicate holds), a global memory access (kind 1: id, through the
// address whose high word is reg, under predicate pred) or a predicate write (kind 2: reg)
typedef struct GEv { int blk, kind, reg, src, id, pred; bool cond; uint64_t mask; } GEv;
#define GF_UNKNOWN (1ull << 32)  // origin: anything else (bits 0-15: storage slot k's c0 entry, 16-31: uniform slot k's)

static void gev_push(Ctx *c, GEv e) {
    if (c->ngev == c->cap_gev) c->gev = realloc(c->gev, (c->cap_gev = c->cap_gev ? 2 * c->cap_gev : 256) * sizeof *c->gev);
    c->gev[c->ngev++] = e;
}

// kind: 0 load, 1 store, 2 reduction, 3 atomic (redop: Ryujinx AtomOp, redsize: AtomSize; rv: an atomic's value).
// d: destination / data register.
static void nvn_global(Ctx *c, const char *addr, int kind, int size, int d, int redop, int redsize, int rv) {
    int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
    c->info->uses_gmem = true;
    int hi;
    if (c->gtrack && sscanf(addr, "((ulong(r%d) << 32", &hi) == 1) {
        gev_push(c, (GEv){.blk = c->gblk, .kind = 1, .reg = hi, .id = c->ngacc, .pred = c->gpred});
        sp(&c->body, "    { ulong ga = %s; MAXMETAL_GACC(ga, %d)\n", addr, c->ngacc++);
    } else {
        sp(&c->body, "    { ulong ga = %s; MAXMETAL_GFIND(ga)\n", addr);
    }
    if (kind == 0) {
        static const char *const small[] = {
            "(gp[go >> 2] >> ((go & 3u) * 8u)) & 0xFFu", "uint(int(gp[go >> 2] << (24u - (go & 3u) * 8u)) >> 24)",
            "(gp[go >> 2] >> ((go & 2u) * 8u)) & 0xFFFFu", "uint(int(gp[go >> 2] << (16u - (go & 2u) * 8u)) >> 16)"};
        if (size < 4) {
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = gok ? %s : 0u;\n", d, small[size]); }
        } else {
            for (int k = 0; k < nw; k++)
                if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = gok ? gp[(go >> 2) + %du] : 0u;\n", d + k, k); }
        }
    } else if (kind == 1) {
        c->info->uses_gmem_store = true;
        if (size < 4) {
            uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
            sp(&c->body, "      if (gok) { uint sh = (go & %s) * 8u; device atomic_uint *gw = (device atomic_uint *)&gp[go >> 2];\n"
                         "        atomic_fetch_and_explicit(gw, ~(0x%xu << sh), memory_order_relaxed); "
                         "atomic_fetch_or_explicit(gw, (%s & 0x%xu) << sh, memory_order_relaxed); }\n",
               size < 2 ? "3u" : "2u", m, R(d), m);
        } else {
            for (int k = 0; k < nw; k++) sp(&c->body, "      if (gok) gp[(go >> 2) + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
        }
    } else {
        c->info->uses_gmem_store = true;
        static const char *const fn[] = {"fetch_add", "fetch_min", "fetch_max", NULL, NULL, "fetch_and", "fetch_or", "fetch_xor", "exchange"};
        bool sgn = redsize == 1;
        if ((redsize != 0 && redsize != 1) || redop > 8 || !fn[redop]) {
            // Ryujinx EmitAtomicOp: an operation / size it has not (INC, DEC, SAFEADD, 64-bit, float) does nothing, the
            // result 0
            if (kind == 3 && d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = 0u;\n", d); }
        } else if (kind == 2) {
            sp(&c->body, "      if (gok) atomic_%s_explicit((device atomic_%s *)&gp[go >> 2], %s(%s), memory_order_relaxed);\n",
               fn[redop], sgn ? "int" : "uint", sgn ? "int" : "uint", R(d));
        } else {
            sp(&c->body, "      uint ao = gok ? uint(atomic_%s_explicit((device atomic_%s *)&gp[go >> 2], %s(%s), memory_order_relaxed)) : 0u;\n",
               fn[redop], sgn ? "int" : "uint", sgn ? "int" : "uint", R(rv));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ao;\n", d); }
        }
    }
    sp(&c->body, "    }\n");
}

// (NVN) LDS / STS: compute shared memory as 32-bit words, the address in bytes. 1024 words, as Ryujinx's translation of the
// same shaders declares, or as many as constant addresses reach; computed addresses are clamped into it.
static void nvn_shared(Ctx *c, uint64_t w, bool store) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), size = (int)bits(w, 48, 3);
    int32_t imm = (int32_t)sbits(w, 20, 24);
    int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
    if (c->stage != STAGE_COMPUTE) return;  // (Ryujinx Lds / Sts: not valid there, nothing done)
    c->uses_shared = true;
    if (a != RZ) c->shared_dyn = true;
    else if (imm >= 0 && (uint32_t)imm / 4 + nw > c->shared_words) c->shared_words = (uint32_t)imm / 4 + nw;
    sp(&c->body, "    { uint sa = %s + 0x%xu;\n", R(a), (uint32_t)imm);
    if (!store) {
        static const char *const small[] = {
            "(shm[MAXMETAL_SHW(sa)] >> ((sa & 3u) * 8u)) & 0xFFu", "uint(int(shm[MAXMETAL_SHW(sa)] << (24u - (sa & 3u) * 8u)) >> 24)",
            "(shm[MAXMETAL_SHW(sa)] >> ((sa & 2u) * 8u)) & 0xFFFFu", "uint(int(shm[MAXMETAL_SHW(sa)] << (16u - (sa & 2u) * 8u)) >> 16)"};
        if (size < 4) {
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = %s;\n", d, small[size]); }
        } else {
            for (int k = 0; k < nw; k++)
                if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = shm[MAXMETAL_SHW(sa + %du)];\n", d + k, k * 4); }
        }
    } else if (size < 4) {
        uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
        // (atomically: other threads may store the other bytes of the word)
        sp(&c->body, "      { uint sh = (sa & %s) * 8u; threadgroup atomic_uint *sw = (threadgroup atomic_uint *)&shm[MAXMETAL_SHW(sa)];\n"
                     "        atomic_fetch_and_explicit(sw, ~(0x%xu << sh), memory_order_relaxed); "
                     "atomic_fetch_or_explicit(sw, (%s & 0x%xu) << sh, memory_order_relaxed); }\n",
           size < 2 ? "3u" : "2u", m, R(d), m);
    } else {
        for (int k = 0; k < nw; k++) sp(&c->body, "      shm[MAXMETAL_SHW(sa + %du)] = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
    }
    sp(&c->body, "    }\n");
}

// (NVN) ATOMS (Ryujinx InstEmitMemory.Atoms): a 32-bit atomic on compute shared memory at word (a >> 2) + the 22-bit
// immediate (in words, as Ryujinx adds it); operations / sizes Ryujinx has not give 0, outside compute nothing happens
static void nvn_shared_atom(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), op = (int)bits(w, 52, 4), size = (int)bits(w, 28, 2);
    int32_t imm = (int32_t)((uint32_t)bits(w, 30, 22) << 10) >> 10;
    static const char *const fn[] = {"fetch_add", "fetch_min", "fetch_max", NULL, NULL, "fetch_and", "fetch_or", "fetch_xor", "exchange"};
    if (c->stage != STAGE_COMPUTE) return;
    if (size > 1 || op > 8 || !fn[op]) { set_reg(c, d, "0u"); return; }
    c->uses_shared = true;
    c->shared_dyn |= a != RZ;
    if (a == RZ && imm >= 0 && (uint32_t)imm + 1 > c->shared_words) c->shared_words = (uint32_t)imm + 1;
    sp(&c->body, "    { uint ao = uint(atomic_%s_explicit((threadgroup atomic_%s *)&shm[MAXMETAL_SHW(%s + 0x%xu)], %s(%s), memory_order_relaxed));\n",
       fn[op], size ? "int" : "uint", R(a), (uint32_t)imm * 4u, size ? "int" : "uint", R((int)bits(w, 20, 8)));
    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ao;\n", d); }
    sp(&c->body, "    }\n");
}

// TLD4 / TLD4S / TMML follow Ryujinx's register layouts (InstEmitTexture.cs: EmitTld4, EmitTexs Tld4s, EmitTmml).
// A register past RZ reads as 0, like Ryujinx's Ra() / Rb().
static const char *reg_or_zero(Ctx *c, int r, char *buf) { return reg(c, r > RZ ? RZ : r, buf); }
#define RZ0(r) reg_or_zero(c, (r), alloca(16))

// The gather of slot `slot` with 2D coordinates x, y (and layer, for arrays): dref NULL gathers component `comp`, else
// compares; off: int2 expression or NULL.
static void gather_call(Ctx *c, char *call, int slot, int type, const char *x, const char *y, const char *z,
                        const char *layer, const char *dref, int comp, const char *off) {
    static const char *const cn[] = {"x", "y", "z", "w"};
    char co[800], ex[200] = "";
    if (type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY) snprintf(co, sizeof co, "%s", C3(x, y, z));
    else snprintf(co, sizeof co, "%s", UV2(slot, x, y));
    if (layer) snprintf(ex, sizeof ex, ", %s", layer);
    if (dref)
        sprintf(call, "t%d.gather_compare(s%d, %s%s, %s%s%s)", slot, slot, co, ex, dref, off ? ", " : "", off ? off : "");
    else if (type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY)
        sprintf(call, "t%d.gather(s%d, %s%s, component::%s)", slot, slot, co, ex, cn[comp]);
    else
        sprintf(call, "t%d.gather(s%d, %s%s, %s, component::%s)", slot, slot, co, ex, off ? off : "int2(0)", cn[comp]);
}

static int texdim_type(Ctx *c, int dim, bool *array) {
    *array = dim & 1;
    switch (dim >> 1) {
    case 1: return *array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D;
    case 3: return *array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE;
    default: fail(c, "tex dim %d", dim); return -1;
    }
}

static void emit_tld4(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    if (c->glsl) { fail(c, "tld4 (glsl)"); return; }
    if (d == RZ) return;
    int toff = (int)bits(w, 54, 2), comp = (int)bits(w, 56, 2);
    bool dc = bits(w, 50, 1), array;
    int type = texdim_type(c, (int)bits(w, 28, 3), &array);
    if (type < 0) return;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    if (toff == 2 && cube) { fail(c, "tld4 ptp cube"); return; }
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), type, dc, array);
    char layer[96] = "", off[160] = "", dref[64] = "";
    if (array && hw_layer_clamp(c)) sprintf(layer, "uint(%s)", RZ0(a++));
    else if (array) sprintf(layer, "min(uint(%s), t%d.get_array_size() - 1u)", RZ0(a++), slot);
    char x[48], y[48], z[48];
    sprintf(x, "as_type<float>(%s)", RZ0(a)); sprintf(y, "as_type<float>(%s)", RZ0(a + 1));
    sprintf(z, "as_type<float>(%s)", RZ0(a + 2));
    if (toff == 1 && !cube) {
        const char *o = RZ0(b++);
        sprintf(off, "int2(int(%s << 26) >> 26, int(%s << 18) >> 26)", o, o);
    }
    // PTP: an offset per texel, 6-bit bytes of two registers (texel i: bytes 2i, 2i + 1); as SPIRV-Cross's
    // spvGatherConstOffsets for Ryujinx, a gather at each offset, its texel w (the offset's own)
    const char *po[2] = {NULL, NULL};
    if (toff == 2) { po[0] = RZ0(b++); po[1] = RZ0(b++); }
    if (dc) sprintf(dref, "as_type<float>(%s)", RZ0(b++));
    char call[2048];
    if (toff == 2) {
        char *o = call;
        o += sprintf(o, "float4(");
        for (int i = 0; i < 4; i++) {
            char oi[160], gi[600];
            const char *p0 = po[(2 * i) >> 2], *p1 = po[(2 * i + 1) >> 2];
            sprintf(oi, "int2(int(%s << %d) >> 26, int(%s << %d) >> 26)", p0, 26 - 8 * ((2 * i) & 3), p1, 26 - 8 * ((2 * i + 1) & 3));
            gather_call(c, gi, slot, type, x, y, z, array ? layer : NULL, dc ? dref : NULL, comp, oi);
            o += sprintf(o, "%s%s.w", i ? ", " : "", gi);
        }
        strcpy(o, ")");
    } else
        gather_call(c, call, slot, type, x, y, z, array ? layer : NULL, dc ? dref : NULL, comp, toff == 1 && !cube ? off : NULL);
    write_tex_result(c, slot, call, (uint32_t)bits(w, 31, 4), d, RZ, false, false);
}

static void emit_tld4s(Ctx *c, uint64_t w, bool f16) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    if (c->glsl) { fail(c, "tld4s (glsl)"); return; }
    bool aoffi = bits(w, 51, 1), dc = bits(w, 50, 1);
    int comp = (int)bits(w, 52, 2);
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), TEXTYPE_2D, dc, false);
    char x[48], y[48], off[160] = "", dref[64] = "";
    sprintf(x, "as_type<float>(%s)", RZ0(a));
    if (!(dc || aoffi)) sprintf(y, "as_type<float>(%s)", RZ0(b));
    else sprintf(y, "as_type<float>(%s)", RZ0(a + 1));
    if (aoffi) {
        const char *o = RZ0(b++);
        sprintf(off, "int2(int(%s << 26) >> 26, int(%s << 18) >> 26)", o, o);
    }
    if (dc) sprintf(dref, "as_type<float>(%s)", RZ0(b));
    char call[2048];
    gather_call(c, call, slot, TEXTYPE_2D, x, y, NULL, NULL, dc ? dref : NULL, comp, aoffi ? off : NULL);
    write_tex_result(c, slot, call, d1 == RZ ? 0x3u : 0xfu, d0, d1, true, f16);
}

// TXD (Ryujinx EmitTxd): the coordinates, then a register whose low 16 bits are the layer and bits 16 + 4i offset i
// (TOFF), from gpr8; dPdx then dPdy from gpr20. A sample with gradients; no 1D (Metal has no 1D gradients).
static void emit_txd(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    if (c->glsl) { fail(c, "txd (glsl)"); return; }
    if (d == RZ) return;
    int dim = (int)bits(w, 28, 3);
    bool array = dim & 1, toff = bits(w, 35, 1);
    if (dim < 2 || dim == 5) { fail(c, "txd dim %d", dim); return; }
    int type = dim == 2 ? TEXTYPE_2D : dim == 3 ? TEXTYPE_2D_ARRAY : dim == 4 ? TEXTYPE_3D : array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE;
    bool cube = dim >= 6;
    int n = dim < 4 ? 2 : 3;
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), type, false, array);
    char co[3][48], g[2][160], layer[96] = "", off[160] = "", call[2048];
    for (int i = 0; i < n; i++) sprintf(co[i], "as_type<float>(%s)", RZ0(a++));
    const char *pp = RZ0(a);
    if (array && hw_layer_clamp(c)) sprintf(layer, ", %s & 0xFFFFu", pp);
    else if (array) sprintf(layer, ", min(%s & 0xFFFFu, t%d.get_array_size() - 1u)", pp, slot);
    for (int k = 0; k < 2; k++) {
        char *o = g[k];
        o += sprintf(o, "float%d(", n);
        for (int i = 0; i < n; i++) o += sprintf(o, "%sas_type<float>(%s)", i ? ", " : "", RZ0(b++));
        strcpy(o, ")");
    }
    if (toff && !cube) {
        if (n == 2) sprintf(off, ", int2(int(%s << 12) >> 28, int(%s << 8) >> 28)", pp, pp);
        else sprintf(off, ", int3(int(%s << 12) >> 28, int(%s << 8) >> 28, int(%s << 4) >> 28)", pp, pp, pp);
    }
    sprintf(call, "t%d.sample(s%d, %s%s, gradient%s(%s, %s)%s)", slot, slot,
            n == 2 ? UV2(slot, co[0], co[1]) : C3(co[0], co[1], co[2]), layer, n == 2 ? "2d" : cube ? "cube" : "3d", g[0], g[1], off);
    write_tex_result(c, slot, call, (uint32_t)bits(w, 31, 4), d, RZ, false, false);
}

// SULD.D / SUST / SUST.D, bindless (Ryujinx EmitSuld / EmitSust): the handle in gpr39 (its image slot by data flow:
// MAXMETAL_IACC, nvn_gflow), the coordinates from gpr8 (1D as 2D at y 0, as Ryujinx's Sample1DAs2D), the data in / from gpr0.
// .D: raw words of a size, the image viewed in its uint / int format (a byte address .BA: x in bytes); else the RGBA
// components in order, stored as floats (the format unknown) from the first component on, the rest 0.
static void emit_surface(Ctx *c, uint64_t w, bool store, bool raw) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), h = (int)bits(w, 39, 8), dim = (int)bits(w, 33, 3);
    if (c->glsl || !nvn_on(c)) { fail(c, "surface (glsl)"); return; }
    if (dim != 0 && dim != 3) { fail(c, "surface dim %d", dim); return; }
    if (!store && d == RZ) return;
    if (!c->gtrack && !c->itrack) { fail(c, "bindless surface without data flow"); return; }
    if (c->niop == 64) { fail(c, "too many surface ops"); return; }
    static const char *const fmts[] = {"r8ui", "r8i", "r16ui", "r16i", "r32ui", "rg32ui", "rgba32ui", "rgba32ui"};
    int size = (int)bits(w, 20, 3), n = !raw ? __builtin_popcount((unsigned)bits(w, 20, 4)) : size == 5 ? 2 : size >= 6 ? 4 : 1;
    const char *elem = !raw ? "float" : size == 1 || size == 3 ? "int" : "uint", *fmt = raw ? fmts[size] : "";
    static const int lg[] = {0, 0, 1, 1, 2, 3, 4, 4};
    char x[64], y[32], co[200];
    if (raw && bits(w, 23, 1)) sprintf(x, "(int(%s) >> %d)", RZ0(a), lg[size]); else sprintf(x, "int(%s)", RZ0(a));
    if (dim == 3) sprintf(y, "int(%s)", RZ0(a + 1)); else strcpy(y, "0");
    sprintf(co, "uint2(int2(%s, %s))", x, y);
    gev_push(c, (GEv){.blk = c->gblk, .kind = 1, .reg = h, .id = c->ngacc, .pred = c->gpred});
    c->iop[c->niop++] = (typeof(c->iop[0])){.id = c->ngacc, .elem = elem, .fmt = fmt, .store = store};
    if (store) {
        char v[4][48];
        for (int i = 0; i < 4; i++) {
            if (i >= n) strcpy(v[i], *elem == 'f' ? "0.0f" : *elem == 'i' ? "0" : "0u");
            else sprintf(v[i], *elem == 'f' ? "as_type<float>(%s)" : *elem == 'i' ? "int(%s)" : "%s", RZ0(d == RZ ? RZ : d + i));
        }
        sp(&c->body, "    MAXMETAL_IACC(%d).write(%s4(%s, %s, %s, %s), %s);\n", c->ngacc++, elem, v[0], v[1], v[2], v[3], co);
    } else {
        sp(&c->body, "    { %s4 sv = MAXMETAL_IACC(%d).read(%s);\n", elem, c->ngacc++, co);
        for (int i = 0; i < n && d + i < RZ; i++) { c->reg_used[d + i] = 1; sp(&c->body, "      r%d = as_type<uint>(sv[%d]);\n", d + i, i); }
        sp(&c->body, "    }\n");
    }
}

// TLD (Ryujinx EmitTex with IntCoords): a texel fetch: the layer (arrays), then the coordinates, from gpr8; the level
// (.LL), the offsets (TOFF: 4 bits each), the sample (.MS) from gpr20. 2D, 2D array, 3D, multisample 2D.
static void emit_tld(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    if (c->glsl) { fail(c, "tld (glsl)"); return; }
    if (d == RZ) return;
    int dim = (int)bits(w, 28, 3);
    bool ll = bits(w, 55, 1), toff = bits(w, 35, 1), ms = bits(w, 50, 1);
    if ((dim != 2 && dim != 3 && dim != 4) || (ms && dim != 2)) { fail(c, "tld dim %d%s", dim, ms ? " ms" : ""); return; }
    int type = ms ? TEXTYPE_2D_MS : dim == 2 ? TEXTYPE_2D : dim == 3 ? TEXTYPE_2D_ARRAY : TEXTYPE_3D;
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), type, false, dim == 3);
    const char *layer = dim == 3 ? RZ0(a++) : NULL, *x = RZ0(a), *y = RZ0(a + 1), *z = RZ0(a + 2);
    const char *lod = ll ? RZ0(b++) : "0u", *o = toff ? RZ0(b++) : NULL, *smp = ms ? RZ0(b++) : NULL;
    char co[400], call[1024];
    if (dim == 4) {
        if (o) sprintf(co, "uint3(int3(%s, %s, %s) + int3(int(%s << 28) >> 28, int(%s << 24) >> 28, int(%s << 20) >> 28))", x, y, z, o, o, o);
        else sprintf(co, "uint3(%s, %s, %s)", x, y, z);
        sprintf(call, "t%d.read(%s, uint(%s))", slot, co, lod);
    } else {
        if (o) sprintf(co, "%s", RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", x, y, o, o));
        else sprintf(co, "%s", RSC2(c, slot, "uint2(%s, %s)", x, y));
        if (ms) sprintf(call, "t%d.read(%s, uint(%s))", slot, co, smp);
        else if (layer) sprintf(call, "t%d.read(%s, uint(%s), uint(%s))", slot, co, layer, lod);
        else sprintf(call, "t%d.read(%s, uint(%s))", slot, co, lod);
    }
    write_tex_result(c, slot, call, (uint32_t)bits(w, 31, 4), d, RZ, false, false);
}

// TMML: the level of detail as 8.8 fixed point, components in the reverse of textureQueryLod's order (Ryujinx); z, w 0.
// Metal computes it in fragment functions only: elsewhere level 0.
static void emit_tmml(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    if (c->glsl) { fail(c, "tmml (glsl)"); return; }
    if (d == RZ) return;
    bool array;
    int type = texdim_type(c, (int)bits(w, 28, 3), &array);
    if (type < 0) return;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int slot = tex_slot(c, (uint32_t)bits(w, 36, 13), -1, false, array);
    if (array) a++;
    char co[200];
    if (cube) sprintf(co, "float3(as_type<float>(%s), as_type<float>(%s), as_type<float>(%s))", RZ0(a), RZ0(a + 1), RZ0(a + 2));
    else sprintf(co, "float2(as_type<float>(%s), as_type<float>(%s))", RZ0(a), RZ0(a + 1));
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    sp(&c->body, "    {\n");
    if (c->stage == STAGE_FRAGMENT)
        sp(&c->body, "      float2 lq = float2(t%d.calculate_unclamped_lod(s%d, %s), t%d.calculate_clamped_lod(s%d, %s));\n",
           slot, slot, co, slot, slot, co);
    else
        sp(&c->body, "      float2 lq = float2(0.0f);\n");
    for (int comp = 0; comp < 4 && d < RZ; comp++) {
        if (!(mask & (1u << comp))) continue;
        c->reg_used[d] = 1;
        if (comp < 2) sp(&c->body, "      r%d = as_type<uint>(int(lq[%d] * 256.0f));\n", d, comp);
        else sp(&c->body, "      r%d = 0u;\n", d);
        d++;
    }
    sp(&c->body, "    }\n");
}

// LDC: c[idx][reg + imm]
static void emit_ldc(Ctx *c, uint64_t w, int d, int a) {
    char t[2048];
    int idx = (int)bits(w, 36, 5);
    // signed (Ryujinx EmitLdc: Imm16ToSInt(CbufOffset)); real games' skinning shaders read c[5][r - 16]
    int64_t off = nvn_on(c) ? sbits(w, 20, 16) : (int64_t)bits(w, 20, 16);
    int sz = (int)bits(w, 48, 3);
    int n = sz == 5 ? 2 : 1;
    if (nvn_on(c) && bits(w, 45, 1)) {  // (address modes IS / ISL take the slot from the register: not here)
        fail(c, "ldc mode %d size %d", (int)bits(w, 44, 2), sz);
        return;
    }
    if (nvn_on(c) && sz > 5) return;  // (128-bit: Ryujinx's Ldc does nothing; mode IL as the default, as there)
    if (nvn_on(c) && sz < 4 && a != RZ) {  // a sub-word load at a computed address (Ryujinx ExtractSmallInt)
        char cb[256], rb[16];
        cbuf_ref(c, idx, off, reg(c, a, rb), cb);
        c->info->cbuf_size[idx] = 0x10000;
        static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(v << (24u - sh)) >> 24)", "extract_bits(v, sh, 16u)",
                                          "uint(extract_bits(int(v), sh, 16u))"};
        if (d != RZ) {
            c->reg_used[d] = 1;
            sp(&c->body, "    { uint v = %s; uint sh = ((%s + 0x%xu) & 3u) * 8u; r%d = %s; }\n", cb, rb, (uint32_t)off, d, ext[sz]);
        }
        return;
    }
    if (nvn_on(c) && sz < 4) {  // a constant sub-word load (Ryujinx ExtractSmallInt)
        char cb[256];
        cbuf_ref(c, idx, off & ~3, NULL, cb);
        int sh = (int)(off & 3) * 8;
        if (sz == 0) sprintf(t, "((%s >> %du) & 0xFFu)", cb, sh);
        else if (sz == 1) sprintf(t, "uint(int(%s << %du) >> 24)", cb, 24 - sh);
        else if (sz == 2) sprintf(t, "((%s >> %du) & 0xFFFFu)", cb, sh & 16);
        else sprintf(t, "uint(int(%s << %du) >> 16)", cb, 16 - (sh & 16));
        set_reg(c, d, t);
        return;
    }
    char rb[16];
    const char *dyn = a == RZ ? NULL : reg(c, a, rb);
    if (nvn_on(c) && dyn) {  // Ryujinx EmitLdc's form: the word index once, word k at index + k (the compiler sees that
                             // an LDC.64's two words share a row and loads it once; from each word's byte address it does not)
        char cb[256];
        cbuf_ref(c, idx, off, dyn, cb);  // (the bank's use)
        c->info->cbuf_size[idx] = 0x10000;
        sp(&c->body, "    { uint lw = (%s + %lldu) >> 2u;\n", dyn, (long long)off);
        for (int i = 0; i < n && d + i < RZ; i++) {
            c->reg_used[d + i] = 1;
            sp(&c->body, "      r%d = (*spvDescriptorSet0.%s_c%d)._m0[(lw + %du) >> 2u][(lw + %du) & 3u];\n", d + i, c->pfx, idx, i, i);
        }
        sp(&c->body, "    }\n");
        return;
    }
    // LDC.64 R4, c[i][R4 + x]: the address register is also a destination. Read it once before the writes, or
    // the second word comes from c[i][<first word> + x + 4]
    bool overlap = dyn && n > 1 && a >= d && a < d + n;
    if (overlap) { sp(&c->body, "    { uint ldc_addr = %s;\n", dyn); dyn = "ldc_addr"; }
    for (int i = 0; i < n; i++) {
        char cb[256];
        cbuf_ref(c, idx, off + 4 * i, dyn, cb);
        if (dyn) c->info->cbuf_size[idx] = 0x10000;
        set_reg(c, d + i, cb);
    }
    if (overlap) sp(&c->body, "    }\n");
}

// ------------------------------------------------------------------ instruction translation
// (MAXMETAL_FEAT_LAS) Link's Awakening's light-culling compute shader (a 30x17 grid of 64-pixel tiles for the tiled lighting of interiors and
// caves) turns a tile's pixel column into a view direction with the screen height alone: x = px * 2 / height -
// aspect, which assumes square pixels (width / height = aspect). With a widened camera the image keeps its 1920x1080
// target, so pixels are no longer square: at 32:9 the tiles right of the screen centre got the lights meant for
// half as far out, and the lighting changed along a hard vertical line where a light's tiles ended (the floor
// right of it lighter, MarthaUkuku at 2/3 of the window). The two column steps (pc 0x198: tile x, 0x1a8: tile x +
// 1) are scaled by aspect * height / width, which is 1 at 16:9: the original shader's result there.
static void light_tiles_fix(Ctx *c, const uint64_t *code, size_t end, uint32_t pc) {
    static const uint64_t sig[][2] = {{0x188, 0x5c68100000970309ull}, {0x198, 0x5c68100000d7030dull},
                                      {0x1a8, 0x5c6810000087030bull}, {0x1b0, 0x5c68100000470303ull}};
    if (c->stage != STAGE_COMPUTE || (pc != 0x198 && pc != 0x1a8) || end <= 0x1b0 / 8) return;
    for (size_t i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (code[sig[i][0] / 8] != sig[i][1]) return;
    int d = (int)(code[pc / 8] & 0xff);  // (the destination: r13 / r11)
    char a[48], h[48], w[48];
    cbuf_ref(c, 4, 0x1bc, NULL, a);  // camera aspect
    cbuf_ref(c, 4, 0x1c4, NULL, h);  // target height
    cbuf_ref(c, 4, 0x1c0, NULL, w);  // target width
    sp(&c->body, "    r%d = as_type<uint>(as_type<float>(r%d) * (as_type<float>(%s) * as_type<float>(%s) / as_type<float>(%s)));\n", d, d, a, h, w);  // (GLSL: as_type is rewritten)
}

static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);  // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;  // @!pt: never executes
    if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));
    // (NVN) a flow instruction's condition-code test (bits 0-4; 15: always): BRA, BRX, EXIT, KIL, BRK, CONT, RET, SYNC
    bool ccflow = false;
    if (nvn_on(c) && (top12 == 0xe240000000000000ull || top12 == 0xe250000000000000ull || top12 == 0xe300000000000000ull ||
                      top12 == 0xe330000000000000ull || top12 == 0xe340000000000000ull || top12 == 0xe350000000000000ull ||
                      top12 == 0xe320000000000000ull || (w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) &&
        (w & 0x1f) != 0xf) {
        ccflow = true;
        sp(&c->body, "  if (%s) {\n", cc_test(c, (int)(w & 0x1f)));
    }

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        c->info->uses_discard = true;
        if (getenv("MAXMETAL_NO_DISCARD")) {
        } else if (!c->glsl) sp(&c->body, "    discard_fragment();\n");
        else if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    discard;\n");
        else sp(&c->body, "    /* kil */\n");  // GLSL has discard in fragment shaders only
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        sp(&c->body, "    pc = %uu; continue;\n", target);
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull) || OPM(0xe2a0000000000000ull, 0xfff0000000000020ull) ||
               OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {
        // SSY / PBK / PCNT push their target on one stack (kind 1 / 2 / 3 in bits 28-31); SYNC / BRK pop to the last
        // target of their kind and go there, CONT goes to it and keeps it: a BRK out of an SSY region inside a loop
        // drops that region's entry (Ryujinx's Decoder: PropagatePushOp, one branch stack)
        c->uses_branches = true;
        sp(&c->body, "    if (crs_n < 32u) crs[crs_n++] = 0x%xu;\n",
           (uint32_t)(bits(w, 52, 4) - 8) << 28 | norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull) || OPM(0xe340000000000000ull, 0xfff0000000000000ull) ||
               OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {  // SYNC / BRK / CONT
        unsigned kind = OPM(0xf0f8000000000000ull, 0xfff8000000000000ull) ? 1 : OPM(0xe340000000000000ull, 0xfff0000000000000ull) ? 2 : 3;
        sp(&c->body, "    while (crs_n > 0u && (crs[crs_n - 1u] >> 28) != %uu) crs_n--;\n", kind);
        sp(&c->body, kind == 3 ? "    pc = crs_n > 0u ? crs[crs_n - 1u] & 0xFFFFFFFu : 0u; continue;\n"
                               : "    pc = crs_n > 0u ? crs[--crs_n] & 0xFFFFFFFu : 0u; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);  // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) { fail(c, "ipa idx"); }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (addr == 0x7C && !guarded && !bits(w, 51, 1) && d != RZ) { c->wstate[d] = 1; c->wdef[d] = pc; }  // gl_FragCoord.w
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        if (ia != RZ) { fail(c, "indexed ld a[]"); }
        for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {  // LD c[idx][reg + imm]
        emit_ldc(c, w, d, a);
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {  // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {  // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xc000000000000000ull, 0xf800000000000000ull)) {  // TEX (.LC and the destination predicate ignored, as Ryujinx)
        emit_tex(c, w, false);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {  // TEX.B (bindless)
        emit_tex(c, w, true);
    } else if (OPM(0xc800000000000000ull, 0xfc00000000000000ull)) {  // TLD4
        emit_tld4(c, w);
    } else if (OPM(0xdf00000000000000ull, 0xffc0000000000000ull)) {  // TLD4S
        emit_tld4s(c, w, false);
    } else if (OPM(0xdf80000000000000ull, 0xffc0000000000000ull)) {  // TLD4S .F16
        emit_tld4s(c, w, true);
    } else if (OPM(0xdf58000000000000ull, 0xfff8000000000000ull)) {  // TMML
        emit_tmml(c, w);
    } else if (OPM(0xeb10000000000000ull, 0xfff8000000000000ull)) {  // SULD.D.B
        emit_surface(c, w, false, true);
    } else if (OPM(0xeb20000000000000ull, 0xfff8000000000000ull)) {  // SUST.B
        emit_surface(c, w, true, false);
    } else if (OPM(0xeb30000000000000ull, 0xfff8000000000000ull)) {  // SUST.D.B
        emit_surface(c, w, true, true);
    } else if (OPM(0xdc00000000000000ull, 0xff00000000000000ull)) {  // TLD
        emit_tld(c, w);
    } else if (OPM(0xdd00000000000000ull, 0xff00000000000000ull)) {  // TLD.B (bindless)
        fail(c, "bindless tld");
    } else if (OPM(0xde40000000000000ull, 0xffc0000000000000ull)) {  // TXD.B (bindless)
        fail(c, "bindless txd");
    } else if (OPM(0xde00000000000000ull, 0xff80000000000000ull)) {  // TXD
        emit_txd(c, w);
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);  // the type comes from a sample, if any
        if (qt == 1 && c->glsl) {  // tsize<slot>() is emitted with the slot's final type
            if (c->stage != STAGE_VERTEX && rscale_on(c)) {
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(floor(float(tsize%d(int(lv)).x) * rscale[%d].z / rscale[%d].x + 0.5f)), "
                   "uint(floor(float(tsize%d(int(lv)).y) * rscale[%d].w / rscale[%d].y + 0.5f)), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(tsize%d(int(lv)).x), uint(tsize%d(int(lv)).y), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else if (qt == 1) {  // dimension
            if (c->stage != STAGE_VERTEX && rscale_on(c)) {  // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) * rscale[%d].z / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) * rscale[%d].w / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "txq type %d", qt);
        }
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
        case 0: sprintf(t, "cos(%s)", x); break;
        case 1: sprintf(t, "sin(%s)", x); break;
        case 2: sprintf(t, "exp2(%s)", x); break;
        case 3: sprintf(t, "log2(%s)", x); break;
        case 4: sprintf(t, nvn_on(c) ? "maxmetal_rcp(%s)" : "(1.0f / %s)", x); break;
        case 5: sprintf(t, "rsqrt(%s)", x); break;
        case 8: sprintf(t, "sqrt(%s)", x); break;
        default: fail(c, "mufu %d", fn); return;
        }
        bool rcpw = fn == 4 && a != RZ && c->wstate[a] == 1 && !bits(w, 48, 1) && !bits(w, 46, 1) && !bits(w, 50, 1) && !guarded;
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
        if (rcpw && d != RZ) { c->wstate[d] = 2; c->wdef[d] = pc; }  // 1 / gl_FragCoord.w
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5900000000000000ull, 0xff80000000000000ull) || OPM(0x4900000000000000ull, 0xff80000000000000ull) ||
               OPM(0x3200000000000000ull, 0xfe80000000000000ull) || OPM(0x5b80000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x4b80000000000000ull, 0xfff0000000000000ull) || OPM(0x3680000000000000ull, 0xfef0000000000000ull) ||
               OPM(0x5c50000000000000ull, 0xfff8000000000000ull) || OPM(0x4c50000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3850000000000000ull, 0xfef8000000000000ull)) {
        // DSET / DSETP / DMNMX: FSET / FSETP / FMNMX (the same fields) on the values as floats, as Ryujinx's DoubleToFloat
        int kind = OPM(0x5900000000000000ull, 0xff80000000000000ull) || OPM(0x4900000000000000ull, 0xff80000000000000ull) ||
                   OPM(0x3200000000000000ull, 0xfe80000000000000ull) ? 0 : (w >> 52 & 0xf) == 8 ? 1 : 2;
        char da[200], db[600], cmp[1600];
        d64_pair(c, a, da); d64_b(c, w, db);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (kind == 0) {  // DSET
            const char *x = NA(da, bits(w, 43, 1), bits(w, 54, 1)), *y = NA(db, bits(w, 53, 1), bits(w, 44, 1));
            fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
            sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), pc_);
            sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
            if (bits(w, 47, 1)) {  // .CC: zero / sign flags of the result
                c->uses_cc = true;
                sp(&c->body, "    { uint fs = %s; ccz = fs == 0u; ccs = %s; }\n", u, bits(w, 52, 1) ? "false" : "fs != 0u");
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "    r%d = %s;\n", d, u); c->wstate[d] = 0; }
            } else {
                set_reg(c, d, u);
            }
        } else if (kind == 1) {  // DSETP
            const char *x = NA(da, bits(w, 43, 1), bits(w, 7, 1)), *y = NA(db, bits(w, 6, 1), bits(w, 44, 1));
            fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
            int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
            sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
            if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
            if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
            sp(&c->body, "    }\n");
        } else {  // DMNMX
            const char *x = NA(da, bits(w, 48, 1), bits(w, 46, 1)), *y = NA(db, bits(w, 45, 1), bits(w, 49, 1));
            sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", pc_, x, y, x, y);
            d64_set(c, d, t);
        }
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo)); }
        else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xff00000000000000ull) ||
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        if (nvn_on(c) && bits(w, 47, 1)) {  // .CC: zero / sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint fs = %s; ccz = fs == 0u; ccs = %s; }\n", u, bits(w, 52, 1) ? "false" : "fs != 0u");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "    r%d = %s;\n", d, u); c->wstate[d] = 0; }
        } else {
            set_reg(c, d, u);
        }
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        set_reg(c, d, B(false));
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 3) d64_b(c, w, srcv);   // F64
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (c->nvn) {   // (NVN) as Ryujinx EmitF2I: the conversion unclamped (Metal saturates, NaN 0), a negative to unsigned 0
            if (sgn) sprintf(t, i16 ? "uint(clamp(int(%s(%s)), -32768, 32767))" : "uint(int(%s(%s)))", rf[rnd], x);
            else sprintf(t, i16 ? "clamp(uint(fast::max(%s(%s), 0.0f)), 0u, 65535u)" : "uint(fast::max(%s(%s), 0.0f))", rf[rnd], x);
        } else if (i16)
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        // |x| and -x apply to the integer, before the conversion (Ryujinx EmitI2F: IAbsNeg; -0 is never made)
        char an[320];
        if (bits(w, 49, 1)) { snprintf(an, sizeof an, sgn ? "abs(%s)" : "uint(abs(int(%s)))", src); strcpy(src, an); }
        if (bits(w, 45, 1)) { snprintf(an, sizeof an, sgn ? "(-%s)" : "(0u - %s)", src); strcpy(src, an); }
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 3) {  // F64 (d64_set)
            d64_set(c, d, u);
        } else if (bits(w, 8, 2) == 1) {  // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))", u);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(u, false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        if (sfmt == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else if (sfmt == 3) d64_b(c, w, srcv);   // F64
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        if ((sfmt == 3) != (dfmt == 3)) mode = 0;  // (Ryujinx EmitF2F: rounds between the same formats only)
        switch (mode) {
        case 4: sprintf(t, "rint(%s)", x); break;
        case 5: sprintf(t, "floor(%s)", x); break;
        case 6: sprintf(t, "ceil(%s)", x); break;
        case 7: sprintf(t, "trunc(%s)", x); break;
        default: sprintf(t, "%s", x);
        }
        if (dfmt == 3) {  // F64 (d64_set)
            if (bits(w, 50, 1)) sprintf(u, "saturate(%s)", t);
            d64_set(c, d, bits(w, 50, 1) ? u : t);
        } else if (dfmt == 1) {  // F16 result in the low half, high half zero
            char h[1100];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false),
                bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1) && nvn_on(c)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s%s) ? 1u : 0u; ccz = %s(res == 0u); ccs = int(res) < 0;\n",
               bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + (cc & 1u)" : "",
               bits(w, 43, 1) ? " || (res == s && (cc & 1u) != 0u)" : "", bits(w, 43, 1) ? "ccz && " : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); c->wstate[d] = 0; }
            sp(&c->body, "    }\n");
        } else if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "",
               R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
           x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5a00000000000000ull, 0xff80000000000000ull) || OPM(0x4a00000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5200000000000000ull, 0xff80000000000000ull) || OPM(0x3400000000000000ull, 0xfe80000000000000ull) ||
               OPM(0x1000000000000000ull, 0xfc00000000000000ull)) {  // IMAD / IMAD32I (Ryujinx EmitImad: no CC, X, SAT)
        bool i32 = OPM(0x1000000000000000ull, 0xfc00000000000000ull);
        bool hi = bits(w, i32 ? 53 : 54, 1), sa = bits(w, i32 ? 54 : 48, 1), sb = bits(w, i32 ? 57 : 53, 1);
        int avg = (int)bits(w, i32 ? 55 : 51, 2);  // 1: C negated, 2: B negated (Ryujinx's NegB / NegA)
        char bb[256], cb[256], ib[16];
        const char *bv, *cv, *av = R(a);
        if (i32) { sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = R(d); }
        else if ((w >> 56) == 0x52) { bv = reg(c, (int)bits(w, 39, 8), bb); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = operand_b(c, w, false, bb); cv = reg(c, (int)bits(w, 39, 8), cb); }
        char nb[300], prod[900];
        sprintf(nb, avg == 2 ? "(-%s)" : "%s", bv);
        if (!hi) sprintf(prod, "(%s * %s)", av, nb);
        else if (sa && sb) sprintf(prod, "uint(mulhi(int(%s), int(%s)))", av, nb);
        else if (sa) sprintf(prod, "(mulhi(%s, %s) + %s * uint(int(%s) >> 31))", av, nb, nb, av);
        else if (sb) sprintf(prod, "(mulhi(%s, %s) + %s * uint(int(%s) >> 31))", av, nb, av, nb);
        else sprintf(prod, "mulhi(%s, %s)", av, nb);
        sprintf(t, "(%s + %s%s)", prod, avg == 1 ? "-" : "", cv);
        set_reg(c, d, t);
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        bool rform = (w >> 56) == 0x5c;
        int lrs = rform ? (int)bits(w, 37, 2) : 0, pa = rform ? (int)bits(w, 35, 2) : 0, pb = rform ? (int)bits(w, 33, 2) : 0,
            pc3 = rform ? (int)bits(w, 31, 2) : 0;
        if (nvn_on(c) && (lrs || pa || pb || pc3)) {  // half selects / 16-bit shift of the first sum (Ryujinx EmitIadd3)
            static const char *const part[] = {"%s", "(%s & 0xFFFFu)", "(%s >> 16)", "%s"};
            char ea[160], eb[160], ec[160];
            if (wcc3 || x3) fail(c, "iadd3 with carry and half selects / shift");
            sprintf(ea, part[pa], R(a));
            sprintf(eb, part[pb], B(false));
            sprintf(ec, part[pc3], R((int)bits(w, 39, 8)));
            sprintf(t, "(((%s%s + %s%s)%s) + %s%s)", bits(w, 51, 1) ? "-" : "", ea, bits(w, 50, 1) ? "-" : "", eb,
                    lrs == 1 ? " >> 16u" : lrs == 2 ? " << 16u" : "", bits(w, 49, 1) ? "-" : "", ec);  // 1: .RS, 2: .LS
            set_reg(c, d, t);
        } else if (wcc3 || x3) {  // carry in/out (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a),
               bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                    bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "",
                B(false));
        if (bits(w, 47, 1)) {  // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n",
               bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        char cmp[512];
        if (nvn_on(c) && bits(w, 43, 1)) icmp_ext(c, cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        else icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        char cmp[512];
        bool isx = nvn_on(c) && bits(w, 43, 1);
        if (isx) icmp_ext(c, cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        else icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        if (nvn_on(c) && bits(w, 47, 1)) {  // .CC: zero / sign flags (Ryujinx EmitIset; .X: Z only if both words are)
            c->uses_cc = true;
            sp(&c->body, "    { uint is_ = %s; ccz = %s(is_ == 0u); ccs = %s; }\n", u, isx ? "ccz && " : "",
               bits(w, 44, 1) ? "false" : "is_ != 0u");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "    r%d = %s;\n", d, u); c->wstate[d] = 0; }
        } else {
            set_reg(c, d, u);
        }
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) { lut = (uint32_t)bits(w, 48, 8); sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0))); bv = bb; cv = R((int)bits(w, 39, 8)); }
        else { lut = (uint32_t)bits(w, 48, 8); bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb); cv = R((int)bits(w, 39, 8)); }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) { strcpy(e, term); first = 0; }
            else { strcat(e, " | "); strcat(e, term); }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (bits(w, 40, 1)) fail(c, "bfe brev");
        sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            sprintf(t, "bfi(%s, %s, %s)", R(a), cv, bv);  // insert a into base (cbuf) with b = position/length
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false); cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {  // reg, reg
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {  // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {  // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16)); bv = bb; cv = R((int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {  // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        // Ryujinx's XmadOptimizer: the idioms of a 32-bit multiply, a * b + c (low 32 bits, the same bits) in one
        //   t = XMAD a, b, c;  u = XMAD.MRG a, b.H1, RZ;  d = XMAD.PSL.CBCC a.H1, u.H1, t    (32 x 32)
        //   t = XMAD a, b, c;  d = XMAD.PSL a.H1, b, t                                       (32 x 16: b's low half)
        int breg = top == 0x5b ? (int)bits(w, 20, 8) : top == 0x51 ? (int)bits(w, 39, 8) : RZ;
        int creg = top == 0x51 ? -1 : (int)bits(w, 39, 8);
        bool plain = !sa && !sb && !ext && !wcc && !guarded && a != RZ;
        // (the same b: the same register unwritten between, or the same immediate / constant)
        #define XM_SAME_B(p, r, v, txt) ((p)->breg == (r) && ((r) == RZ ? !strcmp((p)->b, (txt)) : (p)->bv == (v)))
        const XmPart *xt = creg >= 0 && creg < RZ && c->xm[creg].kind == 1 ? &c->xm[creg] : NULL;
        const XmPart *xu = top == 0x5b && breg < RZ && c->xm[breg].kind == 2 ? &c->xm[breg] : NULL;
        bool at_a = xt && xt->a == a && xt->av == c->ver[a];
        if (plain && ha && hb && psl && !mrg && cmode == 4 && at_a && xu && xu->a == a && xu->av == xt->av &&
            XM_SAME_B(xu, xt->breg, xt->bv, xt->b)) {
            sprintf(t, "((xs%d * xs%d) + xs%d)", xt->xs, xt->xs + 1, xt->xs + 2);
            set_reg(c, d, t);
        } else if (plain && ha && !hb && psl && !mrg && cmode == 0 && at_a && XM_SAME_B(xt, breg, breg < RZ ? c->ver[breg] : 0, bv)) {
            sprintf(t, "((xs%d * (xs%d & 0xFFFFu)) + xs%d)", xt->xs, xt->xs + 1, xt->xs + 2);
            set_reg(c, d, t);
        } else {
            if (plain && !ha && !psl && cmode == 0 && d != RZ && creg >= 0 && strlen(bv) < sizeof c->xm[0].b &&
                ((!hb && !mrg) || (hb && mrg && creg == RZ))) {  // t / u (recorded after the write: xm_writes)
                c->xnew = (XmPart){.kind = (uint8_t)(hb ? 2 : 1), .a = (uint8_t)a, .breg = (uint8_t)breg, .av = c->ver[a],
                                   .bv = breg < RZ ? c->ver[breg] : 0, .xs = -1};
                strcpy(c->xnew.b, bv);
                c->xnew_d = d;
                if (!hb) {  // t: a, b and c as they are now (a later part may write over them)
                    char cr[16];
                    c->xnew.xs = c->nxs; c->nxs += 3;
                    sp(&c->body, "    xs%d = %s; xs%d = %s; xs%d = %s;\n", c->xnew.xs, R(a), c->xnew.xs + 1, bv, c->xnew.xs + 2, reg(c, creg, cr));
                }
            }
            sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
            if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
            if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
            sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
            switch (cmode) {
            case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                  // clo
            case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                      // chi
            case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;  // csfu
            case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                              // cbcc
            default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                  // cfull
            }
            if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
            else sp(&c->body, "      uint res = prod + cc_;\n");
            if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
            if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        }
        #undef XM_SAME_B
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)),
                P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        if (nvn_on(c)) {  // (Ryujinx InstEmitMove.S2r; a vertex is a SIMD group of one lane, as in VOTE)
            bool lanes = c->stage != STAGE_VERTEX;
            if ((sr == 0x00 || (sr >= 0x38 && sr <= 0x3c)) && lanes) c->uses_lane = true;
            switch (sr) {
            case 0x00: set_reg(c, d, lanes ? "lane" : "0u"); break;
            case 0x13:  // thread kill: a helper invocation (Ryujinx: gl_HelperInvocation, read on entry), not in other stages
                if (c->stage == STAGE_FRAGMENT) { c->uses_helper = true; set_reg(c, d, "(maxmetal_helper ? 0xFFFFFFFFu : 0u)"); }
                else set_reg(c, d, "0u");
                break;
            case 0x11: case 0x1d: set_reg(c, d, "0u"); break;  // invocation id, invocation info
            case 0x12: set_reg(c, d, "0x3f800000u"); break;  // y direction: +1 (origin upper left, no flip)
            case 0x20: set_reg(c, d, c->stage == STAGE_COMPUTE ? "(tid.x | (tid.y << 16) | (tid.z << 26))" : "0u"); break;
            case 0x21: case 0x22: case 0x23: case 0x25: case 0x26: case 0x27:
                if (c->stage != STAGE_COMPUTE) { set_reg(c, d, "0u"); break; }
                sprintf(t, "%s.%c", sr < 0x24 ? "tid" : "ctaid", "xyz"[(sr - 1) & 3]);
                set_reg(c, d, t);
                break;
            case 0x38: set_reg(c, d, lanes ? "(1u << lane)" : "1u"); break;
            case 0x39: set_reg(c, d, lanes ? "((1u << lane) - 1u)" : "0u"); break;
            case 0x3a: set_reg(c, d, lanes ? "(lane == 31u ? 0xFFFFFFFFu : (2u << lane) - 1u)" : "1u"); break;
            case 0x3b: set_reg(c, d, lanes ? "(lane == 31u ? 0u : ~((2u << lane) - 1u))" : "0u"); break;
            case 0x3c: set_reg(c, d, lanes ? "~((1u << lane) - 1u)" : "0xFFFFFFFFu"); break;
            default: set_reg(c, d, "0u");  // (clock, ids, counters ...: Ryujinx's S2r gives 0)
            }
        } else switch (sr) {
        case 0x12: set_reg(c, d, "0xbf800000u"); break;  // y_direction: -1 (lower-left)
        case 0x21: set_reg(c, d, "tid.x"); break;
        case 0x22: set_reg(c, d, "tid.y"); break;
        case 0x23: set_reg(c, d, "tid.z"); break;
        case 0x25: set_reg(c, d, "ctaid.x"); break;
        case 0x26: set_reg(c, d, "ctaid.y"); break;
        case 0x27: set_reg(c, d, "ctaid.z"); break;
        case 0x00: set_reg(c, d, "0u"); break;
        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        const char *bv = B(false);
        sprintf(t, bits(w, 48, 1) ? "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))" : "(31u - clz(%s))", bv, bv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) { sprintf(u, "abs(%s)", src); strcpy(src, u); }
        if (bits(w, 45, 1)) { sprintf(u, "(-%s)", src); strcpy(src, u); }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (sat && !ssg) {  // .SAT from an unsigned source: compare unsigned (a U32 value above 0x7FFFFFFF isn't negative)
            static const char *const hi[2][3] = {{"0xFFu", "0xFFFFu", ""}, {"0x7Fu", "0x7FFFu", "0x7FFFFFFFu"}};
            if (dsz < 2 || dsg) sprintf(t, "min(uint(%s), %s)", src, hi[dsg][dsz < 2 ? dsz : 2]);
            else sprintf(t, "uint(%s)", src);
        } else if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else if (sat && !dsg) sprintf(t, "uint(max(%s, 0))", src);  // S32 -> U32 .SAT: negatives become 0
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
        }
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) { bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = B(false); cv = R((int)bits(w, 39, 8)); }
        if (bits(w, 48, 4)) fail(c, "prmt mode");
        sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        // funnel shift of hi:lo (C:A). .W (bit 50) wraps the shift amount, otherwise it's clamped to the width;
        // bits 37-38: 2 .U64 / 3 .S64 shift the full 64 bits (width 64), else 32. The immediate is 6 bits (20-25:
        // bits 37-38 of the 19-bit B immediate are the width field). .CC / .X / .HI (47-49) aren't modelled.
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        if (bits(w, 47, 3)) { fail(c, "shf cc/x/hi"); return; }
        char shb[16];
        const char *sh = ((w >> 60) == 3) ? (sprintf(shb, "%uu", (unsigned)bits(w, 20, 6)), shb) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        int width = bits(w, 38, 1) ? 64 : 32;
        bool s64 = bits(w, 37, 2) == 3, wrap = bits(w, 50, 1);
        sp(&c->body, wrap ? "    { uint s = %s & %du; ulong v = (ulong(%s) << 32) | ulong(%s);\n"
                          : "    { uint s = min(%s, %du); ulong v = (ulong(%s) << 32) | ulong(%s);\n",
           sh, wrap ? width - 1 : width, hi, R(a));
        if (right && width == 64)
            sprintf(t, "s >= 64u ? %s : uint(%s >> s)", s64 ? "uint(int(v >> 32) >> 31)" : "0u", s64 ? "long(v)" : "v");
        else if (right) sprintf(t, "uint(v >> s)");
        else if (width == 64) sprintf(t, "s >= 64u ? 0u : uint((v << s) >> 32)");
        else sprintf(t, "uint((v << s) >> 32)");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = %s;\n", d, t); }
        sp(&c->body, "    }\n");
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        if (nvn_on(c)) nvn_global(c, addr, store ? 1 : 0, size, d, -1, 0, RZ);
        else {
        sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul;\n", addr,
           (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))",
                                               "uint(*(device ushort *)(gmem + ga))", "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        bool lazy_wide = c->glsl && maxwell_gmem_chunk_shift && maxwell_gmem_lazy && nw > 1;  // (one chunk check per access)
        if (c->glsl && c->stage == STAGE_COMPUTE) {
            // Compute (Vulkan): the guest heap is host memory across PCIe on a discrete GPU, and a shader storing to
            // it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~4 ms a dispatch, ~0.07 ms without
            // the heap stores). The dispatch's storage buffers are copied to a device-local buffer (gfast_, address
            // table gmap) and back after it: loads and stores inside them use that copy.
            // Anything outside the table goes to the heap as before.
            c->info->uses_gmem_map = true;
            static const char *const gl_small[] = {
                "(%s >> (uint(ga & 3ul) * 8u)) & 0xFFu", "uint(sx8(%s >> (uint(ga & 3ul) * 8u)))",
                "(%s >> (uint(ga & 2ul) * 8u)) & 0xFFFFu", "uint(sx16(%s >> (uint(ga & 2ul) * 8u)))"};
            sp(&c->body, "      uint fw = ok ? %s(uint(ga)) : 0xFFFFFFFFu;\n", store ? "gfindw" : "gfind");  // (a store: flags its range)
            if (!store) {
                if (size < 4) {
                    if (d != RZ) {
                        char fast[160], slow[160];
                        snprintf(fast, sizeof fast, gl_small[size], "gfast_.w[fw]");
                        snprintf(slow, sizeof slow, gl_small[size], "GMR(uint(ga >> 2))");
                        c->reg_used[d] = 1;
                        sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? %s : ok ? %s : 0u;\n", d, fast, slow);
                    }
                } else if (lazy_wide) {
                    sp(&c->body, "      { uvec%d t = fw != 0xFFFFFFFFu ? uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%sgfast_.w[fw + %du]", k ? ", " : "", k);
                    sp(&c->body, ") : ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? gfast_.w[fw + %du] : ok ? GMR(uint(ga >> 2) + %du) : 0u;\n",
                               d + k, k, k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;
                if (size < 4) {
                    uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                    const char *bm = size < 2 ? "3ul" : "2ul";
                    sp(&c->body, "      if (ok) { uint sh = uint(ga & %s) * 8u; uint v = (%s & 0x%xu) << sh;\n", bm, R(d), m);
                    sp(&c->body, "        if (fw != 0xFFFFFFFFu) { atomicAnd(gfast_.w[fw], ~(0x%xu << sh)); atomicOr(gfast_.w[fw], v); }\n", m);
                    sp(&c->body, "        else { GMAND(uint(ga >> 2), ~(0x%xu << sh)); GMOR(uint(ga >> 2), v); } }\n", m);
                } else if (lazy_wide) {
                    for (int k = 0; k < nw; k++) sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
                    sp(&c->body, "      if (fw == 0xFFFFFFFFu && ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s; else if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k,
                           R(d == RZ ? RZ : d + k), k, R(d == RZ ? RZ : d + k));
                }
            }
        } else if (c->glsl) {  // 32-bit words (GMR / GMW / GMAND / GMOR: the prelude); sub-word accesses within the containing word
            static const char *const gl_small[] = {
                "(GMR(uint(ga >> 2)) >> (uint(ga & 3ul) * 8u)) & 0xFFu",
                "uint(sx8(GMR(uint(ga >> 2)) >> (uint(ga & 3ul) * 8u)))",
                "(GMR(uint(ga >> 2)) >> (uint(ga & 2ul) * 8u)) & 0xFFFFu",
                "uint(sx16(GMR(uint(ga >> 2)) >> (uint(ga & 2ul) * 8u)))"};
            if (!store) {
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      if (ok) r%d = %s; else r%d = 0u;\n", d, gl_small[size], d); }
                } else if (lazy_wide) {  // (one bounds check and address for the whole aligned access)
                    sp(&c->body, "      { uvec%d t = ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      if (ok) r%d = GMR(uint(ga >> 2) + %du); else r%d = 0u;\n", d + k, k, d + k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;  // (the draw marks the pages it stores to: vertex cache copies go stale)
                if (size < 4)
                    sp(&c->body, "      if (ok) { uint wi = uint(ga >> 2); uint sh = uint(ga & %s) * 8u; GMAND(wi, ~(0x%xu << sh)); "
                       "GMOR(wi, (%s & 0x%xu) << sh); }\n", size < 2 ? "3ul" : "2ul", size < 2 ? 0xFFu : 0xFFFFu, R(d),
                       size < 2 ? 0xFFu : 0xFFFFu);
                else if (lazy_wide) {
                    sp(&c->body, "      if (ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k, R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4); }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
        }
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        int b = (int)bits(w, 20, 8), k = a < RZ ? c->qshf[a].k : 0;
        if (k && b != RZ && c->qshf[a].src == b && swz == (k == 1 ? 0x99u : 0xa5u)) {
            // A = SHFL.BFLY B, 1 / 2 and this quad lane's B - A: dfdx / dfdy B (Ryujinx MatchDdxOrDdy)
            sprintf(t, "as_type<uint>(%s(%s))", k == 1 ? "dfdx" : "dfdy", F(R(b)));
        } else if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        // thread-private local memory, modelled as a word array (byte/short accesses within a word)
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        c->uses_local = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = lmem[(la >> 2) & %du]; uint sh = (la & 3u) * 8u; r%d = %s;\n", LMEM_WORDS - 1, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = lmem[((la >> 2) + %du) & %du];\n", d + k, k, LMEM_WORDS - 1); }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = (la >> 2) & %du; lmem[i] = (lmem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   LMEM_WORDS - 1, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      lmem[((la >> 2) + %du) & %du] = %s;\n", k, LMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX) {
            set_reg(c, d, R(a));  // no SIMD-group functions in vertex functions: lane-local value
        } else if (c->glsl) {  // a quad xor (lane mask < 4) is the subgroup xor within the quad
            static const char *const fn[] = {"subgroupShuffle", "subgroupShuffleUp", "subgroupShuffleDown", "subgroupShuffleXor"};
            sprintf(t, "%s(%s, uint(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
            int k = (int)bits(w, 20, 5);
            if (c->nvn && c->stage == STAGE_FRAGMENT && d != RZ && a != RZ && d != a && bits(w, 16, 4) == PT && bits(w, 28, 2) == 3 &&
                (k == 1 || k == 2) && bits(w, 34, 13) == 0x1c03) {
                c->qnew_d = d; c->qnew_src = a; c->qnew_k = k;
            }
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) { c->pred_used[pd] = 1; sp(&c->body, "    p%d = true;\n", pd); }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: bits 0-3 to the flags Z, S, C, O (Ryujinx EmitR2p; overflow isn't tracked)
            c->uses_cc = true;
            static const char *const flag[3] = {"ccz = ", "ccs = ", "cc = "};
            for (int i = 0; i < 3; i++)
                sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) %s((%s >> %du) & 1u)%s;\n"
                                     : "    if ((%s >> %du) & 1u) %s((%s >> %du) & 1u)%s;\n",
                   mask, i, flag[i], R(a), shift + i, i < 2 ? " != 0u" : "");
            goto r2p_done;
        }
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) p%d = ((%s >> %du) & 1u) != 0u;\n"
                                 : "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    r2p_done:;
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) { c->pred_used[i] = 1; sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i); }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {  // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a),
           (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP (zero/sign flags only)
        const char *cond = cc_test(c, (int)bits(w, 8, 5));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0xe3a0000000000000ull, 0xfff0000000000000ull) || OPM(0xef60000000000000ull, 0xffe0000000000000ull) ||
               OPM(0xef80000000000000ull, 0xfff8000000000000ull) || OPM(0xebf0000000000000ull, 0xfff9fff000000000ull) ||
               OPM(0xebe8000000000000ull, 0xfff8000000000000ull) || OPM(0xe390000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x50e0000000000000ull, 0xfff8000000000000ull)) {
        // BPT, CCTL / CCTLL / CCTLT (cache control), IDE, VOTE.VTG: hints, nothing to do (Ryujinx's stubs)
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {  // CSET (Ryujinx InstEmitConditionCode.Cset; no CC)
        sprintf(t, "((%s) %s %s)", cc_test(c, (int)bits(w, 8, 5)), bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMNMX (Ryujinx InstEmitVideoMinMax.Vmnmx)
        int asel = (int)(bits(w, 36, 3) | (bits(w, 48, 1) << 3)), bsel = (int)(bits(w, 28, 3) | (bits(w, 49, 1) << 3));
        bool mn = bits(w, 56, 1), sat = bits(w, 55, 1), dfmt = bits(w, 54, 1);
        int vop = (int)bits(w, 51, 3);
        char ea[160], eb[160], ec[32];
        video_ab(c, w, ea, eb);
        sprintf(ec, "%s", R((int)bits(w, 39, 8)));
        bool res_signed;
        sp(&c->body, "    { uint va = %s, vb = %s, vr;\n", ea, eb);
        if ((asel & 8) != (bsel & 8)) {
            res_signed = !mn;
            sp(&c->body, mn ? "      vr = max(va, vb);\n" : "      vr = uint(min(int(va), int(vb)));\n");
            sp(&c->body, "      if (int(%s) < 0) vr = %s;\n", (asel & 8) ? "vb" : "va", (asel & 8) ? "vb" : "va");
        } else {
            res_signed = asel & 8;
            if (res_signed) sp(&c->body, mn ? "      vr = uint(max(int(va), int(vb)));\n" : "      vr = uint(min(int(va), int(vb)));\n");
            else sp(&c->body, mn ? "      vr = max(va, vb);\n" : "      vr = min(va, vb);\n");
        }
        if (sat && dfmt && !res_signed) sp(&c->body, "      vr = min(vr, 0x7FFFFFFFu);\n");
        else if (sat && !dfmt && res_signed) sp(&c->body, "      vr = uint(max(int(vr), 0));\n");
        switch (vop) {
        case 0: sp(&c->body, "      vr = insert_bits(%s, vr, 16u, 16u);\n", ec); break;
        case 1: sp(&c->body, "      vr = insert_bits(%s, vr, 0u, 16u);\n", ec); break;
        case 2: sp(&c->body, "      vr = insert_bits(%s, vr, 0u, 8u);\n", ec); break;
        case 3: sp(&c->body, "      vr = insert_bits(%s, vr, 16u, 8u);\n", ec); break;
        case 4: sp(&c->body, "      vr = vr + %s;\n", ec); break;
        case 5: sp(&c->body, dfmt ? "      vr = uint(min(int(vr), int(%s)));\n" : "      vr = min(vr, %s);\n", ec); break;
        case 6: sp(&c->body, dfmt ? "      vr = uint(max(int(vr), int(%s)));\n" : "      vr = max(vr, %s);\n", ec); break;
        default: break;  // (7: the result as it is)
        }
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = vr;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5f00000000000000ull, 0xff00000000000000ull)) {  // VMAD (Ryujinx InstEmitVideoArithmetic.Vmad; no CC)
        int asel = (int)(bits(w, 36, 3) | (bits(w, 48, 1) << 3)), bsel = (int)(bits(w, 28, 3) | (bits(w, 49, 1) << 3));
        int avg = (int)bits(w, 53, 2), scale = (int)bits(w, 51, 2);
        bool as = asel & 8, bs = bsel & 8, rs = asel == 14 || bsel == 14 || avg == 1 || avg == 2;
        char ea[160], eb[160];
        video_ab(c, w, ea, eb);
        // the 64-bit product (unsigned with the signed operand's correction where the signs differ), negated (avg 2),
        // plus C (negated: avg 1) and one (avg 3), shifted right 7 / 15, the low word saturated to the result's range
        sp(&c->body, "    { uint va = %s, vb = %s, vc = %s%s;\n", ea, eb, avg == 1 ? "-" : "", R((int)bits(w, 39, 8)));
        if (as == bs) sp(&c->body, "      uint lo = va * vb, hi = %s;\n", as ? "uint(mulhi(int(va), int(vb)))" : "mulhi(va, vb)");
        else sp(&c->body, "      uint lo = va * vb, hi = %s + mulhi(va, vb);\n", as ? "vb * uint(int(va) >> 31)" : "va * uint(int(vb) >> 31)");
        if (avg == 2) sp(&c->body, "      lo = ~lo; hi = ~hi; { uint l = lo + 1u; hi += l < lo ? 1u : 0u; lo = l; }\n");
        sp(&c->body, "      uint rl = lo + vc, rh = hi + (rl < lo ? 1u : 0u);\n");
        if (avg == 3) sp(&c->body, "      { uint l = rl + 1u; rh += l < rl ? 1u : 0u; rl = l; }\n");
        if (scale == 1 || scale == 2) {
            int s = scale == 1 ? 7 : 15;
            sp(&c->body, rs ? "      rl = (rl >> %du) | (rh << %du); rh = uint(int(rh) >> %d);\n"
                            : "      rl = (rl >> %du) | (rh << %du); rh = rh >> %du;\n", s, 32 - s, s);
        }
        if (bits(w, 55, 1)) {  // .SAT
            sp(&c->body, "      uint sg = uint(int(rh) >> 31);\n");
            if (rs) sp(&c->body, "      if (rh != uint(int(rl) >> 31)) rl = sg != 0u ? 0x80000000u : 0x7FFFFFFFu;\n");
            else sp(&c->body, "      if (rh != 0u) rl = ~sg;\n");
        }
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = rl;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x50f0000000000000ull, 0xfff8000000000000ull)) {  // VSETP (Ryujinx InstEmitVideoMinMax.Vsetp)
        int asel = (int)(bits(w, 36, 3) | (bits(w, 48, 1) << 3)), bsel = (int)(bits(w, 28, 3) | (bits(w, 49, 1) << 3));
        int vc = (int)(bits(w, 43, 2) | (bits(w, 47, 1) << 2)), p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        char ea[160], eb[160], cmp[600];
        video_ab(c, w, ea, eb);
        // signs differ: a signed compare when neither is 32-bit (both fit), else false (Ryujinx's TODO)
        if ((asel & 8) != (bsel & 8) && ((asel & 7) == 6 || (bsel & 7) == 6)) strcpy(cmp, "false");
        else icmp_expr(cmp, vc, ea, eb, (asel & 8) || (bsel & 8));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, P((int)bits(w, 39, 3), bits(w, 42, 1)));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5bd0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bd0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36d0000000000000ull, 0xfef8000000000000ull)) {
        // LEA (Ryujinx EmitLea): (A << shift), negated with .NEG, plus B; no CC / X, the predicate result isn't written
        sprintf(t, "((%s(%s << %du)) + %s)", bits(w, 45, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c70000000000000ull, 0xfff8000000000000ull) || OPM(0x4c70000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3870000000000000ull, 0xfef8000000000000ull) ||    // DADD
               OPM(0x5b70000000000000ull, 0xfff0000000000000ull) || OPM(0x4b70000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3670000000000000ull, 0xfef0000000000000ull) || OPM(0x5370000000000000ull, 0xfff0000000000000ull) ||  // DFMA
               OPM(0x5c80000000000000ull, 0xfff8000000000000ull) || OPM(0x4c80000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3880000000000000ull, 0xfef8000000000000ull)) {    // DMUL
        // FP64 (d64_b): the float operation; the rounding mode isn't used. DADD: |A|, -A, |B|, -B; DFMA (Rc: B a register,
        // C the constant): -B, -C; DMUL: -B
        bool add = OPM(0x5c70000000000000ull, 0xfff8000000000000ull) || OPM(0x4c70000000000000ull, 0xfff8000000000000ull) ||
                   OPM(0x3870000000000000ull, 0xfef8000000000000ull);
        bool rc = OPM(0x5370000000000000ull, 0xfff0000000000000ull);
        bool fma_ = rc || OPM(0x5b70000000000000ull, 0xfff0000000000000ull) || OPM(0x4b70000000000000ull, 0xfff0000000000000ull) ||
                    OPM(0x3670000000000000ull, 0xfef0000000000000ull);
        char ob[1100], oc[1100], sa[128];
        if (bits(w, 47, 1)) fail(c, "fp64 cc");
        d64_pair(c, a, sa);
        if (rc) d64_pair(c, (int)bits(w, 39, 8), ob);
        else d64_b(c, w, ob);
        if (rc) d64_b(c, w ^ 0x1000000000000000ull, oc);   // (its constant: the C form's encoding of B)
        else d64_pair(c, (int)bits(w, 39, 8), oc);
        if (add) sprintf(t, "%s + %s", fneg_abs(sa, bits(w, 48, 1), bits(w, 46, 1), alloca(300)),
                         fneg_abs(ob, bits(w, 45, 1), bits(w, 49, 1), alloca(1200)));
        else if (fma_) sprintf(t, "fma(%s, %s%s, %s%s)", sa, bits(w, 48, 1) ? "-" : "", ob, bits(w, 49, 1) ? "-" : "", oc);
        else sprintf(t, "%s * %s%s", sa, bits(w, 48, 1) ? "-" : "", ob);
        d64_set(c, d, t);
    } else if (OPM(0x5bd8000000000000ull, 0xfff8000000000000ull) || OPM(0x1800000000000000ull, 0xfc00000000000000ull)) {
        // LEA.HI (Ryujinx EmitLeaHi): the high word of ((C:A) << shift), negated as 64 bits with .NEG, plus B; no CC / X
        bool cform = (w >> 58) == 0x6;
        int sh = cform ? (int)bits(w, 51, 5) : (int)bits(w, 28, 5);
        bool neg = cform ? bits(w, 56, 1) : bits(w, 37, 1);
        char cb[256];
        const char *bv = cform ? cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb) : R((int)bits(w, 20, 8));
        sp(&c->body, "    { ulong la = (ulong(%s) << 32 | ulong(%s)) << %du;%s\n", R((int)bits(w, 39, 8)), R(a), sh, neg ? " la = -la;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(la >> 32) + %s;\n", d, bv); }
        sp(&c->body, "    }\n");
    } else if (nvn_on(c) && OPM(0xef48000000000000ull, 0xfff8000000000000ull)) {  // LDS
        nvn_shared(c, w, false);
    } else if (nvn_on(c) && OPM(0xef58000000000000ull, 0xfff8000000000000ull)) {  // STS
        nvn_shared(c, w, true);
    } else if (nvn_on(c) && OPM(0xec00000000000000ull, 0xff00000000000000ull)) {  // ATOMS
        nvn_shared_atom(c, w);
    } else if (nvn_on(c) && OPM(0xebf8000000000000ull, 0xfff8000000000000ull)) {  // RED (Ryujinx InstEmitMemory.Red)
        uint32_t imm = (uint32_t)bits(w, 28, 20);
        if (bits(w, 48, 1) && a != RZ) sprintf(t, "((ulong(%s) << 32 | ulong(%s)) + ulong(0x%xu))", R(a + 1), R(a), imm);
        else sprintf(t, "ulong(%s + 0x%xu)", R(a), imm);
        nvn_global(c, t, 2, 4, d, (int)bits(w, 23, 3), (int)bits(w, 20, 3), RZ);
    } else if (nvn_on(c) && OPM(0xed00000000000000ull, 0xff00000000000000ull)) {  // ATOM (Ryujinx InstEmitMemory.Atom)
        uint32_t imm = (uint32_t)((int32_t)((uint32_t)bits(w, 28, 20) << 12) >> 12);  // sign-extended (zero-extended to 64)
        if (bits(w, 48, 1) && a != RZ) sprintf(t, "((ulong(%s) << 32 | ulong(%s)) + ulong(0x%xu))", R(a + 1), R(a), imm);
        else sprintf(t, "ulong(%s + 0x%xu)", R(a), imm);
        nvn_global(c, t, 3, 4, d, (int)bits(w, 52, 4), (int)bits(w, 49, 3), (int)bits(w, 20, 8));
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {  // VOTE (Ryujinx InstEmitWarp.Vote)
        // A vertex function has no SIMD-group functions here: each vertex is a group of one lane (as Ryujinx's path
        // lowers the visibility-counter vertex shaders, VertexCounterLowering.cs: ballot(p) = p, lane 0)
        int vp = (int)bits(w, 45, 3), mode = (int)bits(w, 48, 2);
        const char *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        bool one = c->stage == STAGE_VERTEX;
        if (c->glsl) { fail(c, "vote (glsl)"); return; }
        if (vp != PT) {
            c->pred_used[vp] = 1;
            if (mode == 0) sp(&c->body, one ? "    p%d = %s;\n" : "    p%d = simd_all(%s);\n", vp, p);
            else if (mode == 1) sp(&c->body, one ? "    p%d = %s;\n" : "    p%d = simd_any(%s);\n", vp, p);
            else if (mode == 2 && one) sp(&c->body, "    p%d = true;\n", vp);
            else if (mode == 2) sp(&c->body, "    { bool vv = %s; p%d = simd_all(vv) || simd_all(!vv); }\n", p, vp);
            else fail(c, "vote mode %d", mode);
        }
        if (d != RZ) {
            if (one) sprintf(t, "(%s ? 1u : 0u)", p);
            else sprintf(t, "uint(uint64_t(simd_ballot(%s)))", p);
            set_reg(c, d, t);
        }
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR
        if (!c->glsl) sp(&c->body, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\n");
        else sp(&c->body, "    /* bar */\n");  // GLSL barrier() is compute / tessellation only
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
    }
    (void)op13;
    if (ccflow) sp(&c->body, "  }\n");
    if (guarded) sp(&c->body, "  }\n");
}

// ------------------------------------------------------------------ program assembly
static bool is_branch_target_op(uint64_t w) {
    return ((w & 0xfff0000000000020ull) == 0xe240000000000000ull);
}

// ------------------------------------------------------------------ GLSL output
static bool id_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool id_char(char ch) { return id_start(ch) || (ch >= '0' && ch <= '9'); }

// MSL-spelled instruction body -> GLSL (token-aware; comments and numbers are copied as they are).
static void glsl_rewrite(Ctx *c, Str *o, const char *src) {
    static const char *const map[][2] = {
        {"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}, {"int2", "ivec2"}, {"int3", "ivec3"},
        {"int4", "ivec4"}, {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bool2", "bvec2"},
        {"ulong", "uint64_t"}, {"long", "int64_t"}, {"rsqrt", "inversesqrt"}, {"rint", "roundEven"},
        {"front_facing", "gl_FrontFacing"}, {"point_coord", "gl_PointCoord"}, {"lane", "gl_SubgroupInvocationID"},
        {"vid", "uint(gl_VertexIndex)"}, {"iid", "uint(gl_InstanceIndex)"},
    };
    const char *p = src;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            e = e ? e + 2 : p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (*p >= '0' && *p <= '9') {  // number (0x..u, 1.0f, ...)
            const char *e = p;
            while (id_char(*e) || *e == '.') e++;
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (!id_start(*p)) { sp(o, "%c", *p); p++; continue; }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        char id[64];
        if (n >= (int)sizeof id || (p > src && p[-1] == '.')) {  // member / swizzle
            sp(o, "%.*s", n, p);
            p = e;
            continue;
        }
        memcpy(id, p, n);
        id[n] = 0;
        p = e;
        if (!strcmp(id, "as_type") && *p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) { fail(c, "glsl: bad as_type"); return; }
            int tn = (int)(gt - p - 1);
            if (tn == 5 && !strncmp(p + 1, "float", 5)) sp(o, "as_f");
            else if (tn == 4 && !strncmp(p + 1, "uint", 4)) sp(o, "as_u");
            else if (tn == 3 && !strncmp(p + 1, "int", 3)) sp(o, "as_i");
            else if (tn == 5 && !strncmp(p + 1, "half2", 5)) sp(o, "unpack_h2");
            else { fail(c, "glsl: as_type<%.*s>", tn, p + 1); return; }
            p = gt + 1;
            continue;
        }
        if ((!strcmp(id, "in") || !strcmp(id, "out")) && *p == '.') {  // stage I/O struct members
            const char *m = p + 1, *me = m;
            while (id_char(*me)) me++;
            char mem[32];
            snprintf(mem, sizeof mem, "%.*s", (int)(me - m), m);
            if (!strcmp(mem, "pos")) sp(o, c->stage == STAGE_FRAGMENT ? "gl_FragCoord" : "gl_Position");
            else if (!strcmp(mem, "psize")) sp(o, "gl_PointSize");
            else sp(o, "%s", mem);  // a<i> (vertex input), v<i> (varying)
            p = me;
            continue;
        }
        if ((!strcmp(id, "char") || !strcmp(id, "short")) && *p == '(') { sp(o, id[0] == 'c' ? "sx8" : "sx16"); continue; }
        if (!strcmp(id, "tid") || !strcmp(id, "ctaid")) {
            sp(o, c->stage != STAGE_COMPUTE ? "uvec3(0u)" : id[0] == 't' ? "gl_LocalInvocationID" : "gl_WorkGroupID");
            continue;
        }
        if (id[0] == 'r' && n > 1 && n < 6) {  // registers beyond r255 (RZ) come from out-of-range encodings
            bool num = true;
            for (int i = 1; i < n; i++) num = num && id[i] >= '0' && id[i] <= '9';
            if (num && atoi(id + 1) > 255) { fail(c, "register %s out of range", id); return; }
        }
        const char *rep = id;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(id, map[i][0])) { rep = map[i][1]; break; }
        sp(o, "%s", rep);
    }
}

static const char glsl_prelude[] =
    "#version 460\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_KHR_shader_subgroup_basic : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\n"
    "#extension GL_EXT_texture_shadow_lod : require\n"
    "#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]\n"
    "uint as_u(float x) { return floatBitsToUint(x); }\n"
    "uint as_u(uint x) { return x; }\n"
    "uint as_u(int x) { return uint(x); }\n"
    "float as_f(uint x) { return uintBitsToFloat(x); }\n"
    "float as_f(int x) { return intBitsToFloat(x); }\n"
    "float as_f(float x) { return x; }\n"
    "int as_i(uint x) { return int(x); }\n"
    "int as_i(float x) { return floatBitsToInt(x); }\n"
    "int sx8(uint v) { return bitfieldExtract(int(v), 0, 8); }\n"
    "int sx8(int v) { return bitfieldExtract(v, 0, 8); }\n"
    "int sx16(uint v) { return bitfieldExtract(int(v), 0, 16); }\n"
    "int sx16(int v) { return bitfieldExtract(v, 0, 16); }\n"
    // MSL semantics: saturate(NaN) = 0, fmin/fmax return the non-NaN operand
    "float saturate(float x) { return isnan(x) ? 0.0f : clamp(x, 0.0f, 1.0f); }\n"
    "vec2 saturate(vec2 x) { return vec2(saturate(x.x), saturate(x.y)); }\n"
    "float fmin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }\n"
    "float fmax(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }\n"
    "uint clz(uint v) { return v == 0u ? 32u : uint(31 - findMSB(v)); }\n"
    "uint popcount(uint v) { return uint(bitCount(v)); }\n"
    "uint mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
    "int mulhi(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n"
    "uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return bitfieldExtract(v, int(pos), int(min(len, 32u - pos))); }\n"
    "uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(bitfieldExtract(int(v), int(pos), int(min(len, 32u - pos)))); }\n"
    "uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return bitfieldInsert(base, ins, int(pos), int(min(len, 32u - pos))); }\n"
    "float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n";
// (MAXMETAL_FEAT_RSCALE) texel coordinates and UVs into scaled allocations (rscale[1 + slot])
static const char glsl_prelude_rscale[] =
    "uvec2 rsc(uvec2 c, vec4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uvec2(floor((vec2(c) + 0.5f) * s.xy)); }\n"
    // a scaled texture whose contents occupy its top-left part (Dynamic resolution): sample that part
    "vec2 uvs(vec2 uv, vec4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n";
// The half <-> float helpers (glsl_finish: glsl_h2f_exact or glsl_h2f_native, then glsl_prelude_half).
// Half -> float in vertex (and other pre-raster) shaders: by bits, not unpackHalf2x16. NVIDIA's driver recognises
// unpacked halves and runs some of the fp32 math after them in native fp16, differently per shader, so a depth
// pre-pass and its colour pass (EQUAL test) placed wind-animated foliage apart: black slashes / flicker.
static const char glsl_h2f_exact[] =
    "float h2f(uint h) { uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;"
    " if (e == 0u) { float d = float(m) * 5.9604644775390625e-8; return s != 0u ? -d : d; }"
    " if (e == 31u) return uintBitsToFloat(s | 0x7F800000u | (m << 13));"
    " return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13)); }\n"
    "vec2 unpack_h2(uint v) { return vec2(h2f(v & 0xFFFFu), h2f(v >> 16)); }\n";
// ... and in fragment / compute shaders the native conversion: positions don't depend on them, and the by-bits
// version (~12 integer ops and two branches per half) was half of the main lighting pass's GPU time on an Intel UHD
// 770 and a third on an RTX 3080 Ti (the game's shaders use packed halves heavily). MAXMETAL_VK_EXACT_H2F=1: by bits
// in every stage.
static const char glsl_h2f_native[] =
    "float h2f(uint h) { return unpackHalf2x16(h).x; }\n"
    "vec2 unpack_h2(uint v) { return unpackHalf2x16(v); }\n";
static const char glsl_prelude_half[] =
    "vec2 hx(uint v, uint swz) { if (swz == 1u) return vec2(uintBitsToFloat(v)); vec2 h = unpack_h2(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    "uint hpack(vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(r.x); uint lo = packHalf2x16(vec2(r.x, 0.0f)), hi = packHalf2x16(vec2(r.y, 0.0f)); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n"
    "uint prmt(uint a, uint b, uint sel) { uint r = 0u; for (uint i = 0u; i < 4u; i++) { uint s = (sel >> (i * 4u)) & 7u; uint bt = s < 4u ? (a >> (s * 8u)) & 0xFFu : (b >> ((s - 4u) * 8u)) & 0xFFu; r |= bt << (i * 8u); } return r; }\n";

static const char *glsl_sampler_type(const ShaderTexture *t) {
    static const char *const names[][2] = {
        {"sampler1D", "sampler1DShadow"}, {"sampler2D", "sampler2DShadow"}, {"sampler3D", "sampler3D"},
        {"samplerCube", "samplerCubeShadow"}, {"sampler2DArray", "sampler2DArrayShadow"},
        {"samplerCubeArray", "samplerCubeArrayShadow"}, {"sampler1DArray", "sampler1DArrayShadow"}};
    int ty = t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D;
    return names[ty][t->shadow ? 1 : 0];
}

// (GLSL fragment, lazy heap chunks) Early depth / stencil tests for a shader that counts chunk misses (GMiss atomics):
// only where the tests' outcome can't depend on the shader - no discard, no depth output (a sample mask output
// isn't translated) - and the shader has no side effect of its own (stores), so whether a hidden fragment runs it is
// not observable. (Alpha to
// coverage with early tests would write depth for samples it drops: the renderer uses this shader's module without
// the execution mode for those pipelines.)
static bool glsl_early_tests(const Ctx *c) {
    return c->glsl && c->stage == STAGE_FRAGMENT && c->info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy &&
           !maxwell_gmem_fs_miss && !c->info->uses_discard && !c->omap_depth && !c->uses_gmem_store;
}

// Declarations + entry point around the (rewritten) body; mirrors the MSL assembly in maxwell_translate.
static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    int stage = c->stage;
    Str body = {0};
    glsl_rewrite(c, &body, c->body.buf ? c->body.buf : "");
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
    if (info->cbuf_mask >> 18) fail(c, "cbuf index out of range");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) fail(c, "3d shadow texture");
    if (c->fail) { free(body.buf); free(c->body.buf); free(c); return info; }
    int set = stage == STAGE_FRAGMENT ? 1 : 0;
    Str s = {0};
    sp(&s, "#version 460\n");  // (the prelude's first line; an extension directive has to follow it)
    if (info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy) sp(&s, "#extension GL_EXT_buffer_reference : require\n");
    sp(&s, "%s", glsl_prelude + sizeof "#version 460\n" - 1);
    if (c->feat & MAXMETAL_FEAT_RSCALE) sp(&s, "%s", glsl_prelude_rscale);
    static int exact = -1;
    if (exact < 0) exact = getenv("MAXMETAL_VK_EXACT_H2F") && atoi(getenv("MAXMETAL_VK_EXACT_H2F")) != 0;
    sp(&s, "%s%s", !exact && (stage == STAGE_FRAGMENT || stage == STAGE_COMPUTE) ? glsl_h2f_native : glsl_h2f_exact,
       glsl_prelude_half);
    for (int i = 0; i < 18; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        uint32_t n = info->cbuf_size[i] / 16;
        if (c->cbuf_need[i] > n) n = c->cbuf_need[i];
        if ((info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        sp(&s, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%u]; };\n", set, i, i, i, n);
    }
    if (info->uses_gmem && !maxwell_gmem_chunk_shift) {
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem { uint w[]; } gmem_;\n", set, GMEM_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "#define GMR(i) gmem_.w[i]\n");
        if (c->uses_gmem_store)
            sp(&s, "#define GMW(i, v) gmem_.w[i] = (v)\n#define GMAND(i, m) atomicAnd(gmem_.w[i], m)\n"
                   "#define GMOR(i, m) atomicOr(gmem_.w[i], m)\n");
    } else if (info->uses_gmem && !maxwell_gmem_lazy) {  // the heap in windows, one binding each, plain branches (maxmetal.h)
        int n = maxwell_gmem_chunks(), ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        for (int k = 0; k < n; k++)
            sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem%d { uint w[]; } gmem%d_;\n", cs, GMEM_CHUNK_BINDING + k,
               c->uses_gmem_store ? "" : "readonly ", k, k);
        sp(&s, "uint GMR_(uint i) { uint c = i >> %du, o = i & 0x%xu;\n", ws, (1u << ws) - 1);
        for (int k = 0; k < n; k++) sp(&s, "  if (c == %du) return gmem%d_.w[o];\n", k, k);
        sp(&s, "  return 0u; }\n#define GMR(i) GMR_(i)\n");
        if (c->uses_gmem_store) {
            static const char *const ops[3][2] = {{"GMW_", "gmem%d_.w[o] = v"}, {"GMAND_", "atomicAnd(gmem%d_.w[o], v)"},
                                                  {"GMOR_", "atomicOr(gmem%d_.w[o], v)"}};
            for (int op = 0; op < 3; op++) {
                sp(&s, "void %s(uint i, uint v) { uint c = i >> %du, o = i & 0x%xu;\n", ops[op][0], ws, (1u << ws) - 1);
                for (int k = 0; k < n; k++) {
                    char body[64];
                    snprintf(body, sizeof body, ops[op][1], k);
                    sp(&s, "  if (c == %du) { %s; return; }\n", k, body);
                }
                sp(&s, "}\n");
            }
            sp(&s, "#define GMW(i, v) GMW_(i, v)\n#define GMAND(i, m) GMAND_(i, m)\n#define GMOR(i, m) GMOR_(i, m)\n");
        }
    } else if (info->uses_gmem) {  // chunks imported lazily: a table of device addresses, buffer references (maxmetal.h)
        int ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        bool direct = stage != STAGE_COMPUTE;  // (compute: its storage buffers are in gfast_ already)
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GChunks { uvec4 gchunk[%d]; };\n", cs, GMEM_CHUNK_BINDING, GMEM_CHUNKS_MAX);
        sp(&s, "layout(buffer_reference, std430, buffer_reference_align = 4) %sbuffer GW { uint w[]; };\n", c->uses_gmem_store ? "" : "readonly ");
        // Misses are counted with atomics on GMiss: a side effect, which in a fragment shader turns early depth / stencil
        // tests off - every overdrawn fragment ran in full (UHD 770 main pass 13.3 ms vs 8.4). So a fragment shader
        // whose own result can't depend on late tests (no discard or depth output, no stores) declares
        // early tests (glsl_early_tests) and keeps its counter; one that can't (discard / depth, no stores)
        // counts nothing - it would cost those tests - and relies on the runtime bringing its chunks in before the draw:
        // the bound storage buffers' chunks (gmem_chunk_bind), pointers in uniform buffers, the chunks other shaders
        // missed. MAXMETAL_VK_GMEM_FS_MISS=1 (maxwell_gmem_fs_miss): every shader counts, no
        // early tests (as before; to find a chunk only such a shader reaches).
        bool count = stage != STAGE_FRAGMENT || maxwell_gmem_fs_miss || c->uses_gmem_store || glsl_early_tests(c);
        if (count) {
            sp(&s, "layout(std430, set = %d, binding = %d) buffer GMiss { uint n[%d]; uint w[%d]; } gmiss_;\n", cs, GMEM_MISS_BINDING,
               GMEM_CHUNKS_MAX, GMEM_CHUNKS_MAX);
            sp(&s, "void gmiss(uint c, uint i) { atomicAdd(gmiss_.n[c], 1u); atomicMin(gmiss_.w[c], i); }\n");
        } else {
            sp(&s, "void gmiss(uint c, uint i) { }\n");
        }
        // (gchunk[c]: x, y = device address, z = words; the first word's chunk holds the whole aligned access)
        sp(&s, "#define GMC(i) ((i) >> %du)\n#define GMO(i) ((i) & 0x%xu)\n", ws, (1u << ws) - 1);
        if (direct) {  // (gd[k]: x = the chunk's first word, y = its words (0: unused), z, w = its device address; GDir<k>: it)
            sp(&s, "layout(push_constant) uniform GDirect { uvec4 gd[%d]; };\n", GMEM_DIRECT_MAX);
            for (int k = 0; k < GMEM_DIRECT_MAX; k++)
                sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GDir%d { uint w[]; } gdir%d_;\n", cs, GMEM_DIRECT_BINDING + k,
                   c->uses_gmem_store ? "" : "readonly ", k, k);
        }
        // Each access: the draw's storage buffer chunks first (gd, uniform: no table read, no miss path), then the table.
        static const struct { const char *head, *act, *miss; int last; bool store; } fn[] = {
            {"uint GMR1(uint i)", "return p.w[o];", "return 0u;", 0, false},
            {"uvec2 GMR2(uint i)", "return uvec2(p.w[o], p.w[o + 1u]);", "return uvec2(0u);", 1, false},
            {"uvec4 GMR4(uint i)", "return uvec4(p.w[o], p.w[o + 1u], p.w[o + 2u], p.w[o + 3u]);", "return uvec4(0u);", 3, false},
            {"void GMW1(uint i, uint v)", "p.w[o] = v; return;", "", 0, true},
            {"void GMW2(uint i, uvec2 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; return;", "", 1, true},
            {"void GMW4(uint i, uvec4 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; p.w[o + 2u] = v.z; p.w[o + 3u] = v.w; return;", "", 3, true},
            {"void GMAND1(uint i, uint v)", "atomicAnd(p.w[o], v); return;", "", 0, true},
            {"void GMOR1(uint i, uint v)", "atomicOr(p.w[o], v); return;", "", 0, true},
        };
        for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++) {
            if (fn[f].store && !c->uses_gmem_store) continue;
            sp(&s, "%s { uint o;", fn[f].head);
            if (direct)  // (whole chunks: an aligned access in one never leaves it; the storage buffer, not the address)
                for (int k = 0; k < GMEM_DIRECT_MAX; k++) {
                    char act[192], *d = act;
                    for (const char *a = fn[f].act; *a && d < act + sizeof act - 8; a++) {
                        if (a[0] == 'p' && a[1] == '.') d += snprintf(d, 8, "gdir%d_", k);
                        else *d++ = *a;
                    }
                    *d = 0;
                    sp(&s, " o = i - gd[%d].x; if (o < gd[%d].y) { %s }", k, k, act);
                }
            char lim[16];
            if (fn[f].last) snprintf(lim, sizeof lim, "o + %du", fn[f].last); else snprintf(lim, sizeof lim, "o");
            sp(&s, " uint c = GMC(i); o = GMO(i); uvec4 e = gchunk[c]; if (%s < e.z) { GW p = GW((uint64_t(e.y) << 32) | uint64_t(e.x)); %s } "
                       "gmiss(c, i); %s }\n", lim, fn[f].act, fn[f].miss);
        }
        sp(&s, "#define GMR(i) GMR1(i)\n");
        if (c->uses_gmem_store) sp(&s, "#define GMW(i, v) GMW1(i, v)\n#define GMAND(i, m) GMAND1(i, m)\n#define GMOR(i, m) GMOR1(i, m)\n");
    }
    if (info->uses_gmem_map) {  // (compute) the dispatch's storage buffers, device-local: gmap[i] = heap offset, size, word
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GMap { uvec4 gmap[%d]; };\n", set, GMAP_BUFFER_INDEX, GMAP_MAX + 1);
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GFast { uint w[]; } gfast_;\n", set, GFAST_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "uint gfind(uint g) { uint n = gmap[%d].x; for (uint i = 0u; i < n; i++) { uint o = g - gmap[i].x; "
               "if (g >= gmap[i].x && o < gmap[i].y) return gmap[i].z + (o >> 2); } return 0xFFFFFFFFu; }\n", GMAP_MAX);
        if (c->uses_gmem_store)  // (the stores' lookup: a store inside entry i's bound bytes - gmap[i].w -
                                 //  writes the dispatch's stamp to GFast word GFAST_FLAG_WORD + i; the padding doesn't count)
            sp(&s, "uint gfindw(uint g) { uint n = gmap[%d].x; for (uint i = 0u; i < n; i++) { uint o = g - gmap[i].x; "
                   "if (g >= gmap[i].x && o < gmap[i].y) { if (o >= (gmap[i].w & 0xFFu) && (o >> 2) < (gmap[i].w >> 8)) "
                   "gfast_.w[%uu + i] = gmap[%d].y; return gmap[i].z + (o >> 2); } } "
                   "return 0xFFFFFFFFu; }\n", GMAP_MAX, GFAST_FLAG_WORD, GMAP_MAX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&s, "layout(std140, set = %d, binding = 18) uniform RScale { vec4 rscale[%d]; };\n", set,
           key && key->flip_x ? 66 : 65);  // ([65]: mirror mode's render target width)
        info->uses_rscale = true;
    }
    bool tint = (c->feat & MAXMETAL_FEAT_TINT) && stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&s, "layout(std140, set = %d, binding = 19) uniform Tint { vec4 ltint[4]; };\n", set);
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\n", set, 32 + i, glsl_sampler_type(t), i);
        // size of level l as (width, height, depth or layers)
        switch (t->type) {
        case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1, 1); }\n", i, i); break;
        case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = textureSize(t%d, l); return ivec3(s.x, 1, s.y); }\n", i, i); break;
        case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
            sp(&s, "ivec3 tsize%d(int l) { return textureSize(t%d, l); }\n", i, i); break;
        default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1); }\n", i, i);
        }
    }
    if (stage == STAGE_VERTEX) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "layout(location = %d) in %s a%d;\n", i, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", i);
        }
        // depth pre-pass + EQUAL test needs identical positions from different shaders: invariant, and precise so no
        // compiler fuses or reorders the math feeding it (MSL gets this from preserveInvariance; without it foliage
        // drawn twice flickered black)
        sp(&s, "invariant gl_Position;\nprecise gl_Position;\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "layout(location = %d) out vec4 v%d;\n", i, i);
    } else if (stage == STAGE_FRAGMENT) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? "flat " : (c->noperspective_mask & (1u << i)) ? "noperspective " : "";
            sp(&s, "layout(location = %d) %sin vec4 v%d;\n", i, interp, i);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "layout(location = %d) out %s c%d;\n", rt, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", rt);
        }
        if (c->omap_depth) info->writes_depth = true;
        if (glsl_early_tests(c)) sp(&s, "layout(early_fragment_tests) in;\n");
    } else {
        sp(&s, "layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n");
    }
    sp(&s, "void main() {\n");
    if (stage == STAGE_VERTEX) {
        sp(&s, "  gl_Position = vec4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  v%d = vec4(0, 0, 0, 1);\n", i);
        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");
    }
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0u;\n", i);
    for (int i = 0; i < c->nxs; i++) sp(&s, "  uint xs%d = 0u;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0u; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d]; for (int li_ = 0; li_ < %d; li_++) lmem[li_] = 0u;\n", LMEM_WORDS, LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint crs[32]; uint crs_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0u; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  gl_Position.y = -gl_Position.y;\n");
        if (key && key->flip_x) sp(&s, "  gl_Position.x = -gl_Position.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            const char *cv = kind == 1 ? "int" : kind == 2 ? "uint" : "";
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    if (comp == 3) sp(&s, "  c%d[%d] = %s(1.0f);\n", rt, comp, cv);
                    else sp(&s, "  c%d[%d] = %s(dbgv);\n", rt, comp, cv);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  c%d[%d] = as_f(r%d);\n", rt, comp, r);
                    else sp(&s, "  c%d[%d] = %s(r%d);\n", rt, comp, cv, r);
                } else {
                    sp(&s, "  c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { vec3 g = c0.rgb; float l = dot(g, vec3(0.299f, 0.587f, 0.114f));\n"
                   "    c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "c0", "vec3", "vec4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth = as_f(r%d);\n", r + 1);
            else sp(&s, "  gl_FragDepth = gl_FragCoord.z;\n");
        }
    }
    sp(&s, "}\n");
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

// BRX (indirect branch, Vulkan / GLSL only): label just the targets its jump table can name. A BRX used to make every
// instruction a case of the dispatcher switch (any one may be a target): hundreds of cases falling into each other
// inside the dispatcher loop. Suspected of what AMD's Windows driver never finishes building (the dialogue
// body text's vertex shader stayed "building", its draws skipped - Tarin's name showed, the message stayed blank).
// The compiler puts the table in the program's own constants (cb1, after the code: blob + const_offset), loaded into
// the BRX register by an LDC c[0x1][Rx + table] shortly before: BRX Ra, imm goes to pc + 8 + table[k] + imm. Entries
// are read until one isn't an instruction of this shader (the end of the table; a word past it that still looks like
// one only adds a harmless case). False: not that shape (the register set some other way, no constants, nothing
// valid) - the caller labels every instruction as before. MAXMETAL_BRX_ALL_LABELS=1: always every instruction.
static bool brx_table_labels(const uint8_t *code, size_t i, size_t end, const uint8_t *blob, size_t const_offset, uint8_t *label) {
    static int all = -1;
    if (all < 0) { const char *e = getenv("MAXMETAL_BRX_ALL_LABELS"); all = e && atoi(e) != 0; }
    if (all || const_offset >= 0x10000 || const_offset < 0x80) return false;  // (0x10000: no constants known)
    uint64_t brx = ((const uint64_t *)code)[i];
    int ra = (int)bits(brx, 8, 8);
    int64_t imm = sbits(brx, 20, 24);
    if (ra == RZ) return false;
    // the instruction that set ra: an LDC from cb1 within the few before, nothing else writing ra (or a branch) between
    int64_t table = -1;
    for (size_t k = i, seen = 0; k-- > 0 && seen < 32;) {
        if (k % 4 == 0) continue;  // (scheduling words)
        seen++;
        uint64_t w = ((const uint64_t *)code)[k];
        if ((w & 0xfff0000000000000ull) >= 0xe200000000000000ull && (w & 0xfff0000000000000ull) <= 0xe350000000000000ull)
            return false;  // flow control between: not straight-line
        if ((w & 0xfff8000000000000ull) == 0xef90000000000000ull && (int)bits(w, 0, 8) == ra) {  // LDC ra, c[idx][Rx + off]
            if (bits(w, 36, 5) != 1 || bits(w, 48, 3) != 4) return false;  // (cb1, 32-bit)
            table = (int64_t)bits(w, 20, 16);
            break;
        }
        if ((int)bits(w, 0, 8) == ra) return false;  // (something else may write ra: unknown)
    }
    if (table < 0) return false;
    const uint8_t *tab = blob + const_offset + table;
    uint8_t *mark = calloc(end + 4, 1);
    int n = 0;
    for (int k = 0; k < 256; k++) {
        int32_t v;
        memcpy(&v, tab + 4 * k, 4);
        int64_t t = (int64_t)i * 8 + 8 + v + imm;
        if (t < 8 || (t & 7) || (size_t)(t / 8) >= end) break;
        t = norm_target(t);  // (as the BRX itself does at run time)
        if ((size_t)(t / 8) >= end) break;
        mark[t / 8] = 1;
        n++;
    }
    if (n) for (size_t k = 0; k < end; k++) label[k] |= mark[k];
    free(mark);
    return n > 0;
}

// ------------------------------------------------------------------ the NVN binding contract
// The MSL keeps the shapes the renderer reads and rewrites: one argument buffer
// "constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]" of 8-byte members with dense ids, matched to the
// reflection by id; "struct main0_out\n{\n", "main0_out out = {};", "return out;"; inputs in "main0_in in [[stage_in]]"
// with [[attribute(n)]] / [[user(locnN)]] members; [[color(n)]] outputs; "<tex>.sample(spvDescriptorSet0.<tex>Smplr, ";
// constant buffers read as "(*spvDescriptorSet0.<pfx>_c<n>)._m0[...]". Inputs and outputs are float4 bits: the renderer
// retypes them for the pipeline with as_type.

// The body with texture t<i> / sampler s<i> named by argument-buffer member, and (fragment) in.pos as gl_FragCoord.
static void nvn_rename(Ctx *c, Str *o, const char *src) {
    for (const char *p = src; *p;) {
        if (!id_start(*p) || (p > src && (id_char(p[-1]) || p[-1] == '.'))) {
            if (p > src && id_char(p[-1]) && id_start(*p)) { while (id_char(*p)) sp(o, "%c", *p++); continue; }
            sp(o, "%c", *p++);
            continue;
        }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        bool num = n > 1 && (p[0] == 't' || p[0] == 's');
        for (int i = 1; num && i < n; i++) num = p[i] >= '0' && p[i] <= '9';
        int slot = num ? atoi(p + 1) : -1;
        // only the texture of a member call (t<i>.sample) and the sampler argument (s<i>, ...): the body has locals
        // with such names too
        if (num && (p[0] == 't' ? *e != '.' : !(p > src && p[-1] == '(' && *e == ','))) slot = -1;
        bool ms = num && slot >= 0 && slot < c->info->ntex && p[0] == 't' && c->info->tex[slot].type == TEXTYPE_2D_MS;
        if (ms && !strncmp(e, ".get_num_mip_levels()", 21)) {   // (TXQ before the lookup that made it multisample: one level)
            sp(o, "1u");
            e += 21;
        } else if (num && slot >= 0 && slot < c->info->ntex && p[0] == 't' && !tex_is_depth(c, slot) &&
                   (!strncmp(e, ".sample_compare(", 16) || !strncmp(e, ".gather_compare(", 16))) {
            sp(o, "spvDepthCast(spvDescriptorSet0.%s_t%d)", c->pfx, c->tex_nvn[slot]);   // (a colour texture compared)
        } else if (num && slot >= 0 && slot < c->info->ntex) {
            sp(o, "spvDescriptorSet0.%s_t%d%s", c->pfx, c->tex_nvn[slot], p[0] == 's' ? "Smplr" : "");
            if (ms && (!strncmp(e, ".get_width(lv)", 14) || !strncmp(e, ".get_height(lv)", 15))) {   // (no levels)
                const char *l = strstr(e, "(lv)");
                sp(o, "%.*s()", (int)(l - e), e);
                e = l + 4;
            }
        } else if (c->stage == STAGE_FRAGMENT && n == 2 && !strncmp(p, "in", 2) && !strncmp(e, ".pos", 4) && !id_char(e[4])) {
            sp(o, "gl_FragCoord");
            e += 4;
        } else {
            sp(o, "%.*s", n, p);
        }
        p = e;
    }
}

static const char *nvn_tex_decl(const ShaderTexture *t, bool depth) {
    switch (t->type) {
    case TEXTYPE_1D: return depth ? NULL : "texture1d<float>";
    case TEXTYPE_1D_ARRAY: return depth ? NULL : "texture1d_array<float>";
    case TEXTYPE_3D: return depth ? NULL : "texture3d<float>";
    case TEXTYPE_CUBE: return depth ? "depthcube<float>" : "texturecube<float>";
    case TEXTYPE_CUBE_ARRAY: return depth ? "depthcube_array<float>" : "texturecube_array<float>";
    case TEXTYPE_2D_ARRAY: return depth ? "depth2d_array<float>" : "texture2d_array<float>";
    case TEXTYPE_2D_MS: return depth ? "depth2d_ms<float>" : "texture2d_ms<float>";
    default: return depth ? "depth2d<float>" : "texture2d<float>";
    }
}

static const char *nvn_tex_answer(int type) {  // the request's spelling of a texture type
    switch (type) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_1D_ARRAY: return "1darray";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_CUBE_ARRAY: return "cubearray";
    case TEXTYPE_2D_ARRAY: return "2darray";
    default: return "2d";
    }
}

// Driver-bank words the renderer fills (c0 / c2: as the Ryujinx path's)
static bool nvn_drv_fillable(int stage, int bank, uint32_t off) {
    if (bank == 1) return (off >= 0x20 && off < 0x120) || (off >= 0x168 && off < 0x968);
    if (stage == STAGE_VERTEX) return (off >= 0x110 && off < 0x210) || (off >= 0x610 && off < 0x620);
    if (stage == STAGE_FRAGMENT) return off >= 0x510 && off < 0x610;
    return off >= 0x200 && off < 0x410;
}

// (NVN) a BRX's jump table, as Ryujinx (Decoder.cs FindBrxTargetRange): IMNMX.U32 Rx, Rx, bound, PT; SHL Rx, Rx, 2;
// LDC Ra, c[0x1][Rx + table]; BRX Ra, imm goes to pc + 8 + imm + c1[table + 4 k], k <= bound. The addresses into
// out (at most max); 0: no table found (the writes are looked for in the straight-line code before, no branch between)
static int sbrx(const Ctx *c, const uint64_t *code, size_t i, uint32_t *out, int max) {
    const NvnKey *nk = c->nvn;
    if (!nk || !nk->c1) return 0;
    uint64_t brx = code[i];
    int reg = (int)bits(brx, 8, 8), step = 0;
    int64_t table = 0;
    for (size_t k = i, seen = 0; k-- > 0 && seen < 64;) {
        if (k % 4 == 0) continue;
        seen++;
        uint64_t w = code[k], t = w & 0xfff0000000000000ull;
        if (t >= 0xe200000000000000ull && t <= 0xe350000000000000ull) return 0;
        if (reg == RZ || (int)bits(w, 0, 8) != reg) continue;
        if (step == 0) {  // LDC Ra, c[0x1][Rx + table]
            if ((w & 0xfff8000000000000ull) != 0xef90000000000000ull || bits(w, 36, 5) != 1 || bits(w, 44, 2) != 0) return 0;
            table = sbits(w, 20, 16);
            reg = (int)bits(w, 8, 8);
        } else if (step == 1) {  // SHL Rx, Rx, 2
            if ((w & 0xfef8000000000000ull) != 0x3848000000000000ull || (bits(w, 20, 19) | bits(w, 56, 1) << 19) != 2) return 0;
            reg = (int)bits(w, 8, 8);
        } else {  // IMNMX.U32 Rx, Rx, bound, PT
            if ((w & 0xfef8000000000000ull) != 0x3820000000000000ull || bits(w, 48, 1) || bits(w, 39, 3) != PT || bits(w, 42, 1))
                return 0;
            int n = (int)(bits(w, 20, 19) | bits(w, 56, 1) << 19) + 1;
            if (n > max || table < 0 || (uint64_t)table + 4ull * n > nk->c1_size) return 0;
            int64_t base = (int64_t)i * 8 + 8 + sbits(brx, 20, 24);
            for (int e = 0; e < n; e++) {
                uint32_t v;
                memcpy(&v, nk->c1 + table + 4 * e, 4);
                out[e] = (uint32_t)(base + v);
            }
            return n;
        }
        step++;
    }
    return 0;
}

// ------------------------------------------------------------------ (NVN) structured control flow
// A program whose branch stacks (SSY / PBK / PCNT) resolve statically and whose flow graph is reducible is emitted as
// nested while / do-while(false) / if blocks (Ramsey, "Beyond Relooper", ICFP 2022), as the Ryujinx path's structured
// SPIR-V, instead of the switch (pc) state machine: Metal compiles it several times faster, and lanes reconverge where
// paths join (what the state machine's lock step does by hand). The nodes a loop exits to are placed after the loop
// (Ramsey's would place them inside it, where lanes that left the loop would run them apart from the rest of their
// quad). A branch past the innermost enclosing loop sets brk to the target construct's id (1: exit) and breaks out
// level by level.
// SF_KIL: an unconditional KIL ends its block, as Ryujinx's Discard (the code after it until a label: unreachable)
enum { SF_NONE, SF_BRA, SF_SYNC, SF_BRK, SF_CONT, SF_EXIT, SF_BRX, SF_OTHER, SF_KIL };
#define SB_EXIT (-2)
typedef struct {
    uint32_t first, last;    // instruction words [first, last)
    int term;                // SF_*: the last word's flow
    bool cond;               // the terminator is predicated / tests CC (else: unconditional)
    int succ[2];             // taken, fallthrough: block, SB_EXIT, -1 none
    int nx, *xs;             // BRX: its targets (instead of succ) ...
    uint32_t *xv;            // ... and the addresses that go there
    bool seen, loop, merge;  // reached; target of a back edge; branched to (not placed inline)
    bool seen2;              // (sbuild) its edges set
    int rpo, idom, nfwd;
    int hdr;                 // innermost loop (header block) whose body holds this block, -1 none
    int place;               // the block whose code holds this one's: idom, or a loop it leaves
    bool follow;             // placed after the loop `place` (a loop exit)
    Str code;                // the block's instructions but the terminator
} SBlk;
typedef struct { int kind, blk, id; bool escaped, targeted, tail; } SCons;  // kind 0: block followed by blk, 1: loop headed by blk, 2: exit
typedef struct {
    Ctx *c;
    const uint64_t *code;
    SBlk *b;
    int nb, npost, nst, next_id, depth;
    int *byrpo;
    uint8_t **body;          // per block: the loop body it heads (NULL: not a header)
    bool bad;
    SCons st[64];
    Str out;
} SProg;

static int sflow(uint64_t w) {
    uint64_t t = w & 0xfff0000000000000ull;
    int pr = (int)bits(w, 16, 3);
    if (t == 0xe290000000000000ull || t == 0xe2a0000000000000ull || t == 0xe2b0000000000000ull) return SF_NONE;  // pushes
    if (pr == PT && bits(w, 19, 1)) return SF_NONE;  // @!PT: never executes
    if ((w & 0xfff0000000000020ull) == 0xe240000000000000ull) return SF_BRA;
    if ((w & 0xfff0000000000020ull) == 0xe250000000000000ull) return SF_BRX;
    if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) return SF_SYNC;
    if (t == 0xe340000000000000ull) return SF_BRK;
    if (t == 0xe350000000000000ull) return SF_CONT;
    if (t == 0xe300000000000000ull) return SF_EXIT;
    if (t == 0xe330000000000000ull && pr == PT && !bits(w, 19, 1) && (w & 0x1f) == 0xf) return SF_KIL;
    // BRX, CAL, RET, JMP, PRET, ...: the state machine
    if ((t >= 0xe200000000000000ull && t <= 0xe280000000000000ull) || t == 0xe320000000000000ull) return SF_OTHER;
    return SF_NONE;
}
static bool sflow_cond(uint64_t w) { return !((int)bits(w, 16, 3) == PT && !bits(w, 19, 1)) || (w & 0x1f) != 0xf; }
static uint32_t starget(uint64_t w, uint32_t i) { return norm_target((int64_t)i * 8 + 8 + sbits(w, 20, 24)) / 8; }
static int sdeg(const SBlk *b) { return b->nx ? b->nx : 2; }
static int sget(const SBlk *b, int e) { return b->nx ? b->xs[e] : b->succ[e]; }

// blocks, the targets of their stack pops (SYNC / BRK / CONT), successors
static bool sbuild(SProg *g, size_t end, const uint8_t *label) {
    g->b = calloc(end + 1, sizeof *g->b);
    int *of = malloc((end + 1) * sizeof *of);
    bool cut = true;
    for (size_t i = 0; i < end; i++) {
        of[i] = -1;
        if (i % 4 == 0) continue;
        int f = sflow(g->code[i]);
        if (f == SF_OTHER) { free(of); return false; }
        if (cut || label[i]) g->b[g->nb++].first = (uint32_t)i;
        SBlk *b = &g->b[g->nb - 1];
        of[i] = g->nb - 1;
        b->last = (uint32_t)i + 1;
        b->term = f;
        cut = f != SF_NONE;
    }
    of[end] = -1;
    // The SSY / PBK / PCNT entries on entry, one stack as the state machine's (emit_insn: SYNC / BRK / CONT drop the
    // entries of other kinds above theirs). A block may be reached with different stacks (a BRK out of an SSY region
    // leaves its entry; Ryujinx's decoder: one stack, PropagatePushOp), up to SB_STACKS of them, as long as each of its
    // pops goes to the same target from all of them: the structure follows the edges, not the stacks.
    enum { SB_STACKS = 8 };
    typedef struct { uint8_t n, kind[16]; uint32_t v[16]; } Stk;
    Stk *st = calloc((size_t)g->nb * SB_STACKS, sizeof *st);
    uint8_t *nst = calloc(g->nb + 1, 1);
    int *wl = malloc(((size_t)g->nb * SB_STACKS + 1) * sizeof *wl), nwl = 0;  // block * SB_STACKS + its stack
    bool ok = g->nb > 0;
    if (ok) { g->b[0].seen = true; nst[0] = 1; wl[nwl++] = 0; }
    while (ok && nwl) {
        int k = wl[nwl - 1] / SB_STACKS;
        Stk sk = st[wl[--nwl]];
        SBlk *b = &g->b[k];
        for (uint32_t i = b->first; i < b->last; i++) {
            uint64_t w = g->code[i], t = w & 0xfff0000000000020ull;
            int s = t == 0xe290000000000000ull ? 0 : t == 0xe2a0000000000000ull ? 1 : t == 0xe2b0000000000000ull ? 2 : -1;
            if (i % 4 == 0 || s < 0) continue;
            if (sk.n == 16) { ok = false; break; }
            sk.kind[sk.n] = (uint8_t)s; sk.v[sk.n++] = starget(w, i);
        }
        if (!ok) break;
        uint64_t w = g->code[b->last - 1];
        bool cond = b->term != SF_NONE && sflow_cond(w);
        Stk tk = sk;  // the taken edge's stack (a pop)
        int succ0 = -1, succ1 = -1;
        uint32_t tgt = 0;
        bool has_t = true;
        switch (b->term) {
        case SF_NONE: has_t = false; break;
        case SF_BRA: tgt = starget(w, b->last - 1); break;
        case SF_SYNC: case SF_BRK: case SF_CONT: {
            int s = b->term - SF_SYNC;
            while (tk.n && tk.kind[tk.n - 1] != s) tk.n--;
            if (!tk.n) { ok = false; break; }
            tgt = tk.v[tk.n - 1];
            if (b->term != SF_CONT) tk.n--;
            break;
        }
        case SF_EXIT: case SF_KIL: succ0 = SB_EXIT; has_t = false; break;
        case SF_BRX:
            has_t = false;
            if (!b->nx) {  // (its targets: from the first stack, they do not depend on it)
                uint32_t tv[256];
                int n = cond ? 0 : sbrx(g->c, g->code, b->last - 1, tv, 256);
                if (!n) { ok = false; break; }
                b->xs = malloc(n * sizeof *b->xs); b->xv = malloc(n * sizeof *b->xv);
                b->nx = n;
                for (int e = 0; e < n && ok; e++) {
                    uint32_t t = norm_target(tv[e]) / 8;
                    if (t >= end || of[t] < 0 || g->b[of[t]].first != t) { ok = false; break; }
                    b->xs[e] = of[t]; b->xv[e] = tv[e];
                }
            }
            break;
        }
        if (!ok) break;
        if (has_t) {
            if (tgt >= end || of[tgt] < 0 || g->b[of[tgt]].first != tgt) { ok = false; break; }
            succ0 = of[tgt];
        }
        if (b->term == SF_NONE || cond) succ1 = k + 1 < g->nb ? k + 1 : SB_EXIT;
        if (!b->seen2) {
            b->seen2 = true;
            b->cond = cond && succ0 != succ1;
            b->succ[0] = succ0; b->succ[1] = succ0 == succ1 ? -1 : succ1;  // (both edges to one block: one edge)
        } else if (b->succ[0] != succ0 || b->succ[1] != (succ0 == succ1 ? -1 : succ1)) {
            ok = false;  // (another stack's pop goes elsewhere)
        }
        for (int e = 0; e < (b->nx ? b->nx : 2) && ok; e++) {
            int y = b->nx ? b->xs[e] : e == 0 ? succ0 : succ1;
            if (y < 0) continue;
            const Stk *yk = e == 0 && !b->nx ? &tk : &sk;
            SBlk *t = &g->b[y];
            t->seen = true;
            bool have = false;
            for (int q = 0; q < nst[y] && !have; q++) {
                const Stk *o = &st[(size_t)y * SB_STACKS + q];
                have = o->n == yk->n && !memcmp(o->kind, yk->kind, yk->n) && !memcmp(o->v, yk->v, yk->n * sizeof *yk->v);
            }
            if (have) continue;
            if (nst[y] == SB_STACKS) { ok = false; break; }
            st[(size_t)y * SB_STACKS + nst[y]] = *yk;
            wl[nwl++] = y * SB_STACKS + nst[y]++;
        }
    }
    free(st); free(nst);
    // a KIL block only one conditional branch reaches, as the code it skips (between the branch and its forward target),
    // goes on to the target: Ryujinx's structure if (p) { discard } target... (its Discard ends a block with no successor;
    // a KIL block that is the target, after the code the branch falls into, stays apart: if (p) { ... } discard)
    int *np = calloc(g->nb, sizeof *np), *pf = malloc(g->nb * sizeof *pf);
    for (int x = 0; ok && x < g->nb; x++)
        for (int e = 0; g->b[x].seen && e < sdeg(&g->b[x]); e++) {
            int y = sget(&g->b[x], e);
            if (y >= 0) { np[y]++; pf[y] = x; }
        }
    for (int k = 0; ok && k < g->nb; k++) {
        SBlk *b = &g->b[k], *a = np[k] == 1 ? &g->b[pf[k]] : NULL;
        if (!b->seen || b->term != SF_KIL || !a || a->term != SF_BRA || !a->cond || a->succ[1] != k || a->succ[0] < 0 ||
            g->b[a->succ[0]].first <= b->first) continue;
        b->succ[0] = a->succ[0];
    }
    free(np); free(pf);
    free(wl); free(of);
    return ok;
}

static bool sdom(const SProg *g, int a, int x) {  // a dominates x
    while (x != a && x != 0) x = g->b[x].idom;
    return x == a;
}

// reverse postorder, dominators (Cooper, Harvey, Kennedy), loops and their bodies, where each block is placed; an
// irreducible graph: false
static bool sanalyze(SProg *g) {
    int nb = g->nb, *post = malloc(nb * sizeof *post), *stk = malloc(nb * sizeof *stk), *it = calloc(nb, sizeof *it);
    uint8_t *vis = calloc(nb, 1);
    int npost = 0, ns = 0;
    stk[ns++] = 0; vis[0] = 1;
    while (ns) {  // iterative DFS: postorder
        int x = stk[ns - 1];
        if (it[x] < sdeg(&g->b[x])) {
            int y = sget(&g->b[x], it[x]++);
            if (y >= 0 && !vis[y]) { vis[y] = 1; stk[ns++] = y; }
            continue;
        }
        post[npost++] = x;
        ns--;
    }
    g->npost = npost;
    g->byrpo = malloc(npost * sizeof *g->byrpo);
    for (int i = 0; i < nb; i++) { g->b[i].rpo = -1; g->b[i].idom = -1; g->b[i].hdr = -1; }
    for (int i = 0; i < npost; i++) { g->byrpo[i] = post[npost - 1 - i]; g->b[g->byrpo[i]].rpo = i; }
    // predecessors (reachable), as lists
    int *pn = calloc(nb + 1, sizeof *pn), *pl;
    for (int x = 0; x < nb; x++)
        for (int e = 0; e < sdeg(&g->b[x]); e++)
            if (g->b[x].rpo >= 0 && sget(&g->b[x], e) >= 0) pn[sget(&g->b[x], e) + 1]++;
    for (int x = 0; x < nb; x++) pn[x + 1] += pn[x];
    pl = malloc((pn[nb] + 1) * sizeof *pl);
    int *fill = calloc(nb, sizeof *fill);
    for (int x = 0; x < nb; x++)
        for (int e = 0; e < sdeg(&g->b[x]); e++)
            if (g->b[x].rpo >= 0 && sget(&g->b[x], e) >= 0) { int y = sget(&g->b[x], e); pl[pn[y] + fill[y]++] = x; }
    g->b[0].idom = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (int r = 1; r < npost; r++) {
            int x = g->byrpo[r], nd = -1;
            for (int q = pn[x]; q < pn[x + 1]; q++) {
                int p = pl[q];
                if (g->b[p].idom < 0) continue;
                if (nd < 0) { nd = p; continue; }
                int a = p, b = nd;
                while (a != b) {
                    while (g->b[a].rpo > g->b[b].rpo) a = g->b[a].idom;
                    while (g->b[b].rpo > g->b[a].rpo) b = g->b[b].idom;
                }
                nd = a;
            }
            if (nd >= 0 && g->b[x].idom != nd) { g->b[x].idom = nd; changed = true; }
        }
    }
    bool ok = true;
    g->body = calloc(nb, sizeof *g->body);
    for (int r = 0; r < npost && ok; r++) {
        int x = g->byrpo[r];
        for (int e = 0; e < sdeg(&g->b[x]); e++) {
            int y = sget(&g->b[x], e);
            if (y < 0) continue;
            if (g->b[y].rpo <= g->b[x].rpo) {
                if (!sdom(g, y, x)) { ok = false; break; }
                g->b[y].loop = true;
            } else if (++g->b[y].nfwd >= 2) g->b[y].merge = true;
        }
    }
    // loop bodies (the blocks that reach a back edge without passing the header); outer loops first, so a block's hdr
    // ends as its innermost loop
    int *wl = malloc((nb + 1) * sizeof *wl);
    for (int r = 0; r < npost && ok; r++) {
        int h = g->byrpo[r];
        if (!g->b[h].loop) continue;
        uint8_t *bd = g->body[h] = calloc(nb, 1);
        int nw = 0;
        bd[h] = 1;
        for (int q = pn[h]; q < pn[h + 1]; q++)
            if (g->b[pl[q]].rpo >= g->b[h].rpo && !bd[pl[q]]) { bd[pl[q]] = 1; wl[nw++] = pl[q]; }
        while (nw) {
            int x = wl[--nw];
            for (int q = pn[x]; q < pn[x + 1]; q++)
                if (!bd[pl[q]]) { bd[pl[q]] = 1; wl[nw++] = pl[q]; }
        }
        for (int x = 0; x < nb; x++)
            if (bd[x]) g->b[x].hdr = h;
    }
    // placement: a block outside the loop its dominator is in goes after that loop (the outermost such loop it leaves)
    for (int r = 1; r < npost && ok; r++) {
        int y = g->byrpo[r], p = g->b[y].idom;
        for (int L = g->b[p].hdr; L >= 0 && !g->body[L][y];) {
            p = L;
            g->b[y].follow = true;
            int ph = -1;  // the loop around L
            for (int k = L == 0 ? -1 : g->b[L].idom; k >= 0; k = k == 0 ? -1 : g->b[k].idom)
                if (g->body[k] && g->body[k][L]) { ph = k; break; }
            L = ph;
        }
        g->b[y].place = p;
        if (g->b[y].follow) g->b[y].merge = true;
    }
    free(wl); free(pn); free(pl); free(fill); free(post); free(stk); free(it); free(vis);
    return ok;
}

static void stree(SProg *g, int x);

static void sjump(SProg *g, int kind, int blk) {
    int j = g->nst - 1, top = g->nst - 1;
    while (j >= 0 && !(g->st[j].kind == kind && (kind == 2 || g->st[j].blk == blk))) j--;
    if (j < 0) { g->bad = true; return; }
    // a loop that ends its block (the follow node's): breaking out of it reaches the block's end
    if (j == top || (j == top - 1 && g->st[top].tail)) { sp(&g->out, kind == 1 ? "    continue;\n" : "    break;\n"); return; }
    sp(&g->out, "    brk = %du; break;\n", g->st[j].id);
    g->st[j].targeted = true;
    for (int k = j + 1; k <= top; k++) g->st[k].escaped = true;
}

static void sopen(SProg *g, int kind, int blk, bool tail) {
    if (g->nst == 64) { g->bad = true; return; }
    g->st[g->nst++] = (SCons){kind, blk, g->next_id++, false, false, tail};
    sp(&g->out, kind == 1 ? "  while (true) {\n" : "  do {\n");
}

static void sclose(SProg *g) {
    if (!g->nst) { g->bad = true; return; }
    SCons e = g->st[--g->nst];
    sp(&g->out, e.kind == 1 ? "  }\n" : "  } while (false);\n");
    if (!e.escaped || !g->nst) return;
    SCons *p = &g->st[g->nst - 1];
    if (p->targeted) sp(&g->out, "  if (brk != 0u) { if (brk == %du) { brk = 0u; %s; } break; }\n", p->id, p->kind == 1 ? "continue" : "break");
    else sp(&g->out, "  if (brk != 0u) break;\n");
}

static void sbranch(SProg *g, int x, int y) {
    if (y == SB_EXIT) sjump(g, 2, 0);
    else if (g->b[y].rpo <= g->b[x].rpo) sjump(g, 1, y);
    else if (g->b[y].merge) sjump(g, 0, y);
    else stree(g, y);
}

static void scode(SProg *g, int x) {
    SBlk *b = &g->b[x];
    if (b->code.buf) sp(&g->out, "%s", b->code.buf);
    if (b->nx) {  // BRX: the address against each target's (the table's index is clamped: the last one, the rest)
        Ctx *c = g->c;
        uint64_t w = g->code[b->last - 1];
        sp(&g->out, "  { uint brx_ = %s + 0x%08xu;\n", R((int)bits(w, 8, 8)), (uint32_t)((int64_t)(b->last - 1) * 8 + 8 + sbits(w, 20, 24)));
        int done = 0, last = b->nx - 1;
        while (last > 0) {  // (the last distinct address)
            bool dup = false;
            for (int k = 0; k < last; k++) dup |= b->xv[k] == b->xv[last];
            if (!dup) break;
            last--;
        }
        for (int e = 0; e < b->nx; e++) {
            bool dup = false;
            for (int k = 0; k < e; k++) dup |= b->xv[k] == b->xv[e];
            if (dup) continue;
            if (e < last) sp(&g->out, "  %sif (brx_ == 0x%08xu) {\n", done++ ? "} else " : "", b->xv[e]);
            else sp(&g->out, done++ ? "  } else {\n" : "  {\n");
            sbranch(g, x, b->xs[e]);
            if (e >= last) break;
        }
        sp(&g->out, "  }\n  }\n");
        return;
    }
    if (!b->cond) { sbranch(g, x, b->succ[b->succ[0] != -1 ? 0 : 1]); return; }
    Ctx *c = g->c;
    uint64_t w = g->code[b->last - 1];
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1), guarded = !(pr == PT && !prn), ccflow = (w & 0x1f) != 0xf;
    sp(&g->out, "  if (%s%s%s) {\n", guarded ? P(pr, prn) : "", guarded && ccflow ? " && " : "", ccflow ? cc_test(c, (int)(w & 0x1f)) : "");
    sbranch(g, x, b->succ[0]);
    sp(&g->out, "  } else {\n");
    sbranch(g, x, b->succ[1]);
    sp(&g->out, "  }\n");
}

// x's code, inside blocks ending at the merge nodes ys (by rpo: the last one outermost)
static void swithin(SProg *g, int x, const int *ys, int m) {
    if (!m) { scode(g, x); return; }
    sopen(g, 0, ys[m - 1], false);
    swithin(g, x, ys, m - 1);
    sclose(g);
    stree(g, ys[m - 1]);
}

static void stree(SProg *g, int x) {
    if (g->bad || g->c->fail) return;
    if (++g->depth > 4000) { g->bad = true; return; }
    int *in = malloc(g->npost * 2 * sizeof *in), *out = in + g->npost, ni = 0, no = 0;
    for (int r = 0; r < g->npost; r++) {  // the merge nodes placed in x: inside its loop, or after it
        int y = g->byrpo[r];
        if (y == x || g->b[y].place != x || !g->b[y].merge) continue;
        if (g->b[y].follow) out[no++] = y;
        else in[ni++] = y;
    }
    if (g->b[x].loop) {
        for (int k = no; k > 0; k--) sopen(g, 0, out[k - 1], false);
        sopen(g, 1, x, no > 0);
        swithin(g, x, in, ni);
        sclose(g);
        for (int k = 0; k < no; k++) { sclose(g); stree(g, out[k]); }
    } else {
        swithin(g, x, in, ni);
    }
    free(in);
    g->depth--;
}

static void semit(SProg *g) {
    g->out = (Str){0};
    g->nst = 0;
    g->next_id = 1;
    g->depth = 0;
    sopen(g, 2, 0, false);
    stree(g, 0);
    sclose(g);
}

// (nvn_gflow) the origin of a value written as `x` (n chars): a copy of a register; the high word of a storage / uniform
// slot's c0 entry {address, size}, or of an address in a user uniform block (c3-c16: a slot of c->xcb / xoff, added),
// alone or + 0 + carry (IADD.X after the low word's IADD); else unknown
static GEv gflow_origin(Ctx *c, const char *x, size_t n) {
    GEv e = {.kind = 0, .src = -1, .mask = GF_UNKNOWN};
    int r, used;
    if (sscanf(x, "r%d%n", &r, &used) == 1 && (size_t)used == n) { e.src = r; return e; }
    static const char rd[] = "(*spvDescriptorSet0.";
    const char *p = NULL;
    for (size_t i = 0; i + 6 <= n; i++) {  // the one constant read: "_c<n>)._m0["
        if (memcmp(x + i, ")._m0[", 6)) continue;
        size_t j = i;
        while (j > 0 && isdigit((unsigned char)x[j - 1])) j--;
        if (j == i || j < 2 || memcmp(x + j - 2, "_c", 2)) continue;
        if (p) return e;
        p = x + j - 2;
    }
    unsigned k, col;
    int cb, tail;
    if (!p || sscanf(p, "_c%d)._m0[%uu][%u]%n", &cb, &k, &col, &tail) != 3 || col > 3) return e;
    if (cb == 1 || cb > 16 || (cb != 2 && !(col & 1))) return e;
    const char *q = p;  // the read's start
    while (q > x && memcmp(q, rd, sizeof rd - 1)) q--;
    if (memcmp(q, rd, sizeof rd - 1)) return e;
    char rest[64];
    int m = 0;
    for (const char *s = x; s < x + n && m < 63; s++) {
        if (s == q) s = p + tail;
        if (s < x + n && *s != ' ') rest[m++] = *s;
    }
    rest[m] = 0;
    if (strcmp(rest, "") && strcmp(rest, "(0u++cc)")) return e;
    if (cb == 2) {  // an image handle: image slot n's entry in the driver bank (c2 + 0x120 + 8n), copied as it is
        uint32_t byte = k * 16 + col * 4;
        if (strcmp(rest, "") || byte < 0x120 || byte >= 0x120 + 8 * 15 || (byte - 0x120) % 8) return e;
        e.mask = 1ull << (49 + (byte - 0x120) / 8);
        return e;
    }
    if (cb) {
        uint16_t lo = (uint16_t)(k * 16 + (col - 1) * 4);
        int i = 0;
        while (i < c->nx && (c->xcb[i] != cb || c->xoff[i] != lo)) i++;
        if (i == c->nx) {
            if (c->nx == 16) return e;
            c->xcb[c->nx] = (uint8_t)cb; c->xoff[c->nx++] = lo;
        }
        e.mask = 1ull << (33 + i);
        return e;
    }
    if (col != 1) return e;
    uint32_t off = k * 16, sbase = c->stage == STAGE_VERTEX ? 0x110 : c->stage == STAGE_FRAGMENT ? 0x510 : 0x310;
    if (c->stage == STAGE_COMPUTE && off >= 0x210 && off < 0x310) e.mask = 1ull << (16 + ((off - 0x210) >> 4));
    else if (off >= sbase && off < sbase + 0x100) e.mask = 1ull << ((off - sbase) >> 4);
    return e;
}

// (nvn_gflow) the register and predicate writes in one instruction's code s (n chars): every "r<i> = <value>;" and
// "p<i> = ". Code with a branch or loop in it may skip them; a predicated instruction's own "if (p0) { ... }" alone
// makes them happen when its predicate holds.
static void gflow_writes(Ctx *c, const char *s, size_t n) {
    int nif = 0;
    bool loop = false;
    for (size_t i = 0; i + 4 <= n; i++) {
        nif += !memcmp(s + i, "if (", 4);
        loop |= (i + 5 <= n && !memcmp(s + i, "for (", 5)) || (i + 7 <= n && !memcmp(s + i, "while (", 7));
    }
    // pass 0: whether it writes a predicate (then its register writes get no guard: which value of the predicate);
    // pass 1: the events
    bool pw = false;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i + 1 < n; i++) {
            if ((s[i] != 'r' && s[i] != 'p') || !isdigit((unsigned char)s[i + 1]) ||
                (i && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '.'))) continue;
            size_t j = i + 1;
            int reg = 0;
            while (j < n && isdigit((unsigned char)s[j])) reg = reg * 10 + (s[j++] - '0');
            if (j + 3 > n || memcmp(s + j, " = ", 3)) continue;
            if (s[i] == 'p') {
                pw = true;
                if (pass) gev_push(c, (GEv){.blk = c->gblk, .kind = 2, .reg = reg});
                continue;
            }
            size_t a = j + 3, e = a;
            for (int depth = 0; e < n && !(s[e] == ';' && depth == 0); e++)
                depth += s[e] == '(' || s[e] == '[' ? 1 : s[e] == ')' || s[e] == ']' ? -1 : 0;
            if (pass && reg < 256) {
                GEv w = gflow_origin(c, s + a, e - a);
                w.blk = c->gblk; w.reg = reg; w.cond = nif || loop;
                w.pred = c->gpred >= 0 && nif == 1 && !loop && !pw ? c->gpred : -1;
                gev_push(c, w);
            }
            i = e;
        }
}

// an instruction's predicate for nvn_gflow: -1 none (PT), else the index + 8 when negated
static int gflow_pred(uint64_t w) {
    int pr = (int)bits(w, 16, 3), ng = (int)bits(w, 19, 1);
    return pr == PT && !ng ? -1 : pr | ng << 3;
}

// c->qshf after an instruction's code s: a register it writes no longer holds a shuffle, nor does one shuffled from it;
// then the shuffle it wrote. Nor gl_FragCoord.w or 1 / w (c->wstate), unless this instruction (pc) made it that.
static void qshf_writes(Ctx *c, const char *s, size_t n, uint32_t pc) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] != 'r' || !isdigit((unsigned char)s[i + 1]) ||
            (i && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '.'))) continue;
        size_t j = i + 1;
        int reg = 0;
        while (j < n && isdigit((unsigned char)s[j])) reg = reg * 10 + (s[j++] - '0');
        if (j + 3 > n || memcmp(s + j, " = ", 3) || reg >= 256) continue;
        if (c->wdef[reg] != pc) c->wstate[reg] = 0;
        c->qshf[reg].k = 0;
        for (int r = 0; r < 256; r++)
            if (c->qshf[r].src == reg) c->qshf[r].k = 0;
    }
    if (c->qnew_k) c->qshf[c->qnew_d] = (typeof(c->qshf[0])){.src = (uint8_t)c->qnew_src, .k = (uint8_t)c->qnew_k};
}

// c->xm after an instruction's code s: a register it writes holds no XMAD part (and its version goes up); then the part
// it wrote
static void xm_writes(Ctx *c, const char *s, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] != 'r' || !isdigit((unsigned char)s[i + 1]) ||
            (i && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '.'))) continue;
        size_t j = i + 1;
        int reg = 0;
        while (j < n && isdigit((unsigned char)s[j])) reg = reg * 10 + (s[j++] - '0');
        if (j + 3 > n || memcmp(s + j, " = ", 3) || reg >= 256) continue;
        c->xm[reg].kind = 0;
        c->ver[reg]++;
    }
    if (c->xnew.kind) c->xm[c->xnew_d] = c->xnew;
}

// emit_insn, noting the instruction's writes for nvn_gflow while c->gtrack
static void gflow_insn(Ctx *c, uint64_t w, uint32_t pc) {
    size_t at = c->body.len;
    c->gpred = gflow_pred(w);
    c->qnew_k = 0;
    c->xnew.kind = 0;
    emit_insn(c, w, pc);
    if (c->gtrack || c->itrack) gflow_writes(c, c->body.buf + at, c->body.len - at);
    if (c->nvn && c->stage == STAGE_FRAGMENT) qshf_writes(c, c->body.buf + at, c->body.len - at, pc);
    xm_writes(c, c->body.buf + at, c->body.len - at);
}

// MAXMETAL_IACC(<id>) in code -> the view of the image slot whose c2 entry the image op's handle comes from (acc[id]: its
// origins), noting the view (c->img) and how it is used
static void iacc_resolve(Ctx *c, Str *code, const uint64_t *acc) {
    if (!code->buf || !strstr(code->buf, "MAXMETAL_IACC(")) return;
    Str o = {0};
    const char *p = code->buf, *q;
    while ((q = strstr(p, "MAXMETAL_IACC("))) {
        int id = atoi(q + strlen("MAXMETAL_IACC(")), k = 0;
        uint64_t m = acc[id];
        while (k < c->niop && c->iop[k].id != id) k++;
        sp(&o, "%.*s", (int)(q - p), p);
        if (k == c->niop || !m || (m & ((1ull << 49) - 1)) || (m & (m - 1))) { fail(c, "bindless image handle not from one image slot"); sp(&o, "t0"); }
        else {
            int slot = __builtin_ctzll(m) - 49, v = 0;
            while (v < c->nimg && (c->img[v].slot != slot || strcmp(c->img[v].fmt, c->iop[k].fmt))) v++;
            if (v == c->nimg) {
                if (c->nimg == 16) { fail(c, "too many image views"); v = 0; }
                else c->img[c->nimg++] = (typeof(c->img[0])){.slot = slot, .elem = c->iop[k].elem, .fmt = c->iop[k].fmt};
            }
            if (c->iop[k].store) c->img[v].write = true; else c->img[v].read = true;
            sp(&o, "spvDescriptorSet0.%s_i%d_%s", c->pfx, slot, c->iop[k].fmt);
        }
        p = strchr(q, ')') + 1;
    }
    sp(&o, "%s", p);
    free(code->buf);
    *code = o;
}

// Which slots each global memory access's address can come from: the origins of its high word over every path to it
// (the registers start unknown; blocks no path reaches: unknown). Within a block a predicated write is also known as
// the value under its predicate (guard), for accesses under the same one until the predicate changes. Then each
// MAXMETAL_GACC in the blocks' code becomes the access to its one slot (MAXMETAL_GAT), tests of its slots (MAXMETAL_GIN), or the
// search (MAXMETAL_GFIND) when an origin is unknown.
static void nvn_gflow(SProg *g) {
    Ctx *c = g->c;
    int nb = g->nb, guard[256];
    uint64_t *in = calloc((size_t)nb * 256, sizeof *in), st[256], gst[256], *acc = calloc(c->ngacc + 1, sizeof *acc);
    int *ev0 = calloc(nb + 1, sizeof *ev0);  // each block's events: [ev0[b], ev0[b + 1])
    for (int i = 0; i < c->ngev; i++) ev0[c->gev[i].blk + 1]++;
    for (int b = 0; b < nb; b++) ev0[b + 1] += ev0[b];
    for (int r = 0; r < 256; r++) in[r] = GF_UNKNOWN;
    // passes over the blocks to the fixed point (the origins only grow), then one more that reads the accesses
    for (bool changed = true, last = false; !last;) {
        last = !changed;
        changed = false;
        for (int b = 0; b < nb; b++) {
            if (!g->b[b].seen) continue;
            memcpy(st, in + (size_t)b * 256, sizeof st);
            for (int r = 0; r < 256; r++) guard[r] = -1;
            for (int i = ev0[b]; i < ev0[b + 1]; i++) {
                const GEv *e = &c->gev[i];
                if (e->kind == 1) {
                    acc[e->id] = e->pred >= 0 && guard[e->reg] == e->pred ? gst[e->reg] : st[e->reg];
                } else if (e->kind == 2) {
                    for (int r = 0; r < 256; r++)
                        if (guard[r] >= 0 && (guard[r] & 7) == e->reg) guard[r] = -1;
                } else {
                    uint64_t v = e->src >= 0 ? st[e->src] : e->mask;
                    st[e->reg] = e->cond ? st[e->reg] | v : v;
                    guard[e->reg] = e->pred;
                    gst[e->reg] = v;
                }
            }
            for (int x = 0; x < sdeg(&g->b[b]); x++) {
                int y = sget(&g->b[b], x);
                if (y < 0) continue;
                for (uint64_t *t = in + (size_t)y * 256, r = 0; r < 256; r++)
                    if ((t[r] | st[r]) != t[r]) { t[r] |= st[r]; changed = true; }
            }
        }
    }
    for (int b = 0; b < nb; b++) {  // MAXMETAL_GACC(ga, <id>) -> the tests
        Str *code = &g->b[b].code, o = {0};
        if (!code->buf || (!strstr(code->buf, "MAXMETAL_GACC(") && !strstr(code->buf, "MAXMETAL_IACC("))) continue;
        const char *p = code->buf, *q;
        // origin bit -> the slot's name: s<k> / u<k> (c0 entries), x<i> (a user uniform block's address)
        #define GSLOT(k) (k) < 16 ? 's' : (k) < 32 ? 'u' : 'x', (k) < 32 ? (k) & 15 : (k) - 33
        while ((q = strstr(p, "MAXMETAL_GACC(ga, "))) {
            uint64_t m = acc[atoi(q + strlen("MAXMETAL_GACC(ga, "))];
            sp(&o, "%.*s", (int)(q - p), p);
            if (!m || (m & GF_UNKNOWN) || (m >> 49)) { sp(&o, "MAXMETAL_GFIND(ga)"); m = 0; }   // (an image handle: no address)
            else if (!(m & (m - 1))) sp(&o, "MAXMETAL_GAT(ga, %c%d)", GSLOT(__builtin_ctzll(m)));
            else {
                sp(&o, "MAXMETAL_GDECL");
                for (int k = 0, n = 0; k < 64; k++)
                    if (m & (1ull << k)) sp(&o, "%s MAXMETAL_GIN(ga, %c%d)", n++ ? " else" : "", GSLOT(k));
                sp(&o, " MAXMETAL_GOK");
            }
            if (m && !(m & GF_UNKNOWN)) c->xused |= (uint32_t)(m >> 33) & 0xFFFF;
        #undef GSLOT
            p = strchr(q, ')') + 1;
        }
        sp(&o, "%s", p);
        free(code->buf);
        *code = o;
        iacc_resolve(c, code, acc);
    }
    free(in); free(acc); free(ev0);
}

// (nvn_iflow) the origin of reg just before event `at` (under predicate pred): its last write in the block, when that
// write surely happened (no condition; or the same predicate, not written since); else org[reg]
static uint64_t iflow_at(const Ctx *c, const uint64_t *org, int at, int reg, int pred, int depth) {
    bool pchanged = false;
    for (int i = at - 1; i >= 0 && c->gev[i].blk == c->gev[at].blk; i--) {
        const GEv *e = &c->gev[i];
        if (e->kind == 2 && pred >= 0 && e->reg == (pred & 7)) pchanged = true;
        if (e->kind != 0 || e->reg != reg) continue;
        if (e->cond || (e->pred >= 0 && (e->pred != pred || pchanged))) break;
        if (e->src < 0) return e->mask;
        return depth < 8 ? iflow_at(c, org, i, e->src, e->pred >= 0 ? e->pred : pred, depth + 1) : org[e->src];
    }
    return org[reg];
}

// (state machine: its blocks only start at labels) the image ops' handles: the handle's last write in the block
// (iflow_at), else every write of the registers it can come from, whatever the path (a register's origin the OR of
// all its writes' (a copy: its source's), to the fixed point)
static void nvn_iflow(Ctx *c) {
    uint64_t org[256] = {0}, *acc = calloc(c->ngacc + 1, sizeof *acc);
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < c->ngev; i++) {
            const GEv *e = &c->gev[i];
            if (e->kind != 0) continue;
            uint64_t v = e->src >= 0 ? org[e->src] : e->mask;
            if ((org[e->reg] | v) != org[e->reg]) { org[e->reg] |= v; changed = true; }
        }
    }
    for (int i = 0; i < c->ngev; i++)
        if (c->gev[i].kind == 1) acc[c->gev[i].id] = iflow_at(c, org, i, c->gev[i].reg, c->gev[i].pred, 0);
    iacc_resolve(c, &c->body, acc);
    free(acc);
}

// a branch target's wstate on entry: the registers every block going there (all translated before it: no back edge)
// leaves holding the same IPA / MUFU.RCP result, one definition on every path as Ryujinx's SSA sees it. A KIL block's
// edge does not count: Ryujinx's Discard ends its block with no successor (sbuild).
static void swstate(const SProg *g, int k, uint8_t (*ws)[256], uint32_t (*wd)[256], Ctx *c) {
    bool any = false;
    memset(c->wstate, 0, sizeof c->wstate);
    for (int x = 0; x < g->nb; x++) {
        const SBlk *p = &g->b[x];
        bool edge = false;
        for (int e = 0; p->seen && p->term != SF_KIL && e < sdeg(p); e++) edge |= sget(p, e) == k;
        if (!edge) continue;
        if (x >= k) { memset(c->wstate, 0, sizeof c->wstate); return; }
        for (int r = 0; r < 256; r++)
            if (!any) { c->wstate[r] = ws[x][r]; c->wdef[r] = wd[x][r]; }
            else if (c->wstate[r] != ws[x][r] || c->wdef[r] != wd[x][r]) c->wstate[r] = 0;
        any = true;
    }
}

// the body as structured code into c->body; false (nothing emitted): the caller uses the state machine
static bool nvn_structure(Ctx *c, const uint64_t *code, size_t end, const uint8_t *label, bool annotate) {
    SProg g = {.c = c, .code = code};
    bool ok = sbuild(&g, end, label) && sanalyze(&g);
    if (ok) {  // the structure first (its code does not depend on the blocks' code)
        semit(&g);
        ok = !g.bad && !c->fail;
        free(g.out.buf);
    }
    if (ok) {
        Str body = c->body;
        c->gtrack = true;
        uint8_t (*ws)[256] = calloc(g.nb, sizeof *ws);
        uint32_t (*wd)[256] = calloc(g.nb, sizeof *wd);
        for (int k = 0; k < g.nb && !c->fail; k++) {  // every block in address order (as the state machine)
            SBlk *b = &g.b[k];
            c->gblk = k;
            c->body = (Str){0};
            if (label[b->first]) { swstate(&g, k, ws, wd, c); memset(c->qshf, 0, sizeof c->qshf); memset(c->xm, 0, sizeof c->xm); }
            for (uint32_t i = b->first; i < b->last; i++) {
                if (i % 4 == 0) continue;
                uint64_t w = code[i], t = w & 0xfff0000000000020ull;
                if (annotate) sp(&c->body, "    // pc %x: %016llx\n", i * 8, (unsigned long long)w);
                c->info->num_insts++;
                if (i == b->last - 1 && b->term != SF_NONE && b->term != SF_KIL) continue;  // the terminator: the structure
                if (t == 0xe290000000000000ull || t == 0xe2a0000000000000ull || t == 0xe2b0000000000000ull) continue;
                gflow_insn(c, w, i * 8);
            }
            b->code = c->body;
            memcpy(ws[k], c->wstate, sizeof c->wstate); memcpy(wd[k], c->wdef, sizeof c->wdef);
        }
        free(ws); free(wd);
        c->body = body;
        c->gtrack = false;
        if (!c->fail) nvn_gflow(&g);
        free(c->gev); c->gev = NULL; c->ngev = c->cap_gev = 0;
        semit(&g);
        if (!c->fail && !g.bad) {
            if (strstr(g.out.buf, "brk")) sp(&c->body, "    uint brk = 0u;\n");
            sp(&c->body, "%s", g.out.buf);
        } else if (!c->fail) fail(c, "structured: emission");
        free(g.out.buf);
    }
    for (int k = 0; k < g.nb; k++) { free(g.b[k].code.buf); free(g.b[k].xs); free(g.b[k].xv); if (g.body) free(g.body[k]); }
    free(g.body); free(g.byrpo); free(g.b);
    return ok;
}

// whether src names `name` (a name that starts there: not the tail of a longer one)
static bool nvn_names(const char *src, const char *name) {
    if (!src) return false;
    for (const char *p = strstr(src, name); p; p = strstr(p + 1, name))
        if (p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_')) return true;
    return false;
}

// The prelude helpers the source after `at` uses, inserted at `at` (each one costs the Metal compiler time whether or
// not it is called).
static void nvn_helpers(Str *s, size_t at) {
    static const struct { const char *name, *def; } h[] = {
        {"bfe_u(", "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n"},
        {"bfe_s(", "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n"},
        {"bfi(", "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n"},
        {"fswzadd(", "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n"},
        {"rsc(", "static inline uint2 rsc(uint2 c, float4 s) { return c; }\n"},
        {"uvs(", "static inline float2 uvs(float2 uv, float4 s) { return uv; }\n"},
        {"hx(", "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n"},
        {"hpack(", "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); half2 o = as_type<half2>(old), n = half2(r); if (merge == 2u) o.x = n.x; else if (merge == 3u) o.y = n.y; else o = n; return as_type<uint>(o); }\n"},
        {"prmt(", "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n"},
        // 1 / x, bit for bit (all 2^32 x) in a third of the GPU code: Metal's 1.0f / x in safe math is a
        // general correctly rounded division; the GPU's reciprocal is off by an ulp at most, which one fma step corrects
        {"maxmetal_rcp(", "static inline float maxmetal_rcp(float x) { float r = fast::divide(1.0f, x), e = fma(fma(-x, r, 1.0f), r, r); return isfinite(e) ? e : r; }\n"},
        {"maxmetal_nz(", "template<typename T> static inline T maxmetal_nz(T v) { return select(v, T(0.0f), isnan(v)); }\n"},
        {"maxmetal_d16ref(", "static inline float maxmetal_d16ref(float r) { return floor(fast::clamp(r, 0.0f, 1.0f) * 65535.0f + 0.5f) * (1.0f / 65535.0f); }\n"},
        // (SPIRV-Cross's) a compare of a colour texture: its handle as the depth texture
        {"spvDepthCast(", "template <typename T> static inline depth2d<T> spvDepthCast(texture2d<T> t) { return reinterpret_cast<thread const depth2d<T> &>(t); }\n"
                          "template <typename T> static inline depth2d_array<T> spvDepthCast(texture2d_array<T> t) { return reinterpret_cast<thread const depth2d_array<T> &>(t); }\n"
                          "template <typename T> static inline depthcube<T> spvDepthCast(texturecube<T> t) { return reinterpret_cast<thread const depthcube<T> &>(t); }\n"
                          "template <typename T> static inline depthcube_array<T> spvDepthCast(texturecube_array<T> t) { return reinterpret_cast<thread const depthcube_array<T> &>(t); }\n"},
        // Ryujinx's ConvertDoubleToFloat / ConvertFloatToDouble (HelperFunctionManager), step for step
        {"maxmetal_d2f(", "static inline uint maxmetal_d2f(uint lo, uint hi) { uint m = (lo >> 22u) | ((hi & 0xFFFFFu) << 10u); m |= (lo & 0x3FFFFFu) != 0u ? 1u : 0u; "
                      "uint e = (hi >> 20u) & 0x7FFu; uint rs = uint(int(hi) >> 31) << 31u; if ((m | e) == 0u) return rs; bool fin = e != 0x7FFu; "
                      "m |= 0x40000000u; e -= 0x381u; uint rx = (m & 0x7Fu) ^ 0x40u; m = (m + 0x40u) >> 7u; m &= rx != 0u ? ~0u : ~1u; "
                      "e = m != 0u ? e : 0u; e = fin ? e : 0xFFu; return m + (e << 23u) + rs; }\n"},
        {"maxmetal_f2d(", "static inline uint2 maxmetal_f2d(uint v) { uint m = v & 0x7FFFFFu, e = (v >> 23u) & 0xFFu, s = uint(int(v) >> 31); "
                      "bool fin = e != 0xFFu, enz = e != 0u, nd = enz || m == 0u; e += 0x380u; uint sd = 32u - (m == 0u ? 0xFFFFFFFFu : 31u - clz(m)); "
                      "uint ne = (1u - sd) - 1u, nm = m << sd; e = fin ? e : 0x7FFu; e = nd ? e : ne; m = enz ? m : nm; "
                      "return uint2(m << 29u, (m >> 3u) + (e << 20u) + (s << 31u)); }\n"},
        {"maxmetal_nearest", "constexpr sampler maxmetal_nearest(filter::nearest, mip_filter::none, address::clamp_to_edge);\n"},
        // the resolution-scale table of the original translator's dynamic resolution: identity (the renderer scales nothing)
        {"rscale[", "struct maxmetal_rscale_t { float4 operator[](uint i) const thread { return i == 0u ? float4(1.0f, 1.0f, 0.0f, 0.0f) : float4(1.0f); } };\n\n"},
    };
    Str o = {0};
    sp(&o, "%.*s", (int)at, s->buf);
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++)
        if (nvn_names(s->buf + at, h[i].name)) sp(&o, "%s", h[i].def);
    sp(&o, "%s", s->buf + at);
    free(s->buf);
    *s = o;
}

// ---- (NVN) bank 1 inlined: c1 holds the program's own constants (the request's c1, zeros after them), so a read at a
// constant index can be that word, which Metal's compiler folds, instead of a load. Two limits keep the result the
// Ryujinx path's, bit for bit (docs/adr/0003):
// - not +-1.0 / +-0.0: the compiler folds x * 1.0, x / -1.0, x - 0.0 ... to x, where the GPU flushes a denormal x to 0
//   and gives its default NaN for a NaN x;
// - not at all when a value computed only from constants, one of them such a word, may reach exp2 / log2 / sin / cos /
//   sqrt / rsqrt: the compiler computes those itself, not as the GPU does (the A15's GPU compiler sin and cos too).
// The check below follows the emitted body: which variables may hold such a value, merging both ways of an if and a
// loop's iterations (a may-analysis: a false alarm only leaves one shader its loads).
#define C1V_N 2048
typedef struct { uint8_t k[C1V_N], t[C1V_N]; } C1Vals;  // k: may be a constant; t: ... computed from an inlined c1 word
typedef struct C1Walk C1Walk;
struct C1Walk {
    const char *s; size_t i, n;
    const NvnKey *nk;
    char pat[64];             // "(*spvDescriptorSet0.<pfx>_c1)._m0["
    char (*names)[32];
    bool hit;
    C1Vals *loop_exits;       // the innermost loop's break / continue states
    C1Vals *sw_entry;         // the innermost switch's entry (a case label joins it)
};

static bool c1_word_inlinable(const NvnKey *nk, uint32_t word) {
    uint32_t v = 0;
    if ((uint64_t)word * 4 + 4 <= nk->c1_size) memcpy(&v, nk->c1 + word * 4, 4);
    return (v & 0x7FFFFFFFu) != 0x3F800000u && (v & 0x7FFFFFFFu) != 0u;
}

// "<pat><digits>u][<digit>]" at p: the word index, and its length (0: not a constant-index c1 read)
static size_t c1_read_at(const C1Walk *w, const char *p, uint32_t *word) {
    size_t lp = strlen(w->pat);
    if (strncmp(p, w->pat, lp)) return 0;
    unsigned row, col;
    int used = 0;
    if (sscanf(p + lp, "%uu][%u]%n", &row, &col, &used) != 2 || !used || col > 3) return 0;
    *word = row * 4 + col;
    return lp + (size_t)used;
}

static int c1_var(C1Walk *w, const char *p, size_t len) {
    if (len >= 32) len = 31;
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < len; k++) h = (h ^ (uint8_t)p[k]) * 16777619u;
    for (uint32_t probe = 0; probe < C1V_N; probe++) {
        char *e = w->names[(h + probe) % C1V_N];
        if (!e[0]) { memcpy(e, p, len); e[len] = 0; return (int)((h + probe) % C1V_N); }
        if (!strncmp(e, p, len) && !e[len]) return (int)((h + probe) % C1V_N);
    }
    w->hit = true;  // (table full: keep the loads)
    return -1;
}

static bool c1_pure(const char *p, size_t len) {
    static const char *const pure[] = {
        "as_type", "float", "float2", "float3", "float4", "half", "half2", "half3", "half4", "uint", "uint2", "uint3",
        "uint4", "int", "int2", "int3", "int4", "ushort", "ushort2", "short", "short2", "uchar", "char", "bool", "bool2",
        "bool3", "bool4", "ulong", "long", "true", "false", "fma", "abs", "saturate", "min", "max", "fmin", "fmax", "clamp",
        "select", "rint", "floor", "ceil", "trunc", "fract", "round", "exp2", "log2", "sin", "cos", "sqrt", "rsqrt",
        "maxmetal_rcp", "isnan", "isinf", "isfinite", "isunordered", "extract_bits", "insert_bits", "popcount", "clz",
        "ctz", "reverse_bits", "mulhi", "bfe_u", "bfe_s", "bfi", "hx", "hpack", "prmt", "fswzadd", "sign", "copysign",
        "step", "mix", "fast", "precise", "metal", "maxmetal_nz", "maxmetal_d2f", "maxmetal_f2d", "maxmetal_d16ref",
        "rscale", "rsc", "uvs", NULL};
    for (int k = 0; pure[k]; k++) if (strlen(pure[k]) == len && !strncmp(pure[k], p, len)) return true;
    return false;
}

// e[0..len): may it be a constant (*k), computed from an inlined c1 word (*t)?
static void c1_value(C1Walk *w, const C1Vals *st, const char *e, size_t len, bool *k, bool *t) {
    *k = true; *t = false;
    char prev = 0;
    for (size_t i = 0; i < len;) {
        uint32_t word;
        size_t m = e[i] == '(' ? c1_read_at(w, e + i, &word) : 0;
        if (m && i + m <= len) {
            if (c1_word_inlinable(w->nk, word)) *t = true; else *k = false;
            i += m; prev = ']';
            continue;
        }
        char ch = e[i];
        if ((ch >= '0' && ch <= '9') || (ch == '.' && i + 1 < len && e[i + 1] >= '0' && e[i + 1] <= '9')) {
            while (i < len && (id_char(e[i]) || e[i] == '.')) i++;  // a literal (1.5e-05f: "-05f" another one)
            prev = '0';
            continue;
        }
        if (id_start(ch)) {
            size_t b = i;
            while (i < len && id_char(e[i])) i++;
            if (prev == '.' || (i + 1 < len && e[i] == ':' && e[i + 1] == ':') || c1_pure(e + b, i - b)) { prev = 'a'; continue; }
            int v = c1_var(w, e + b, i - b);
            if (v >= 0 && st->k[v]) *t = *t || st->t[v];
            else *k = false;
            prev = 'a';
            continue;
        }
        if (ch != ' ' && ch != '\n' && ch != '\t') prev = ch;
        i++;
    }
    if (!*k) *t = false;
}

// the transcendentals in e[0..len) whose argument may be such a value
static void c1_check_calls(C1Walk *w, const C1Vals *st, const char *e, size_t len) {
    static const char *const fn[] = {"exp2", "log2", "sin", "cos", "sqrt", "rsqrt", NULL};
    for (size_t i = 0; i < len && !w->hit; i++) {
        if (!id_start(e[i]) || (i && (id_char(e[i - 1]) || e[i - 1] == '.'))) continue;
        size_t b = i;
        while (i < len && id_char(e[i])) i++;
        size_t j = i;
        while (j < len && e[j] == ' ') j++;
        if (j >= len || e[j] != '(') { i--; continue; }
        bool is = false;
        for (int f = 0; fn[f]; f++) if (strlen(fn[f]) == i - b && !strncmp(fn[f], e + b, i - b)) is = true;
        if (!is) { i--; continue; }
        size_t a = j + 1, q = a;
        for (int d = 1; q < len && d; q++) d += e[q] == '(' ? 1 : e[q] == ')' ? -1 : 0;
        bool k, t;
        c1_value(w, st, e + a, q - 1 - a, &k, &t);
        if (t) w->hit = true;
        i = a - 1;
    }
}

static void c1_union(C1Vals *a, const C1Vals *b) { for (int i = 0; i < C1V_N; i++) { a->k[i] |= b->k[i]; a->t[i] |= b->t[i]; } }

static bool c1_is_type(const char *p, size_t len) {
    static const char *const ty[] = {"bool", "uint", "int", "float", "half", "ushort", "short", "uchar", "char", "ulong",
                                     "long", NULL};
    for (int k = 0; ty[k]; k++) {
        size_t l = strlen(ty[k]);
        if (len >= l && len <= l + 1 && !strncmp(p, ty[k], l) && (len == l || (p[l] >= '2' && p[l] <= '4'))) return true;
    }
    return false;
}

// one statement e[0..len) (no ';'): `[type] name = expr` (declarators split at top-level commas), `lvalue op= expr`
static void c1_statement(C1Walk *w, C1Vals *st, const char *e, size_t len) {
    c1_check_calls(w, st, e, len);
    while (len && (*e == ' ' || *e == '\n')) { e++; len--; }
    if (len > 6 && !strncmp(e, "const ", 6)) { e += 6; len -= 6; }
    size_t i = 0;
    while (i < len && id_char(e[i])) i++;
    bool decl = i && c1_is_type(e, i) && i < len && e[i] == ' ';
    if (decl) { e += i + 1; len -= i + 1; }
    while (len) {  // each declarator / the assignment
        size_t end = 0;
        for (int d = 0; end < len; end++) {
            char ch = e[end];
            d += (ch == '(' || ch == '[' || ch == '{') ? 1 : (ch == ')' || ch == ']' || ch == '}') ? -1 : 0;
            if (decl && d == 0 && ch == ',') break;
        }
        // the top-level '=' (not ==, <=, >=, !=) and an operator before it
        size_t eq = 0; int d = 0;
        for (size_t q = 0; q < end; q++) {
            char ch = e[q];
            d += (ch == '(' || ch == '[' || ch == '{') ? 1 : (ch == ')' || ch == ']' || ch == '}') ? -1 : 0;
            if (d == 0 && ch == '=' && (q + 1 >= end || e[q + 1] != '=') && (!q || !strchr("=<>!", e[q - 1]) ||
                (q >= 2 && (e[q - 1] == '<' || e[q - 1] == '>') && e[q - 2] == e[q - 1]))) { eq = q; break; }
        }
        if (eq) {
            bool compound = strchr("|&^+-*/%<>", e[eq - 1]) != NULL;
            size_t lend = eq - (compound ? (e[eq - 1] == '<' || e[eq - 1] == '>') ? 2 : 1 : 0);
            const char *l = e;
            while (l < e + lend && (*l == ' ' || *l == '(' || *l == '*')) l++;
            size_t ln = 0;
            while (l + ln < e + lend && id_char(l[ln])) ln++;
            const char *r = l + ln;
            while (r < e + lend && *r == ' ') r++;
            bool simple = r == e + lend;  // a plain variable (else an element, a member, a dereference: a store)
            bool k, t;
            c1_value(w, st, e + eq + 1, end - eq - 1, &k, &t);
            int v = ln ? c1_var(w, l, ln) : -1;
            if (v >= 0) {
                if (!simple) { if (t) { st->k[v] = 1; st->t[v] = 1; } }  // (it may now hold one: sticky)
                else if (compound) { bool kk = k && st->k[v]; st->t[v] = kk && (t || st->t[v]); st->k[v] = kk; }
                else { st->k[v] = k; st->t[v] = t; }
            }
        } else if (decl) {  // declared, no value: not a constant
            size_t ln = 0;
            while (ln < end && id_char(e[ln])) ln++;
            int v = ln ? c1_var(w, e, ln) : -1;
            if (v >= 0) st->k[v] = st->t[v] = 0;
        }
        if (end >= len) break;
        e += end + 1; len -= end + 1;
        while (len && *e == ' ') { e++; len--; }
    }
}

static void c1_ws(C1Walk *w) {
    for (;;) {
        while (w->i < w->n && strchr(" \t\r\n", w->s[w->i])) w->i++;
        if (w->i + 1 < w->n && w->s[w->i] == '/' && w->s[w->i + 1] == '/') { while (w->i < w->n && w->s[w->i] != '\n') w->i++; continue; }
        if (w->i + 1 < w->n && w->s[w->i] == '/' && w->s[w->i + 1] == '*') {
            const char *e = strstr(w->s + w->i + 2, "*/");
            w->i = e ? (size_t)(e - w->s) + 2 : w->n;
            continue;
        }
        return;
    }
}

static bool c1_kw(C1Walk *w, const char *kw) {
    size_t l = strlen(kw);
    if (strncmp(w->s + w->i, kw, l) || id_char(w->s[w->i + l])) return false;
    w->i += l;
    return true;
}

// "( ... )" at the position: its contents
static void c1_paren(C1Walk *w, size_t *a, size_t *b) {
    c1_ws(w);
    *a = *b = w->i;
    if (w->i >= w->n || w->s[w->i] != '(') return;
    size_t q = w->i + 1;
    for (int d = 1; q < w->n && d; q++) d += w->s[q] == '(' ? 1 : w->s[q] == ')' ? -1 : 0;
    *a = w->i + 1; *b = q - 1; w->i = q;
}

static void c1_item(C1Walk *w, C1Vals *st);
static void c1_block(C1Walk *w, C1Vals *st);

static void c1_loop(C1Walk *w, C1Vals *st, bool body_first, bool is_switch) {
    // the body runs from the entry state merged with every state its back edge brings, to a fixpoint (the sets only grow)
    size_t start = w->i, after = start;
    C1Vals *entry = malloc(sizeof *entry), *s = malloc(sizeof *s), *exits = calloc(1, sizeof *exits);
    *entry = *st;
    C1Vals *saved_exits = w->loop_exits, *saved_sw = w->sw_entry;
    bool cond_false = false;
    for (int it = 0; it < 8; it++) {
        w->i = start; *s = *entry;
        w->loop_exits = exits;
        if (is_switch) w->sw_entry = entry;
        c1_item(w, s);
        after = w->i;
        cond_false = false;
        if (body_first) {  // do { } while (cond);
            c1_ws(w);
            if (c1_kw(w, "while")) {
                size_t a, b;
                c1_paren(w, &a, &b);
                c1_check_calls(w, s, w->s + a, b - a);
                cond_false = b - a == 5 && !strncmp(w->s + a, "false", 5);
                c1_ws(w);
                if (w->i < w->n && w->s[w->i] == ';') w->i++;
            }
            after = w->i;
        }
        if (cond_false && !is_switch) break;
        C1Vals next = *entry;
        c1_union(&next, s);
        c1_union(&next, exits);  // (continue: the back edge too)
        if (!memcmp(&next, entry, sizeof next)) break;
        *entry = next;
    }
    w->loop_exits = saved_exits; w->sw_entry = saved_sw;
    *st = *s;
    c1_union(st, exits);
    if (!body_first || is_switch) c1_union(st, entry);  // (while / for: maybe no iteration)
    w->i = after;
    free(entry); free(s); free(exits);
}

// the statements up to the closing '}' (or the end)
static void c1_block(C1Walk *w, C1Vals *st) {
    while (!w->hit) {
        c1_ws(w);
        if (w->i >= w->n) return;
        if (w->s[w->i] == '}') { w->i++; return; }
        c1_item(w, st);
    }
}

// one construct at the position: a { block }, an if (else), a loop, a case label or a statement
static void c1_item(C1Walk *w, C1Vals *st) {
    c1_ws(w);
    if (w->i >= w->n) return;
    if (w->s[w->i] == '{') { w->i++; c1_block(w, st); return; }
    if (c1_kw(w, "if")) {
        size_t a, b;
        c1_paren(w, &a, &b);
        c1_check_calls(w, st, w->s + a, b - a);
        C1Vals *els = malloc(sizeof *els);
        *els = *st;
        c1_item(w, st);
        c1_ws(w);
        if (c1_kw(w, "else")) c1_item(w, els);
        c1_union(st, els);
        free(els);
        return;
    }
    if (c1_kw(w, "do")) { c1_loop(w, st, true, false); return; }
    bool sw = c1_kw(w, "switch");
    if (sw || c1_kw(w, "while") || c1_kw(w, "for")) {
        size_t a, b;
        c1_paren(w, &a, &b);
        c1_check_calls(w, st, w->s + a, b - a);
        c1_loop(w, st, false, sw);
        return;
    }
    if (c1_kw(w, "case") || c1_kw(w, "default")) {  // a jump target: the switch's entry joins here
        while (w->i < w->n && w->s[w->i] != ':') w->i++;
        w->i++;
        if (w->sw_entry) c1_union(st, w->sw_entry);
        else w->hit = true;  // (a case outside any switch: give up)
        return;
    }
    size_t q = w->i;
    for (int d = 0; q < w->n; q++) {
        char ch = w->s[q];
        d += (ch == '(' || ch == '[' || ch == '{') ? 1 : (ch == ')' || ch == ']' || ch == '}') ? -1 : 0;
        if (d < 0 || (d == 0 && ch == ';')) break;
    }
    const char *e = w->s + w->i;
    size_t len = q - w->i;
    w->i = q < w->n && w->s[q] == ';' ? q + 1 : q;
    while (len && strchr(" \t\n", e[len - 1])) len--;
    if ((len == 5 && !strncmp(e, "break", 5)) || (len == 8 && !strncmp(e, "continue", 8))) {
        if (w->loop_exits) c1_union(w->loop_exits, st);
        return;
    }
    c1_statement(w, st, e, len);
}

// The body with bank 1's inlinable words in place of their loads, unless the check finds one reaching a transcendental
static void nvn_inline_c1(Ctx *c) {
    const NvnKey *nk = c->nvn;
    if (!nk || !nk->c1 || !c->body.buf || !(c->info->cbuf_mask & 2u)) return;
    C1Walk w = {.s = c->body.buf, .n = c->body.len, .nk = nk};
    snprintf(w.pat, sizeof w.pat, "(*spvDescriptorSet0.%s_c1)._m0[", c->pfx);
    if (!strstr(c->body.buf, w.pat)) return;
    w.names = calloc(C1V_N, sizeof *w.names);
    C1Vals *st = calloc(1, sizeof *st);
    // the registers, predicates and condition codes start as 0 / false: constants
    for (const char *p = c->body.buf; *p; p++) {
        if (!id_start(*p) || (p > c->body.buf && (id_char(p[-1]) || p[-1] == '.'))) continue;
        size_t l = 0;
        while (id_char(p[l])) l++;
        bool reg = (p[0] == 'r' || p[0] == 'p') && l > 1 && l <= 4;
        for (size_t k = 1; reg && k < l; k++) reg = p[k] >= '0' && p[k] <= '9';
        if (reg || (l == 2 && !strncmp(p, "cc", 2)) || (l == 3 && (!strncmp(p, "ccz", 3) || !strncmp(p, "ccs", 3)))) {
            int v = c1_var(&w, p, l);
            if (v >= 0) st->k[v] = 1;
        }
        p += l - 1;
    }
    c1_block(&w, st);
    free(st); free(w.names);
    if (w.hit) return;
    Str o = {0};
    bool left = false;  // (a load of bank 1 is left: its binding stays)
    const char *p = c->body.buf, *q;
    while ((q = strstr(p, w.pat))) {
        uint32_t word;
        size_t m = c1_read_at(&w, q, &word);
        if (m && c1_word_inlinable(nk, word)) {
            uint32_t v = 0;
            if ((uint64_t)word * 4 + 4 <= nk->c1_size) memcpy(&v, nk->c1 + word * 4, 4);
            sp(&o, "%.*s0x%08xu", (int)(q - p), p, v);
            p = q + m;
        } else {
            left = true;
            sp(&o, "%.*s", (int)(q - p + 1), p);
            p = q + 1;
        }
    }
    sp(&o, "%s", p);
    free(c->body.buf);
    c->body = o;
    if (!left) c->info->cbuf_mask &= ~2u;
}

static ShaderInfo *nvn_finish(Ctx *c, bool any_branch) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    const NvnKey *nk = c->nvn;
    int stage = c->stage;
    const char *pfx = c->pfx;
    bool gstore = info->uses_gmem_store;
    // the c0 entries of the storage slots an access may go to (MAXMETAL_GFIND reads their {address, size})
    uint32_t sbase = stage == STAGE_VERTEX ? 0x110 : stage == STAGE_FRAGMENT ? 0x510 : 0x310;
    if (info->uses_gmem && !c->sslot && !c->uslot && !c->xused) fail(c, "global memory without a storage buffer address from c0");
    // the image handles bindless surface ops were resolved to (nvn_gflow): as Ryujinx, which binds no driver bank for
    // them, their c2 loads read 0 (the renderer cannot fill those words)
    for (int i = 0; i < c->nimg && c->body.buf; i++) {
        uint32_t byte = 0x120 + 8 * c->img[i].slot;
        char from[96], vec[64];
        snprintf(from, sizeof from, "(*spvDescriptorSet0.%s_c2)._m0[%uu][%u]", pfx, byte >> 4, (byte >> 2) & 3);
        Str o = {0};
        for (const char *p = c->body.buf, *q; ; p = q + strlen(from)) {
            if (!(q = strstr(p, from))) { sp(&o, "%s", p); break; }
            sp(&o, "%.*s0u", (int)(q - p), p);
        }
        free(c->body.buf);
        c->body = o;
        snprintf(vec, sizeof vec, "%s_c2)._m0[%uu]", pfx, byte >> 4);
        if (!strstr(c->body.buf, vec)) c->drv_read[1][byte >> 4] = 0;
    }
    if (c->nimg && c->body.buf && !strstr(c->body.buf, "_c2)") && !c->drv_dyn[1]) info->cbuf_mask &= ~4u;
    nvn_inline_c1(c);
    for (int k = 0; k < 16; k++) {
        if (c->sslot & (1u << k)) { c->drv_read[0][(sbase + 16 * k) >> 4] = 1; info->cbuf_mask |= 1u; }
        if (c->uslot & (1u << k)) { c->drv_read[0][(0x210 + 16 * k) >> 4] = 1; info->cbuf_mask |= 1u; }
    }
    if (stage == STAGE_FRAGMENT && key->vs_out_mask) fail(c, "(nvn) vs_out_mask must be 0");
    if (c->fail) { free(c->body.buf); free(c); return info; }

    Str s = {0}, decl = {0}, bind = {0}, deps = {0}, cmp = {0};
    int aid = 0;
    #define BIND(name, type, kind, extra, ...) do { \
        sp(&decl, "    %s %s [[id(%d)]];\n", type, name, aid); \
        sp(&bind, "%s{\"msl_name\": \"%s\", \"msl_type\": \"%s\", \"arg_id\": %d, \"kind\": \"%s\"" extra "}", aid ? ", " : "", \
           name, type, aid, kind, ##__VA_ARGS__); \
        aid++; } while (0)
    char nm[64];
    for (int i = 0; i <= 16; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        snprintf(nm, sizeof nm, "%s_c%d", pfx, i);
        if (i == 1) BIND(nm, "constant maxmetal_cb*", "c1", ", \"bank\": 1");
        else if (i == 0 || i == 2) BIND(nm, "constant maxmetal_cb*", "drvcb", ", \"bank\": %d", i);
        else BIND(nm, "constant maxmetal_cb*", "cbuf", ", \"reg\": %d", i - 3);
    }
    const char *sbt = gstore ? "device maxmetal_sb*" : "const device maxmetal_sb*";
    for (int k = 0; k < 16; k++) {
        if (!(c->sslot & (1u << k))) continue;
        snprintf(nm, sizeof nm, "%s_s%d", pfx, k);
        BIND(nm, sbt, "sbuf", ", \"reg\": %d, \"rw\": %s", k, gstore ? "true" : "false");
    }
    for (int k = 0; k < 16; k++) {
        if (!(c->uslot & (1u << k))) continue;
        snprintf(nm, sizeof nm, "%s_u%d", pfx, k);
        BIND(nm, sbt, "ubo_ptr", ", \"reg\": %d", k);
    }
    for (int i = 0; i < c->nx; i++) {  // the renderer binds the buffer at the address it reads there
        if (!(c->xused & (1u << i))) continue;
        snprintf(nm, sizeof nm, "%s_x%d", pfx, i);
        BIND(nm, sbt, "sbuf_ubo", ", \"reg\": %d, \"off\": %d, \"rw\": %s", c->xcb[i] - 3, c->xoff[i], gstore ? "true" : "false");
    }
    for (int i = 0; i < c->nimg; i++) {  // image views (emit_surface): written ones access::write / read_write
        char ty[64];
        snprintf(nm, sizeof nm, "%s_i%d_%s", pfx, c->img[i].slot, c->img[i].fmt);
        snprintf(ty, sizeof ty, "texture2d<%s%s>", c->img[i].elem,
                 !c->img[i].write ? "" : c->img[i].read ? ", access::read_write" : ", access::write");
        BIND(nm, ty, "img", ", \"reg\": %d", c->img[i].slot);
    }
    bool cmp_seen[128] = {0};
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        int ns = c->tex_nvn[i];
        // a colour texture compared: declared as the colour texture, compared through spvDepthCast (nvn_rename), as
        // Ryujinx + SPIRV-Cross (the Ryujinx path declares depth only where a depth texture is bound)
        const char *ty = nvn_tex_decl(t, nk->tex_depth[ns]);
        if (!ty || (t->shadow && !nvn_tex_decl(t, true))) { fail(c, "depth texture of type %d", t->type); break; }
        snprintf(nm, sizeof nm, "%s_t%d", pfx, ns);
        BIND(nm, ty, "tex", ", \"reg\": %d", ns);
        snprintf(nm, sizeof nm, "%s_t%dSmplr", pfx, ns);
        BIND(nm, "sampler", "samp_tex", ", \"reg\": %d", ns);
        // the answers this translation depends on (as the Ryujinx path: a queried slot its type, others whether a depth
        // texture is bound there)
        if (c->tex_weak[i]) {
            int8_t rq = nk->tex_type[ns];
            sp(&deps, "%s\"%d\": \"%s%s\"", deps.len ? ", " : "", ns, rq >= 0 ? nvn_tex_answer(rq) : "2d",
               rq >= 0 && nk->tex_depth[ns] ? "_depth" : "");
        } else {
            sp(&deps, "%s\"%d\": \"%s\"", deps.len ? ", " : "", ns, nk->tex_depth[ns] ? "*_depth" : "*");
        }
        if (t->shadow && !cmp_seen[ns]) { cmp_seen[ns] = true; sp(&cmp, "%s%d", cmp.len ? ", " : "", ns); }
    }
    #undef BIND
    if (c->fail) { free(s.buf); free(decl.buf); free(bind.buf); free(deps.buf); free(cmp.buf); free(c->body.buf); free(c); return info; }

    // ---- source
    sp(&s, "#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n");
    size_t helpers_at = s.len;  // (nvn_helpers: the ones the source uses)
    sp(&s, "struct maxmetal_cb\n{\n    uint4 _m0[4096];\n};\n\n");
    sp(&s, "struct maxmetal_sb\n{\n    uint _m0[1];\n};\n\n");
    if (info->uses_gmem) {
        // the slots data flow found (nvn_gflow): one, MAXMETAL_GAT(ga, s4); more, MAXMETAL_GDECL MAXMETAL_GIN(ga, s4) else
        // MAXMETAL_GIN(ga, u7) ... MAXMETAL_GOK; the search: one slot, its test; more, a loop over the slot table (gsp / gsb /
        // gsn: main0), which compiles faster than a test per slot at every access
        int nslot = __builtin_popcount(c->sslot) + __builtin_popcount(c->uslot);
        const char *cq = gstore ? "" : "const ";
        sp(&s, "#define MAXMETAL_GAT(ga, k) %sdevice uint *gp = (*spvDescriptorSet0.%s_##k)._m0; uint go = uint(ga) - gk_##k.x; const bool gok = true;\n", cq, pfx);
        sp(&s, "#define MAXMETAL_GDECL %sdevice uint *gp = nullptr; uint go = 0u;\n", cq);
        sp(&s, "#define MAXMETAL_GIN(ga, x) if ((ga) - gb_##x < ulong(gn_##x)) { gp = (*spvDescriptorSet0.%s_##x)._m0; go = uint((ga) - gb_##x); }\n", pfx);
        sp(&s, "#define MAXMETAL_GOK const bool gok = gp != nullptr;\n");
        sp(&s, "#define MAXMETAL_GFIND(ga) MAXMETAL_GDECL MAXMETAL_GSEARCH(ga) MAXMETAL_GOK\n");
        if (nslot == 0) {   // (only user uniform blocks' addresses: no search, an unknown address finds nothing)
            sp(&s, "#define MAXMETAL_GSEARCH(ga)");
        } else if (nslot == 1) {
            char x[8];
            snprintf(x, sizeof x, "%c%d", c->sslot ? 's' : 'u', __builtin_ctz(c->sslot | c->uslot));
            sp(&s, "#define MAXMETAL_GSEARCH(ga) if ((ga) - gb_%s < ulong(gn_%s)) { gp = (*spvDescriptorSet0.%s_%s)._m0; go = uint((ga) - gb_%s); }",
               x, x, pfx, x, x);
        } else
            sp(&s, "#define MAXMETAL_GSEARCH(ga) for (uint gi = 0u; gi < %du; gi++) if ((ga) - gsb[gi] < ulong(gsn[gi])) { gp = gsp[gi]; go = uint((ga) - gsb[gi]); break; }", nslot);
        sp(&s, "\n\n");
    }
    uint32_t shw = c->shared_words;
    if (c->uses_shared && (c->shared_dyn || shw < 1024)) shw = shw > 1024 ? shw : 1024;
    if (shw > 8192) { fail(c, "%u words of shared memory", shw); }
    if (c->uses_shared) sp(&s, "#define MAXMETAL_SHW(a) min((a) >> 2, %uu)\n\n", shw - 1);
    // no bindings: no argument buffer at all (as SPIRV-Cross: the renderer makes no encoder for it)
    if (aid) sp(&s, "struct spvDescriptorSetBuffer0\n{\n%s};\n\n", decl.buf);

    Str params = {0};
    #define PARAM(...) do { if (params.len) sp(&params, ", "); sp(&params, __VA_ARGS__); } while (0)
    const char *abuf = "constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]";
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct main0_out\n{\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "    float4 v%d [[user(locn%d)]];\n", i, i);
        sp(&s, "    float4 pos [[position, invariant]];\n    float psize [[point_size]];\n};\n\n");
        if (c->in_attr_mask) {
            sp(&s, "struct main0_in\n{\n");
            for (int i = 0; i < 32; i++)
                if (c->in_attr_mask & (1u << i)) sp(&s, "    float4 a%d [[attribute(%d)]];\n", i, i);
            sp(&s, "};\n\n");
            PARAM("main0_in in [[stage_in]]");
        }
        if (aid) PARAM("%s", abuf);
        if (c->uses_vid) PARAM("uint vid [[vertex_id]]");
        if (c->uses_iid) PARAM("uint iid [[instance_id]]");
        sp(&s, "vertex main0_out main0(%s)\n{\n    main0_out out = {};\n", params.buf ? params.buf : "");
        sp(&s, "    out.pos = float4(0.0f, 0.0f, 0.0f, 1.0f);\n    out.psize = 1.0f;\n");
        for (int i = 0; i < 32; i++)  // a varying the shader writes in part reads (0, 0, 0, 1) elsewhere (patch 0005)
            if (c->out_attr_mask & (1u << i)) sp(&s, "    out.v%d = float4(0.0f, 0.0f, 0.0f, 1.0f);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct main0_out\n{\n");
        for (int rt = 0; rt < 8; rt++)
            if ((c->omap_target >> (rt * 4)) & 0xF) sp(&s, "    float4 c%d [[color(%d)]];\n", rt, rt);
        if (c->omap_depth) { sp(&s, "    float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n\n");
        if (c->in_attr_mask) {
            sp(&s, "struct main0_in\n{\n");
            for (int i = 0; i < 32; i++) {
                if (!(c->in_attr_mask & (1u << i))) continue;
                const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
                sp(&s, "    float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
            }
            sp(&s, "};\n\n");
            PARAM("main0_in in [[stage_in]]");
        }
        if (aid) PARAM("%s", abuf);
        PARAM("float4 gl_FragCoord [[position]]");
        if (c->uses_front_facing) PARAM("bool front_facing [[front_facing]]");
        if (c->uses_lane) PARAM("uint lane [[thread_index_in_simdgroup]]");
        if (c->uses_point_coord) PARAM("float2 point_coord [[point_coord]]");
        sp(&s, "%sfragment main0_out main0(%s)\n{\n    main0_out out = {};\n", nk->early_fragment_tests ? "[[ early_fragment_tests ]] " : "", params.buf);
    } else {
        if (aid) PARAM("%s", abuf);
        PARAM("uint3 ctaid [[threadgroup_position_in_grid]]");
        PARAM("uint3 tid [[thread_position_in_threadgroup]]");
        if (c->uses_lane) PARAM("uint lane [[thread_index_in_simdgroup]]");
        sp(&s, "kernel void main0(%s)\n{\n", params.buf);
        if (c->uses_shared) sp(&s, "    threadgroup uint shm[%u];\n", shw);
    }
    #undef PARAM
    free(params.buf);
    if (nvn_names(c->body.buf, "rscale[")) sp(&s, "    maxmetal_rscale_t rscale;\n");
    for (int k = 0; k < 16; k++) {
        if (c->sslot & (1u << k))
            sp(&s, "    uint4 gk_s%d = (*spvDescriptorSet0.%s_c0)._m0[%uu]; ulong gb_s%d = ulong(gk_s%d.y) << 32 | ulong(gk_s%d.x); uint gn_s%d = gk_s%d.z;\n",
               k, pfx, (sbase + 16 * k) >> 4, k, k, k, k, k);
        if (c->uslot & (1u << k))
            sp(&s, "    uint4 gk_u%d = (*spvDescriptorSet0.%s_c0)._m0[%uu]; ulong gb_u%d = ulong(gk_u%d.y) << 32 | ulong(gk_u%d.x); uint gn_u%d = gk_u%d.z;\n",
               k, pfx, (0x210 + 16 * k) >> 4, k, k, k, k, k);
    }
    // a user uniform block's address: its size isn't known (as Ryujinx's sbuf_ubo access: unchecked), z all ones
    for (int i = 0; i < c->nx; i++) {
        if (!(c->xused & (1u << i))) continue;
        unsigned lo = c->xoff[i], hi = lo + 4;
        sp(&s, "    uint4 gk_x%d = uint4((*spvDescriptorSet0.%s_c%d)._m0[%uu][%u], (*spvDescriptorSet0.%s_c%d)._m0[%uu][%u], 0xFFFFFFFFu, 0u); "
               "ulong gb_x%d = ulong(gk_x%d.y) << 32 | ulong(gk_x%d.x); uint gn_x%d = gk_x%d.z;\n",
           i, pfx, c->xcb[i], lo >> 4, (lo >> 2) & 3, pfx, c->xcb[i], hi >> 4, (hi >> 2) & 3, i, i, i, i, i);
    }
    if (info->uses_gmem && __builtin_popcount(c->sslot) + __builtin_popcount(c->uslot) > 1 && nvn_names(c->body.buf, "MAXMETAL_GFIND(ga)")) {
        Str p = {0}, b = {0}, n = {0};
        for (int u = 0; u < 2; u++)
            for (int k = 0; k < 16; k++) {
                if (!((u ? c->uslot : c->sslot) & (1u << k))) continue;
                char x = u ? 'u' : 's';
                sp(&p, "%s(*spvDescriptorSet0.%s_%c%d)._m0", p.len ? ", " : "", pfx, x, k);
                sp(&b, "%sgb_%c%d", b.len ? ", " : "", x, k);
                sp(&n, "%sgn_%c%d", n.len ? ", " : "", x, k);
            }
        sp(&s, "    %sdevice uint *gsp[] = {%s};\n    ulong gsb[] = {%s};\n    uint gsn[] = {%s};\n", gstore ? "" : "const ", p.buf, b.buf, n.buf);
        free(p.buf); free(b.buf); free(n.buf);
    }
    if (c->uses_helper) sp(&s, "    const bool maxmetal_helper = simd_is_helper_thread();\n");
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "    uint r%d = 0;\n", i);
    for (int i = 0; i < c->nxs; i++) sp(&s, "    uint xs%d = 0u;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "    bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "    uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "    uint lmem[%d] = {};\n", LMEM_WORDS);
    Str body = {0};
    nvn_rename(c, &body, c->body.buf ? c->body.buf : "");
    if (any_branch) {
        sp(&s, "    uint pc = 8u; uint crs[32]; uint crs_n = 0u;\n");
        sp(&s, "    bool exit_now = false;\n");
        // Fragment / compute: the SIMD group runs one block at a time, its lowest pending one (each block ends by going to
        // the next), so lanes whose paths parted meet again where the paths join, as on the Switch (SSY / SYNC). Else a quad
        // that branched apart samples with implicit derivatives from lanes in other blocks (wrong mip levels where a branch
        // condition changes) and quad swizzles read lanes elsewhere. Vertex functions have no lanes to agree with.
        if (stage != STAGE_VERTEX)
            sp(&s, "    for (uint guard = 0; guard < 1000000u; guard++) {\n    if (pc != simd_min(pc)) continue;\n  switch (pc) {\n");
        else
            sp(&s, "    for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else if (c->structured) {
        sp(&s, "%s", body.buf ? body.buf : "");
    } else {
        sp(&s, "    do {\n%s    } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (stage == STAGE_VERTEX) {
        if (nk->depth_minus_w) sp(&s, "    out.pos.z = fma(out.pos.z, 0.5f, out.pos.w * 0.5f);\n");  // (as the Ryujinx path)
        sp(&s, "    return out;\n}\n\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (r < 256 && c->reg_used[r]) sp(&s, "    out.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                r++;
            }
        }
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "    out.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "    out.depth = gl_FragCoord.z;\n");
        }
        sp(&s, "    return out;\n}\n\n");
    } else {
        sp(&s, "}\n\n");
    }

    // ---- reflection members
    Str rf = {0}, dr = {0}, unf = {0};
    sp(&rf, "\"bindings\": [%s]", bind.buf ? bind.buf : "");
    sp(&rf, ", \"vertex_inputs\": [");
    if (stage == STAGE_VERTEX)
        for (int i = 0, n = 0; i < 32; i++)
            if (c->in_attr_mask & (1u << i)) sp(&rf, "%s{\"location\": %d, \"msl_type\": \"float4\"}", n++ ? ", " : "", i);
    sp(&rf, "]");
    for (int bank = 0; bank < 2; bank++) {
        int n = 0;
        for (int i = 0; i < 0xA00 / 16; i++) n += c->drv_read[bank][i];
        if (!n && !c->drv_dyn[bank]) continue;
        sp(&dr, "%s\"%d\": {\"vec4_offsets\": [", dr.len ? ", " : "", bank * 2);
        for (int i = 0, k = 0; i < 0xA00 / 16; i++) {
            if (!c->drv_read[bank][i]) continue;
            sp(&dr, "%s%d", k++ ? ", " : "", i * 16);
            if (!nvn_drv_fillable(stage, bank, (uint32_t)i * 16)) sp(&unf, "%s\"c%d+0x%x\"", unf.len ? ", " : "", bank * 2, i * 16);
        }
        sp(&dr, "], \"dynamic\": %s}", c->drv_dyn[bank] ? "true" : "false");
        if (c->drv_dyn[bank]) sp(&unf, "%s\"c%d dynamic index\"", unf.len ? ", " : "", bank * 2);
    }
    sp(&rf, ", \"drv_reads\": {%s}, \"drv_unfillable\": [%s]", dr.buf ? dr.buf : "", unf.buf ? unf.buf : "");
    sp(&rf, ", \"deps\": {\"tex\": {%s}, \"img\": {}, \"attr\": {}, \"fout\": {}}", deps.buf ? deps.buf : "");
    sp(&rf, ", \"compare_slots\": [%s]", cmp.buf ? cmp.buf : "");
    free(dr.buf); free(unf.buf); free(decl.buf); free(bind.buf); free(deps.buf); free(cmp.buf);
    if (c->fail) { free(s.buf); free(rf.buf); free(c->body.buf); free(c); return info; }
    nvn_helpers(&s, helpers_at);
    info->msl = s.buf;
    info->refl = rf.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    c->info = info;
    c->key = key;
    c->stage = stage;
    c->ipa_mode = -1;
    c->glsl = key && key->glsl;
    c->nvn = key && !key->glsl ? key->nvn : NULL;
    c->feat = key ? key->features | (key->flip_x ? MAXMETAL_FEAT_RSCALE : 0) : 0;
    c->pfx = stage == STAGE_VERTEX ? "vp" : stage == STAGE_FRAGMENT ? "fp" : "cp";
    const uint8_t *sph = blob + 0x30;
    const uint8_t *code = blob + 0x80;
    // compute programs have neither the 0x30-byte NVN header nor an SPH: code starts at the blob
    if (stage == STAGE_COMPUTE) code = blob;
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
    }
    // find the end: the last EXIT followed by self-branch padding, or max_size
    size_t n = (max_size - (code - blob)) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        // bra self (NVN's padding branches to the scheduling word before it, which runs the same instruction)
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && norm_target((int64_t)i * 8 + 8 + sbits(w, 20, 24)) == i * 8 && end) break;
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    // (NVN) a fragment program that discards (KIL, PT, CC T) before any branch or side effect: Ryujinx ends the block at
    // the KIL and drops the rest as dead, outputs and bindings too (a real game's 5 all-discard programs). Likewise: just the KIL
    uint64_t *kcode = NULL;
    if (c->nvn && stage == STAGE_FRAGMENT) {
        for (size_t i = 0; i < end; i++) {
            if (i % 4 == 0) continue;
            uint64_t w = ((const uint64_t *)code)[i];
            unsigned hi = (unsigned)(w >> 56), op = (unsigned)(w >> 48);
            if ((w & 0xfff00000000f001full) == 0xe33000000007000full) {
                kcode = calloc(i + 1, 8);
                kcode[i] = w;
                for (size_t k = 0; k < i; k++) kcode[k] = k % 4 ? 0x50b0000000070f00ull : ((const uint64_t *)code)[k];  // NOP
                code = (const uint8_t *)kcode;
                end = i + 1;
                c->omap_target = 0;
                c->omap_depth = 0;
                break;
            }
            if ((hi & 0xc0) == 0x80 || (hi >= 0xe2 && op != 0xef90 && (op & 0xfff8) != 0xf0c8)) break;  // LD / ST, flow, memory
        }
    }
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) {  // BRX: its jump table's targets, else any instruction
            any_branch = true;
            uint32_t tv[256];  // (NVN) its table's targets (sbrx)
            int n = c->nvn ? sbrx(c, (const uint64_t *)code, i, tv, 256) : 0;
            for (int e = 0; e < n; e++)
                if (norm_target(tv[e]) / 8 < end) label[norm_target(tv[e]) / 8] = 1;
            if (!n && (!c->glsl || !brx_table_labels(code, i, end, blob, max_size, label))) memset(label, 1, end);
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
        (void)is_branch_target_op;
    }
    // translate
    bool dbg = maxwell_dbg_code && blob == maxwell_dbg_code && stage == STAGE_FRAGMENT;
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("MAXMETAL_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    // (NVN) structured control flow where the program allows it; MAXMETAL_STATE_MACHINE=1: always the state machine
    c->structured = c->nvn && any_branch && !dbg && !getenv("MAXMETAL_STATE_MACHINE") &&
                    nvn_structure(c, (const uint64_t *)code, end, label, annotate);
    c->gtrack = c->nvn && !any_branch && !dbg;  // (no branches: one block, nvn_gflow below)
    c->itrack = c->nvn && any_branch && !c->structured && !dbg;  // (the state machine: nvn_iflow below)
    c->gblk = 0;
    for (size_t i = 0; i < end && !c->fail && !c->structured; i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (any_branch && label[i]) {
            // (NVN, fragment / compute: lanes run in lock step, nvn_finish) a block ends by going to the next one, so the
            // SIMD group picks its lowest pending block before any lane runs it
            if (c->nvn && stage != STAGE_VERTEX && i > 1) sp(&c->body, "    pc = %uu; continue;\n", pc);
            sp(&c->body, "  case %uu:\n", pc);
            if (c->itrack) c->gblk++;  // (nvn_iflow: a block starts)
            memset(c->wstate, 0, sizeof c->wstate);
            memset(c->qshf, 0, sizeof c->qshf);
            memset(c->xm, 0, sizeof c->xm);
        }
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        gflow_insn(c, ((const uint64_t *)code)[i], pc);
        if (!c->nvn && (c->feat & MAXMETAL_FEAT_LAS)) light_tiles_fix(c, (const uint64_t *)code, end, pc);  // (Link's Awakening's own shader)
        if (dbg && pc == maxwell_dbg_pc) {
            int rr = maxwell_dbg_reg % 1000, half = maxwell_dbg_reg / 1000;  // +1000: low f16, +2000: high f16
            c->reg_used[rr] = 1;
            if (half) sp(&c->body, c->glsl ? "    dbgv = unpackHalf2x16(r%d)[%d];\n" : "    dbgv = float(as_type<half2>(r%d)[%d]);\n", rr, half - 1);
            else sp(&c->body, "    dbgv = as_type<float>(r%d);\n", rr);
        }
        info->num_insts++;
    }
    if (c->gtrack) {
        c->gtrack = false;
        SBlk one = {.seen = true, .succ = {-1, -1}, .code = c->body};
        SProg g = {.c = c, .b = &one, .nb = 1};
        if (!c->fail) nvn_gflow(&g);
        c->body = one.code;
        free(c->gev); c->gev = NULL; c->ngev = c->cap_gev = 0;
    }
    if (c->itrack) {
        c->itrack = false;
        if (!c->fail) nvn_iflow(c);
        free(c->gev); c->gev = NULL; c->ngev = c->cap_gev = 0;
    }
    free(label);
    free(kcode);
    // (MSL: more textures than sampler slots take a slot table, at a buffer index the vertex stage's streams use)
    if (!c->glsl && stage == STAGE_VERTEX && info->ntex > MSL_MAX_SAMPLERS)
        fail(c, "%d textures in a vertex shader (Metal: %d samplers)", info->ntex, MSL_MAX_SAMPLERS);
    if (c->fail) { free(c->body.buf); free(c); return info; }
    if (c->glsl) return glsl_finish(c, any_branch, dbg);
    if (c->nvn) return nvn_finish(c, any_branch && !c->structured);

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint off) { return cb[off >> 4][(off >> 2) & 3]; }\n");
    sp(&s, "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s, "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    if (c->feat & MAXMETAL_FEAT_RSCALE) {
        sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
        sp(&s, "static inline float2 uvs(float2 uv, float4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n");
    }
    sp(&s, "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    // merges on half2 lanes rather than with integer masks: on an M1 (macOS 15.0.1) the Metal compiler miscompiled
    // (old & 0xFFFF0000u) | lo in Link's body shader - the merged low half read back as a constant 512.0 and Link
    // rendered white. Same bits either way.
    sp(&s, "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); half2 o = as_type<half2>(old), n = half2(r); if (merge == 2u) o.x = n.x; else if (merge == 3u) o.y = n.y; else o = n; return as_type<uint>(o); }\n");
    sp(&s, "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n");

    // inputs/outputs
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct VIn {\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", i, i);
        }
        sp(&s, "};\n");
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");  // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) { sp(&s, "  float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};  // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    // (MAXMETAL_FEAT_TINT) Runtime highlight (e.g. golden unchecked pots): fragment shaders writing a float color
    // target 0 get a per-draw tint, float4 [0] = colour.rgb + strength, [1] = added glow.rgb (strength 0 = untouched;
    // < 0: the recolour, emit_recolour).
    bool tint = (c->feat & MAXMETAL_FEAT_TINT) && stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&args, ", constant float4 *ltint [[buffer(19)]]");
        info->uses_tint = true;
    }
    // Metal has 16 sampler slots a stage (textures: 31). Up to 16 textures, texture i samples with sampler i. With more
    // (a cave floor's fragment shader has 17), the samplers come as one array of the 16 slots and a table
    // (buffer SMAP_BUFFER_INDEX) names each texture's slot: the renderer puts each distinct sampler state the draw uses
    // in a slot of its own, so every texture keeps its own filtering and wrapping.
    bool smap = info->ntex > MSL_MAX_SAMPLERS;  // (not in vertex shaders: refused above)
    if (smap) sp(&args, ", constant uint *smap [[buffer(%d)]], array<sampler, %d> ss [[sampler(0)]]", SMAP_BUFFER_INDEX,
                 MSL_MAX_SAMPLERS);
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        sp(&args, ", %s%s<float> t%d [[texture(%d)]]", t->shadow ? "depth" : "texture", tn, i, i);
        if (!smap) sp(&args, ", sampler s%d [[sampler(%d)]]", i, i);
    }
    if (stage == STAGE_VERTEX) {
        sp(&s, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
           (maxwell_dbg_vs && blob == maxwell_dbg_vs) ? ", device float4 *dbgout [[buffer(29)]]" : "",
           args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "",
           c->uses_point_coord ? ", float2 point_coord [[point_coord]]" : "", args.buf ? args.buf : "");
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n",
           args.buf ? args.buf : "");
    }
    free(args.buf);
    if (smap)
        for (int i = 0; i < info->ntex; i++) sp(&s, "  sampler s%d = ss[smap[%d]];\n", i, i);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < c->nxs; i++) sp(&s, "  uint xs%d = 0u;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint crs[32]; uint crs_n = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->flip_x) sp(&s, "  out.pos.x = -out.pos.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (maxwell_dbg_vs && blob == maxwell_dbg_vs) sp(&s, "  if (iid < 64u && vid < 8u) { dbgout[iid * 8u + vid] = out.pos; dbgout[512u + iid * 8u + vid] = %s; }\n", (c->out_attr_mask & 2u) ? "out.v1" : "float4(0)");
        sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    sp(&s, comp == 3 ? "  o.c%d[%d] = 1.0f;\n" : "  o.c%d[%d] = dbgv;\n", rt, comp);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { float3 g = o.c0.rgb; float l = dot(g, float3(0.299f, 0.587f, 0.114f));\n"
                   "    o.c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "o.c0", "float3", "float4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s->refl);
    free(s);
}

void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file) {
    (void)code; (void)max_size; (void)stage; (void)file;
}
