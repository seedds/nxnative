// Run-time translation of shaders the split missed (nvn_maxmetal.m): small hand-assembled Maxwell programs in NVN code areas (the 0x30-byte header, the SPH, the instructions, c1 after
// them), each translated, its reflection converted as shader_load converts it (nvn_refl_from_json), its library compiled from the MSL, and its argument buffer checked against the layout
// shader_load requires (encodedLength = 8 * (largest arg_id + 1)). Then the translations are read back from their directory by a fresh start, and unsupported stages fail with a reason.
// The instruction encodings are maxmetal's own test assembler's (dm/third_party/maxmetal, tests/test_translate.c upstream).
#import "../nvn/nvn_maxmetal.h"
#import "../nvn/nvn_refl.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "dmsh_chain.h"

static int fail, n;
static void check(const char *what, long want, long got) {
    n++; int ok = want == got; fail += !ok;
    if (!ok) printf("  %-56s expected %ld, got %ld\n", what, want, got);
}

enum { PT = 7, RZ = 255 };
#define AL ((uint64_t)PT << 16)
static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t rr(uint64_t op, int d, int a) { return op | AL | (uint64_t)a << 8 | (uint64_t)d; }
static uint64_t movf(int d, float v) { return 0x0100000000000000ull | (uint64_t)fb(v) << 20 | 0xfull << 12 | AL | d; }
static uint64_t fmul_c(int d, int a, int bank, uint32_t off) { return rr(0x4c68000000000000ull | (uint64_t)bank << 34 | (uint64_t)(off / 4) << 20, d, a); }
static uint64_t ipa(int d, uint32_t addr, int mode, int mul) {
    return 0xe000000000000000ull | (uint64_t)mode << 54 | (uint64_t)RZ << 39 | (uint64_t)addr << 28 | (uint64_t)mul << 20 | AL | (uint64_t)RZ << 8 | d;
}
static uint64_t mufu_rcp(int d, int a) { return rr(0x5080000000000000ull | 4ull << 20, d, a); }
static uint64_t texs2d(int d0, int d1, int a, int b, int slot) {
    return rr(0xd800000000000000ull | 1ull << 53 | 4ull << 50 | (uint64_t)(0x20 + 8 * slot) / 4 << 36 | (uint64_t)d1 << 28 | (uint64_t)b << 20, d0, a);
}
static uint64_t ald(int d, uint32_t addr, int k) { return rr(0xefd8000000000000ull | (uint64_t)(k - 1) << 47 | (uint64_t)RZ << 39 | (uint64_t)addr << 20, d, RZ); }
static uint64_t ast(int s, uint32_t addr, int k) { return rr(0xeff0000000000000ull | (uint64_t)(k - 1) << 47 | (uint64_t)RZ << 39 | (uint64_t)addr << 20, s, RZ); }
static uint64_t s2r(int d, int sr) { return rr(0xf0c8000000000000ull | (uint64_t)sr << 20, d, 0); }
static uint64_t exit_(void) { return 0xe30000000007000full; }

