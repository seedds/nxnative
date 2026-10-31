// t_vk_spirv_fix: the guest Vulkan layer's SPIR-V checks and fix-ups (dm/gfx/hostvk/guest_vk.c guest_spirv_*; docs/design/vulkan-backend.md 8.1, step P9a) on hand-written
// modules, unit level, no device: the structural checks refuse what they name and pass what is valid; Super Mario Galaxy's form, ArrayStride on a struct member instead of on the
// array type, comes out with the stride on the type and nothing else changed, as do the variants (the type already decorated, a conflicting stride, a member that is no array);
// a fragment shader writing its second blend source at Location 1 without an Index comes out writing it at Location 0, Index 1, and every other output shape is left alone.
// The same on glslang's modules of the GPU test (guest_ubo.vert made into Galaxy's form, guest_dual.frag). Where spirv-val is installed (the nxnative-build image), every module
// the fix-ups produce passes it and Galaxy's form fails it (so the fix-up is what makes it valid); elsewhere that half is reported as not run.
#include "t_vk_harness.h"
#include "../../../host/hle.h"
#include "../../hostvk/guest_vk.h"
#include "shaders/gen/guest_ubo_vert.h"
#include "shaders/gen/guest_dual_frag.h"

PLAT_TABLE_DEFINE(swhle)   // the import table guest_vk.c's entries register in (dm/host/hle.c defines it in a host binary)
int dm_opmode(void) { return 0; }

// ---------- A SPIR-V emitter ----------
typedef struct { uint32_t w[1024]; size_t n; } mod_t;
static void emit(mod_t *m, uint32_t op, const uint32_t *ops, size_t n) {
    if (m->n + 1 + n > sizeof m->w / sizeof *m->w) { fprintf(stderr, "module too long\n"); exit(2); }
    m->w[m->n++] = (uint32_t)(1 + n) << 16 | op;
    for (size_t i = 0; i < n; i++) m->w[m->n++] = ops[i];
}
#define I(m, op, ...) emit(m, op, (const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
#define I0(m, op) emit(m, op, NULL, 0)
enum {   // opcodes, decorations and enumerants of the SPIR-V specification
    OpCapability = 17, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpDecorate = 71, OpMemberDecorate = 72, OpTypeVoid = 19, OpTypeFunction = 33,
    OpTypeFloat = 22, OpTypeVector = 23, OpTypeInt = 21, OpConstant = 43, OpConstantComposite = 44, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpVariable = 59,
    OpFunction = 54, OpLabel = 248, OpAccessChain = 65, OpLoad = 61, OpStore = 62, OpReturn = 253, OpFunctionEnd = 56,
    Shader = 1, Logical = 0, GLSL450 = 1, Vertex = 0, Fragment = 4, OriginUpperLeft = 7, BuiltIn = 11, Position = 0, ArrayStride = 6, Offset = 35, Block = 2,
    DescriptorSet = 34, Binding = 33, Location = 30, Index = 32, Uniform = 2, Output = 3,
};
#define MAIN 0x6e69616du, 0u   // "main" and its terminating zero
static void header(mod_t *m, uint32_t bound) { m->n = 0; m->w[m->n++] = 0x07230203u; m->w[m->n++] = 0x00010000u; m->w[m->n++] = 0; m->w[m->n++] = bound; m->w[m->n++] = 0; }