typedef struct { uint64_t w[64]; int n; } Prog;
static int pc_of(int i) { return (i / 3 * 4 + i % 3 + 1) * 8; }
static void emit(Prog *p, uint64_t w) { p->w[p->n++] = w; }
// An NVN code area: header (magic 0x12345678 graphics, 0x12345679 compute) + SPH (graphics) or zero padding to +0x100 (compute), the instructions, then c1 (c1_len bytes, 0x40 + i)
static uint8_t *area(const Prog *src, int sph_type, uint32_t omap, uint32_t c1_len, uint32_t *ncode, uint32_t *c1_off) {
    Prog q = *src;
    while (q.n % 3 != 2) q.w[q.n++] = 0x50b0000000070f00ull;   // NOP
    q.w[q.n++] = 0xe2400fffff87000full;                          // NVN's padding: a branch to itself
    size_t head = sph_type < 0 ? 0x100 : 0x80, words = (size_t)q.n / 3 * 4 + 4;
    *c1_off = (uint32_t)(head + words * 8); *ncode = *c1_off + c1_len;
    uint8_t *b = calloc(1, *ncode);
    uint32_t magic = sph_type < 0 ? 0x12345679 : 0x12345678; memcpy(b, &magic, 4);
    if (sph_type >= 0) { uint32_t sph0 = (sph_type == 5 ? 2u : 1u) | 3u << 5 | (uint32_t)sph_type << 10; memcpy(b + 0x30, &sph0, 4); memcpy(b + 0x30 + 0x48, &omap, 4); }
    uint64_t *code = (uint64_t *)(b + head);
    for (int i = 0; i < q.n; i++) { if (i % 3 == 0) code[i / 3 * 4] = 0x001f8000fc0007e0ull; code[pc_of(i) / 8] = q.w[i]; }
    for (uint32_t i = 0; i < c1_len; i++) b[*c1_off + i] = (uint8_t)(0x40 + i);
    return b;
}

// Translate, convert, compile and check one program the way shader_load would take it
static void one(id<MTLDevice> dev, NSString *key, const Prog *p, int sph_type, uint32_t omap, uint32_t c1_len, int want_stage, const uint32_t local[3]) {
    uint32_t ncode, c1_off; uint8_t *code = area(p, sph_type, omap, c1_len, &ncode, &c1_off); char why[256] = "";
    static const uint32_t none[3];
    int st = nvn_mm_translate(key, code, ncode, c1_off, local ? local : none, 0, why, sizeof why);
    free(code);
    char what[96]; snprintf(what, sizeof what, "%s: stage", key.UTF8String);
    check(what, want_stage, st);
    if (st < 0) { printf("  %s: %s\n", key.UTF8String, why); return; }
    NSString *pc = [key stringByAppendingString:@"_b"];
    NSDictionary *j = nvn_mm_json(pc);
    snprintf(what, sizeof what, "%s: reflection names the chain", key.UTF8String); check(what, 1, [j[@"chain"] isEqual:@DMSH_CHAIN]);
    nvn_refl r; char err[160] = "";
    int rc = nvn_refl_from_json(j, &r, err, sizeof err);
    snprintf(what, sizeof what, "%s: reflection converts", key.UTF8String); check(what, 0, rc);
    if (rc) { printf("  %s: %s\n", key.UTF8String, err); return; }
    snprintf(what, sizeof what, "%s: c1 bytes", key.UTF8String); check(what, c1_len, r.c1_len);
    if (c1_len) { snprintf(what, sizeof what, "%s: c1 starts at c1_off", key.UTF8String); check(what, 0x40, r.c1[0]); }
    NSError *e = nil; int layer = -1;
    id<MTLLibrary> lib = nvn_mm_library(dev, pc, &layer, &e);
    snprintf(what, sizeof what, "%s: library compiles", key.UTF8String); check(what, 1, lib != nil);
    if (!lib) { printf("  %s: %s\n", key.UTF8String, e.localizedDescription.UTF8String); return; }
    snprintf(what, sizeof what, "%s: second call reuses the library", key.UTF8String); check(what, 1, nvn_mm_library(dev, pc, NULL, NULL) == lib);
    id<MTLFunction> f = [lib newFunctionWithName:j[@"entry"]];
    snprintf(what, sizeof what, "%s: entry point", key.UTF8String); check(what, 1, f != nil);
    if (st == 0) { snprintf(what, sizeof what, "%s: near=0 entry point main0_z", key.UTF8String); check(what, 1, j[@"entry_z"] && [lib newFunctionWithName:j[@"entry_z"]] != nil); }
    if (f && r.nb) {   // shader_load's argument buffer check
        int max_aid = -1; for (int i = 0; i < r.nb; i++) max_aid = MAX(max_aid, r.b[i].aid + (r.b[i].n ? r.b[i].n - 1 : 0));
        id<MTLArgumentEncoder> ae = [f newArgumentEncoderWithBufferIndex:r.abi];
        snprintf(what, sizeof what, "%s: argument buffer layout", key.UTF8String); check(what, 8 * (max_aid + 1), (long)ae.encodedLength);
    }
}

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { printf("SKIP: no Metal device\n"); return 0; }
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_maxmetal-%d", getpid()]];
    nvn_mm_init(dir);
    Prog color = {0}; emit(&color, movf(0, 1)); emit(&color, movf(1, 0.5f)); emit(&color, movf(2, 0.25f)); emit(&color, movf(3, 1)); emit(&color, fmul_c(0, 0, 1, 0)); emit(&color, exit_());
    one(dev, @"00000000000000a1", &color, 5, 0xf, 0x100, 1, NULL);   // pixel shader reading c1
    Prog tex = {0}; emit(&tex, ipa(4, 0x7c, 0, RZ)); emit(&tex, mufu_rcp(4, 4)); emit(&tex, ipa(5, 0x80, 1, 4)); emit(&tex, ipa(6, 0x84, 1, 4)); emit(&tex, texs2d(0, 2, 5, 6, 0)); emit(&tex, exit_());
    one(dev, @"00000000000000a2", &tex, 5, 0xf, 0, 1, NULL);        // pixel shader sampling texture slot 0 (no answer table: its instruction's 2D)
    Prog vs = {0}; emit(&vs, ald(0, 0x80, 4)); emit(&vs, ald(4, 0x90, 4)); for (int i = 0; i < 3; i++) emit(&vs, fmul_c(i, i, 3, 0)); emit(&vs, ast(0, 0x70, 4)); emit(&vs, ast(4, 0x80, 4)); emit(&vs, exit_());
    one(dev, @"00000000000000a3", &vs, 1, 0, 0, 0, NULL);           // vertex shader: attributes, uniform slot 0, both depth entry points
    Prog cs = {0}; emit(&cs, s2r(0, 0x21)); emit(&cs, exit_());
    static const uint32_t local[3] = {64, 1, 1};
    one(dev, @"00000000000000a4", &cs, -1, 0, 0, 5, local);         // compute shader: instructions at +0x100, thread group from the control section
    {   char why[256] = ""; uint32_t ncode, c1_off; static const uint32_t none[3]; uint8_t *code = area(&color, 2, 0, 0, &ncode, &c1_off);   // a tessellation control shader: not translated
        check("tessellation shader: refused", -1, nvn_mm_translate(@"00000000000000a5", code, ncode, c1_off, none, 0, why, sizeof why));
        check("tessellation shader: says why", 1, strstr(why, "SPH shader type 2") != NULL);
        why[0] = 0; check("a failed key fails again at once", -1, nvn_mm_translate(@"00000000000000a5", code, ncode, c1_off, none, 0, why, sizeof why));
        check("  with its reason", 1, strstr(why, "SPH shader type 2") != NULL);
        free(code); }
    nvn_mm_init(dir);   // a later start: the translations come back from the directory
    check("kept translation: vertex stage", 0, nvn_mm_stage(@"00000000000000a3"));
    check("kept translation: compute stage", 5, nvn_mm_stage(@"00000000000000a4"));
    check("kept translation: a refused key was not kept", -1, nvn_mm_stage(@"00000000000000a5"));
    check("kept translation: library compiles", 1, nvn_mm_library(dev, @"00000000000000a1_b", NULL, NULL) != nil);
    uint64_t tn, tns, cn, cns; nvn_mm_stats(&tn, &tns, &cn, &cns);
    check("stats: translations", 4, (long)tn);
    nvn_mm_init(nil);   // memory only: nothing from the directory
    check("no directory: nothing kept", -1, nvn_mm_stage(@"00000000000000a3"));
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    printf("%s: %d checks, %d failed\n", fail ? "FAIL" : "OK", n, fail);
    return fail != 0;
} }