// A vertex shader reading element 2 of a uniform block's array of four vec4: the array type's ArrayStride as SPIR-V puts it (form 0), on the block's member instead (form 1, Super
// Mario Galaxy's), on both with the same stride (2), on both with different strides (3), or form 1 plus an ArrayStride on a second member that is no array (4). The decorations
// of forms 1 to 4 stand where form 0's stands, so the fixed module of each is form 0's word for word (form 4: with its second member)
enum { A_MAIN = 1, A_VOID, A_FN, A_FLOAT, A_V4, A_UINT, A_C4, A_ARR, A_BLK, A_PBLK, A_UBO, A_PV4O, A_POS, A_PV4U, A_INT, A_I0, A_I2, A_LBL, A_P, A_V, A_BOUND };
static void module_a(mod_t *m, int form) {
    header(m, A_BOUND);
    I(m, OpCapability, Shader); I(m, OpMemoryModel, Logical, GLSL450); I(m, OpEntryPoint, Vertex, A_MAIN, MAIN, A_POS);
    I(m, OpDecorate, A_POS, BuiltIn, Position);
    if (form == 0 || form == 2 || form == 3) I(m, OpDecorate, A_ARR, ArrayStride, 16);
    if (form != 0) I(m, OpMemberDecorate, A_BLK, 0, ArrayStride, form == 3 ? 32 : 16);
    if (form == 4) I(m, OpMemberDecorate, A_BLK, 1, ArrayStride, 16);
    I(m, OpMemberDecorate, A_BLK, 0, Offset, 0);
    if (form == 4) I(m, OpMemberDecorate, A_BLK, 1, Offset, 64);
    I(m, OpDecorate, A_BLK, Block); I(m, OpDecorate, A_UBO, DescriptorSet, 0); I(m, OpDecorate, A_UBO, Binding, 0);
    I(m, OpTypeVoid, A_VOID); I(m, OpTypeFunction, A_FN, A_VOID); I(m, OpTypeFloat, A_FLOAT, 32); I(m, OpTypeVector, A_V4, A_FLOAT, 4);
    I(m, OpTypeInt, A_UINT, 32, 0); I(m, OpConstant, A_UINT, A_C4, 4); I(m, OpTypeArray, A_ARR, A_V4, A_C4);
    if (form == 4) I(m, OpTypeStruct, A_BLK, A_ARR, A_V4); else I(m, OpTypeStruct, A_BLK, A_ARR);
    I(m, OpTypePointer, A_PBLK, Uniform, A_BLK); I(m, OpVariable, A_PBLK, A_UBO, Uniform);
    I(m, OpTypePointer, A_PV4O, Output, A_V4); I(m, OpVariable, A_PV4O, A_POS, Output); I(m, OpTypePointer, A_PV4U, Uniform, A_V4);
    I(m, OpTypeInt, A_INT, 32, 1); I(m, OpConstant, A_INT, A_I0, 0); I(m, OpConstant, A_INT, A_I2, 2);
    I(m, OpFunction, A_VOID, A_MAIN, 0, A_FN); I(m, OpLabel, A_LBL);
    I(m, OpAccessChain, A_PV4U, A_P, A_UBO, A_I0, A_I2); I(m, OpLoad, A_V4, A_V, A_P); I(m, OpStore, A_POS, A_V);
    I0(m, OpReturn); I0(m, OpFunctionEnd);
}
// Form 4's fixed module: form 0 with the second member (its member ArrayStride dropped)
static void module_a4_fixed(mod_t *m) {
    module_a(m, 4);
    mod_t o = {{0}, 0};
    for (size_t i = 0; i < 5; i++) o.w[o.n++] = m->w[i];
    for (size_t i = 5; i < m->n; i += m->w[i] >> 16) {
        const uint32_t *x = m->w + i;
        if ((x[0] & 0xffff) == OpMemberDecorate && x[3] == ArrayStride) { if (x[2] == 0) I(&o, OpDecorate, A_ARR, ArrayStride, 16); continue; }
        for (uint32_t k = 0; k < x[0] >> 16; k++) o.w[o.n++] = x[k];
    }
    *m = o;
}
// A fragment shader writing two outputs: at Locations 0 and 1 (form 0: the second blend source as Super Mario Galaxy's GX shaders write it), the fixed form (1: Location 0 and
// Location 0 Index 1), the second with an explicit Index 0 (2), the first with Index 1 already (3); form 4 is form 0 as a vertex shader's outputs (no fragment entry point)
enum { B_MAIN = 1, B_VOID, B_FN, B_FLOAT, B_V4, B_PV4O, B_O0, B_O1, B_C1, B_C0, B_VC, B_LBL, B_BOUND };
static void module_b(mod_t *m, int form) {
    header(m, B_BOUND);
    I(m, OpCapability, Shader); I(m, OpMemoryModel, Logical, GLSL450);
    if (form == 4) I(m, OpEntryPoint, Vertex, B_MAIN, MAIN, B_O0, B_O1);
    else { I(m, OpEntryPoint, Fragment, B_MAIN, MAIN, B_O0, B_O1); I(m, OpExecutionMode, B_MAIN, OriginUpperLeft); }
    I(m, OpDecorate, B_O0, Location, 0);
    if (form == 3) I(m, OpDecorate, B_O0, Index, 1);
    I(m, OpDecorate, B_O1, Location, form == 1 ? 0 : 1);
    if (form == 1) I(m, OpDecorate, B_O1, Index, 1);
    if (form == 2) I(m, OpDecorate, B_O1, Index, 0);
    I(m, OpTypeVoid, B_VOID); I(m, OpTypeFunction, B_FN, B_VOID); I(m, OpTypeFloat, B_FLOAT, 32); I(m, OpTypeVector, B_V4, B_FLOAT, 4);
    I(m, OpTypePointer, B_PV4O, Output, B_V4); I(m, OpVariable, B_PV4O, B_O0, Output); I(m, OpVariable, B_PV4O, B_O1, Output);
    I(m, OpConstant, B_FLOAT, B_C1, 0x3f800000u); I(m, OpConstant, B_FLOAT, B_C0, 0x3e800000u); I(m, OpConstantComposite, B_V4, B_VC, B_C1, B_C0, B_C0, B_C1);
    I(m, OpFunction, B_VOID, B_MAIN, 0, B_FN); I(m, OpLabel, B_LBL); I(m, OpStore, B_O0, B_VC); I(m, OpStore, B_O1, B_VC); I0(m, OpReturn); I0(m, OpFunctionEnd);
}

// ---------- spirv-val ----------
static int g_val = -1;   // spirv-val is installed: -1 not known yet
static int val_have(void) {
#if defined(_WIN32)
    g_val = 0;
#else
    if (g_val < 0) g_val = system("command -v spirv-val >/dev/null 2>&1") == 0;
#endif
    return g_val;
}
// spirv-val's verdict on a module for Vulkan 1.3 (the shared device's version): 1 valid, 0 refused, -1 not run
static int val(const uint32_t *w, size_t n) {
    if (!val_have()) return -1;
    char p[700]; snprintf(p, sizeof p, "%s/t_vk_spirv_fix.spv", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    FILE *f = fopen(p, "wb"); if (!f) return -1;
    fwrite(w, 4, n, f); fclose(f);
    char c[800]; snprintf(c, sizeof c, "spirv-val --target-env vulkan1.3 '%s' >/dev/null 2>&1", p);
    int r = system(c);
    remove(p);
    return r == 0;
}
static int g_val_checks, g_val_skipped;
static void t_val(const char *name, const uint32_t *w, size_t n, int want) {
    int v = val(w, n);
    if (v < 0) { g_val_skipped++; return; }
    g_val_checks++; t(name, v == want);
}

// ---------- Helpers on modules ----------
static int same(const uint32_t *a, size_t na, const mod_t *b) { return na == b->n && !memcmp(a, b->w, na * 4); }
static size_t count_dec(const uint32_t *w, size_t n, uint32_t op, uint32_t dec, int at) {   // instructions op whose word at holds dec
    size_t k = 0;
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == op && (size_t)at < (w[i] >> 16) && w[i + at] == dec) k++;
    return k;
}
// glslang's module in Super Mario Galaxy's form: each array type's OpDecorate ArrayStride, where the array is a struct's member, becomes that member's OpMemberDecorate
static size_t galaxy_form(const uint32_t *in, size_t n, uint32_t *out) {
    size_t k = 0;
    for (size_t i = 0; i < 5; i++) out[k++] = in[i];
    for (size_t i = 5; i < n; i += in[i] >> 16) {
        uint32_t wc = in[i] >> 16;
        if ((in[i] & 0xffff) == OpDecorate && wc == 4 && in[i + 2] == ArrayStride) {
            uint32_t arr = in[i + 1], st = 0, mem = 0;
            for (size_t j = 5; j < n && !st; j += in[j] >> 16)
                if ((in[j] & 0xffff) == OpTypeStruct) for (uint32_t q = 2; q < (in[j] >> 16); q++) if (in[j + q] == arr) { st = in[j + 1]; mem = q - 2; break; }
            if (st) { out[k++] = 5u << 16 | OpMemberDecorate; out[k++] = st; out[k++] = mem; out[k++] = ArrayStride; out[k++] = in[i + 3]; continue; }
        }
        for (uint32_t q = 0; q < wc; q++) out[k++] = in[i + q];
    }
    return k;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    mod_t a0, a, b0, b, e; char name[200]; guest_spirv_report r; size_t n; uint32_t *f;
    // ---------- Structural checks ----------
    module_a(&a0, 0); module_b(&b0, 0);
    t("check: the uniform-array vertex shader passes", guest_spirv_check(a0.w, a0.n) == NULL);
    t("check: the two-output fragment shader passes", guest_spirv_check(b0.w, b0.n) == NULL);
    t("check: Super Mario Galaxy's form passes (structurally valid; the fix-up makes it valid SPIR-V)", (module_a(&a, 1), guest_spirv_check(a.w, a.n) == NULL));
    t("check: shorter than the header refused", guest_spirv_check(a0.w, 4) != NULL);
    a = a0; a.w[0] = 0x12345678; t("check: a wrong magic number refused", guest_spirv_check(a.w, a.n) != NULL);
    a = a0; a.w[0] = 0x03022307u; t("check: big-endian SPIR-V refused", guest_spirv_check(a.w, a.n) && strstr(guest_spirv_check(a.w, a.n), "big-endian"));
    a = a0; a.w[1] = 0x00020000u; t("check: SPIR-V 2.0 refused", guest_spirv_check(a.w, a.n) != NULL);
    a = a0; a.w[1] = 0x00010700u; t("check: SPIR-V 1.7 refused", guest_spirv_check(a.w, a.n) != NULL);
    a = a0; a.w[1] = 0x00010600u; t("check: SPIR-V 1.6 passes", guest_spirv_check(a.w, a.n) == NULL);
    a = a0; a.w[3] = 0; t("check: an id bound of 0 refused", guest_spirv_check(a.w, a.n) != NULL);
    a = a0; a.w[4] = 1; t("check: a schema other than 0 refused", guest_spirv_check(a.w, a.n) != NULL);
    a = a0;   // the module ends OpStore (3 words), OpReturn, OpFunctionEnd: cut 3 words, OpStore runs past the end
    t("check: an instruction cut short refused", guest_spirv_check(a.w, a.n - 3) && strstr(guest_spirv_check(a.w, a.n - 3), "past the end"));
    a = a0; a.w[5] = OpCapability; t("check: a word count of 0 refused", guest_spirv_check(a.w, a.n) && strstr(guest_spirv_check(a.w, a.n), "word count 0"));
    a = a0; a.w[3] = A_BLK; t("check: an id at the bound refused", guest_spirv_check(a.w, a.n) != NULL);
    a = a0; { uint32_t t0[2] = {a.w[5], a.w[6]}; memmove(a.w + 5, a.w + 7, 3 * 4); a.w[8] = t0[0]; a.w[9] = t0[1]; }   // OpMemoryModel before OpCapability
    t("check: instructions out of the logical layout's order refused", guest_spirv_check(a.w, a.n) && strstr(guest_spirv_check(a.w, a.n), "order"));
    { mod_t x; header(&x, 4); I(&x, OpCapability, Shader); I(&x, OpEntryPoint, Vertex, 1, MAIN); I(&x, OpTypeVoid, 2);
      t("check: no memory model refused", guest_spirv_check(x.w, x.n) && strstr(guest_spirv_check(x.w, x.n), "OpMemoryModel")); }
    a = a0; a.w[6] = 2;   // Capability Matrix instead of Shader
    t("check: no Shader capability refused", guest_spirv_check(a.w, a.n) && strstr(guest_spirv_check(a.w, a.n), "Shader"));
    { mod_t x; header(&x, 4); I(&x, OpCapability, Shader); I(&x, OpMemoryModel, Logical, GLSL450); I(&x, OpTypeVoid, 1);
      t("check: no entry point refused", guest_spirv_check(x.w, x.n) && strstr(guest_spirv_check(x.w, x.n), "entry point")); }

    // ---------- ArrayStride on struct members ----------
    t_val("spirv-val: the array type's ArrayStride (form 0) is valid", a0.w, a0.n, 1);
    f = guest_spirv_fix(a0.w, a0.n, GUEST_SPIRV_STRIDES | GUEST_SPIRV_DUAL, &n, &r);
    t("strides: a module with its strides on the array types comes back as it is (no copy)", f == a0.w && n == a0.n && !r.strides_dropped && !r.strides_moved);
    module_a(&a, 1);
    t_val("spirv-val: Super Mario Galaxy's form (ArrayStride on the member) is refused", a.w, a.n, 0);
    f = guest_spirv_fix(a.w, a.n, GUEST_SPIRV_STRIDES, &n, &r);
    t("strides: Galaxy's form fixed into SPIR-V's form, word for word", f && f != a.w && same(f, n, &a0));
    snprintf(name, sizeof name, "strides: report moved %d dropped %d conflicts %d (1 1 0)", r.strides_moved, r.strides_dropped, r.stride_conflicts);
    t(name, r.strides_moved == 1 && r.strides_dropped == 1 && r.stride_conflicts == 0);
    if (f) { t("strides: the fixed module passes the structural checks", guest_spirv_check(f, n) == NULL); t_val("spirv-val: the fixed module is valid", f, n, 1); if (f != a.w) free(f); }
    f = guest_spirv_fix(a.w, a.n, GUEST_SPIRV_DUAL, &n, &r);
    t("strides: left alone when only the dual-source fix-up is asked for", f == a.w && n == a.n && !r.strides_dropped);
    module_a(&a, 2); module_a(&e, 0);
    f = guest_spirv_fix(a.w, a.n, GUEST_SPIRV_STRIDES, &n, &r);
    t("strides: the member's stride dropped where the type has the same one", f && same(f, n, &e) && r.strides_dropped == 1 && r.strides_moved == 0 && !r.stride_conflicts);
    if (f && f != a.w) free(f);
    module_a(&a, 3);
    f = guest_spirv_fix(a.w, a.n, GUEST_SPIRV_STRIDES, &n, &r);
    t("strides: a member stride that differs from the type's dropped and counted as a conflict, the type's kept", f && same(f, n, &e) && r.stride_conflicts == 1 && r.strides_dropped == 1);
    if (f && f != a.w) free(f);
    module_a(&a, 4); module_a4_fixed(&e);
    f = guest_spirv_fix(a.w, a.n, GUEST_SPIRV_STRIDES, &n, &r);
    t("strides: the stride of a member that is no array dropped, the array's moved", f && same(f, n, &e) && r.strides_dropped == 2 && r.strides_moved == 1);
    if (f) { t_val("spirv-val: that module fixed is valid", f, n, 1); if (f != a.w) free(f); }

    // ---------- The second blend source at Location 1 ----------
    t_val("spirv-val: the fragment shader with outputs at Locations 0 and 1 is valid", b0.w, b0.n, 1);
    t("dual: a fragment output at Location 1 without an Index makes a candidate", guest_spirv_dual_candidate(b0.w, b0.n) == 1);
    module_b(&e, 1);
    f = guest_spirv_fix(b0.w, b0.n, GUEST_SPIRV_DUAL, &n, &r);
    t("dual: Location 1 becomes Location 0 with Index 1, word for word", f && f != b0.w && same(f, n, &e) && r.dual_rewritten == 1);
    if (f) { t_val("spirv-val: the dual-source form is valid", f, n, 1); if (f != b0.w) free(f); }
    f = guest_spirv_fix(b0.w, b0.n, GUEST_SPIRV_STRIDES, &n, &r);
    t("dual: left alone when only the stride fix-up is asked for", f == b0.w && n == b0.n && !r.dual_rewritten);
    for (int form = 1; form <= 4; form++) {
        static const char *const what[] = {"", "the fixed form", "an explicit Index 0", "Index 1 already on the first output", "a vertex shader's outputs"};
        module_b(&b, form);
        f = guest_spirv_fix(b.w, b.n, GUEST_SPIRV_DUAL | GUEST_SPIRV_STRIDES, &n, &r);
        snprintf(name, sizeof name, "dual: %s is no candidate and comes back as it is", what[form]);
        t(name, !guest_spirv_dual_candidate(b.w, b.n) && f == b.w && n == b.n && !r.dual_rewritten);
    }

    // ---------- glslang's modules of the GPU test ----------
    static uint32_t g[4096]; size_t gn = galaxy_form(guest_ubo_vert_spv, sizeof guest_ubo_vert_spv / 4, g);
    t("glslang: guest_ubo.vert in Galaxy's form has a member ArrayStride and no type ArrayStride",
      count_dec(g, gn, OpMemberDecorate, ArrayStride, 3) == 1 && count_dec(g, gn, OpDecorate, ArrayStride, 2) == 0);
    t_val("spirv-val: guest_ubo.vert as glslang made it is valid", guest_ubo_vert_spv, sizeof guest_ubo_vert_spv / 4, 1);
    t_val("spirv-val: guest_ubo.vert in Galaxy's form is refused", g, gn, 0);
    f = guest_spirv_fix(g, gn, GUEST_SPIRV_STRIDES | GUEST_SPIRV_DUAL, &n, &r);
    t("glslang: guest_ubo.vert fixed: one type ArrayStride, no member ArrayStride, the same length as glslang's",
      f && n == sizeof guest_ubo_vert_spv / 4 && count_dec(f, n, OpMemberDecorate, ArrayStride, 3) == 0 && count_dec(f, n, OpDecorate, ArrayStride, 2) == 1 && r.strides_moved == 1);
    if (f) { t_val("spirv-val: guest_ubo.vert fixed is valid", f, n, 1); if (f != g) free(f); }
    const uint32_t *d = guest_dual_frag_spv; size_t dn = sizeof guest_dual_frag_spv / 4;
    t("glslang: guest_dual.frag is a candidate", guest_spirv_dual_candidate(d, dn) == 1);
    f = guest_spirv_fix(d, dn, GUEST_SPIRV_DUAL, &n, &r);
    t("glslang: guest_dual.frag fixed: no Location 1, one Index 1, four words longer",
      f && n == dn + 4 && r.dual_rewritten && count_dec(f, n, OpDecorate, Index, 2) == 1 && !guest_spirv_dual_candidate(f, n));
    if (f) {
        size_t loc1 = 0; for (size_t i = 5; i < n; i += f[i] >> 16) loc1 += (f[i] & 0xffff) == OpDecorate && (f[i] >> 16) == 4 && f[i + 2] == Location && f[i + 3] == 1;
        t("glslang: guest_dual.frag fixed has no output at Location 1", loc1 == 0);
        t_val("spirv-val: guest_dual.frag fixed is valid", f, n, 1);
        if (f != d) free(f);
    }
    if (g_val_skipped) printf("note: spirv-val is not installed: %d spirv-val checks not run (the structural and word-for-word checks ran)\n", g_val_skipped);
    else printf("spirv-val ran on %d modules\n", g_val_checks);
    printf("== t_vk_spirv_fix: %d checks, %d failed ==\n", g_total, g_fail);
    return g_fail != 0;
}
