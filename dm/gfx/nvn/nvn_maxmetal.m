// Shaders the split missed, translated at run time by maxmetal (nvn_maxmetal.h). The request is what dmsh_build.py job_for gives the offline translator, read from the code area and
// the control fields SetShaders kept (no answer table: texture slots take the instruction's type, as the offline chain does for a key shader_facts.tsv lacks). The reflection is maxmetal's
// (the binding layout of dmsh_abi.h) with the fields the host reads beside it; a vertex shader gets dmsh_build.py vs_depth's two entry points (main0: the GL convention, main0_z: NVN's
// near=0). What the offline chain adds after translation, a translation has none of: the pulling, shadow-input, alpha test, logic operation and readable-tile libraries, the raster facts,
// and the shader's lines in c1_full.tsv and cb_extent.tsv. The host treats each as absent (its c1 gets its own 64 KiB, its uniform bindings are copied whole).
#import "nvn_maxmetal.h"
#include "dmsh_chain.h"
#include <maxmetal/maxmetal.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXMETAL_REV "919805d"   // the vendored revision (dm/third_party/maxmetal/README.md): a translation names it, and one of another revision is translated again

static NSString *g_dir;   // where translations are kept (nil: memory only)
static NSMutableDictionary<NSString *, NSDictionary *> *g_tr;   // key → @{@"json", @"msl"}: this run's translations and those read from g_dir
static NSMutableDictionary<NSString *, NSString *> *g_failed;   // key → why it could not be translated (not tried again this run)
static NSMutableDictionary<NSString *, id<MTLLibrary>> *g_lib;   // pc → its library, compiled once (the submit, warm-up and prefetch threads all load shaders)
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;   // the three tables; translating and compiling run outside it
static _Atomic uint64_t g_n, g_ns, g_cn, g_cns;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

void nvn_mm_init(NSString *dir) {
    os_unfair_lock_lock(&g_lock);
    g_tr = [NSMutableDictionary dictionary]; g_failed = [NSMutableDictionary dictionary]; g_lib = [NSMutableDictionary dictionary];
    g_dir = dir && [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil] ? dir : nil;
    os_unfair_lock_unlock(&g_lock);
}
int nvn_mm_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_NVN_MAXMETAL"); on = !(e && !strcmp(e, "0")); }
    return on;
}

static int stage_num(NSString *s) { return [s isEqual:@"vs"] ? 0 : [s isEqual:@"ps"] ? 1 : [s isEqual:@"cs"] ? 5 : -1; }
// key's translation (memory, else g_dir when it names this maxmetal and chain), or nil
static NSDictionary *tr_get(NSString *key) {
    os_unfair_lock_lock(&g_lock); NSDictionary *t = g_tr[key]; NSString *dir = g_dir; os_unfair_lock_unlock(&g_lock);
    if (t || !dir || !nvn_mm_on()) return t;
    NSString *base = [dir stringByAppendingPathComponent:[key stringByAppendingString:@"_b"]];
    NSData *jd = [NSData dataWithContentsOfFile:[base stringByAppendingString:@".json"]];
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (![j isKindOfClass:NSDictionary.class] || ![j[@"translator"] isEqual:@"maxmetal " MAXMETAL_REV] || ![j[@"chain"] isEqual:@DMSH_CHAIN] || stage_num(j[@"stage"]) < 0) return nil;
    NSString *msl = [NSString stringWithContentsOfFile:[base stringByAppendingString:@".metal"] encoding:NSUTF8StringEncoding error:nil];
    if (!msl) return nil;
    t = @{@"json": j, @"msl": msl};
    os_unfair_lock_lock(&g_lock); if (!g_tr[key]) g_tr[key] = t; t = g_tr[key]; os_unfair_lock_unlock(&g_lock);
    return t;
}
int nvn_mm_stage(NSString *key) { NSDictionary *t = tr_get(key); return t ? stage_num(t[@"json"][@"stage"]) : -1; }
NSDictionary *nvn_mm_json(NSString *pc) { return [pc hasSuffix:@"_b"] ? tr_get([pc substringToIndex:pc.length - 2])[@"json"] : nil; }

static int fail(NSString *key, char *why, size_t nwhy, NSString *msg) {
    snprintf(why, nwhy, "%s", msg.UTF8String);
    os_unfair_lock_lock(&g_lock); g_failed[key] = msg; os_unfair_lock_unlock(&g_lock);
    return -1;
}
int nvn_mm_translate(NSString *key, const uint8_t *code, uint32_t ncode, uint32_t c1_off, const uint32_t local[3], int early_z, char *why, size_t nwhy) {
    if (!nvn_mm_on()) { snprintf(why, nwhy, "off (DM_NVN_MAXMETAL=0)"); return -1; }
    os_unfair_lock_lock(&g_lock); NSString *failed = g_failed[key]; os_unfair_lock_unlock(&g_lock);
    if (failed) { snprintf(why, nwhy, "%s", failed.UTF8String); return -1; }
    int st = nvn_mm_stage(key);
    if (st >= 0) return st;
    if (!code || ncode < 0x80 || ncode >= 1u << 24) return fail(key, why, nwhy, [NSString stringWithFormat:@"code area of %u bytes", ncode]);
    uint64_t t0 = now_ns();
    // The code area: a 0x30-byte header, then for graphics the 0x50-byte SPH and the instructions, for compute zero padding and the instructions at +0x100 (or no header at all), c1 from c1_off on
    uint32_t magic = rd32(code), c1o = c1_off && c1_off < ncode ? c1_off : ncode;
    const uint8_t *at = code; size_t size = ncode;
    if (magic == 0x12345678) {
        uint32_t type = rd32(code + 0x30) >> 10 & 0xf;   // the SPH's shader type, as dmsh_build.py reads it
        if (type == 0 && (local[0] || local[1] || local[2])) return fail(key, why, nwhy, @"vertex shader in two parts (VertexA + VertexB; dmsh_build.py vertex_ab)");
        if (type == 0 || type == 1) st = 0;   // type 0 alone: read as a vertex shader, as Ryujinx does
        else if (type == 5) st = 1;
        else return fail(key, why, nwhy, [NSString stringWithFormat:@"SPH shader type %u (maxmetal translates vertex, pixel and compute shaders)", type]);
    } else {
        st = 5;
        if (!local[0] || !local[1] || !local[2]) return fail(key, why, nwhy, [NSString stringWithFormat:@"code area header %#x and no thread group in the control section", magic]);
        if (magic == 0x12345679) {
            for (uint32_t i = 0x30; i < 0x100; i++) if (code[i]) return fail(key, why, nwhy, @"compute code area +0x30..+0x100 is not all zero");
            at = code + 0x100; size = ncode - 0x100;
        }
    }
    NvnKey nk; memset(&nk, 0, sizeof nk); memset(nk.tex_type, -1, sizeof nk.tex_type);
    nk.early_fragment_tests = st == 1 && early_z && !(rd32(code + 0x30 + 19 * 4) & 2);   // dmsh_build.py early_fragment_tests: not for a shader that writes depth
    nk.depth_minus_w = st == 0;   // main0 in the GL convention; main0_z below
    if (st == 5) memcpy(nk.local, local, sizeof nk.local);
    if (c1o < ncode) { nk.c1 = code + c1o; nk.c1_size = ncode - c1o > 0x10000 ? 0x10000 : ncode - c1o; }
    ShaderKey sk; memset(&sk, 0, sizeof sk); sk.nvn = &nk;
    uint8_t *buf = calloc(1, size + 64);   // a little room after the code, as maxmetal's own callers give it
    memcpy(buf, at, size);
    ShaderInfo *si = maxwell_translate(buf, size, st, &sk);
    free(buf);
    if (!si->msl || !si->refl) { NSString *e = [NSString stringWithFormat:@"maxmetal: %s", si->error[0] ? si->error : "no output"]; maxwell_free(si); return fail(key, why, nwhy, e); }
    NSString *msl = @(si->msl), *entry_z = @"";
    if (st == 0) {   // dmsh_build.py vs_depth: main0 with z' = z/2 + w/2 (maxmetal's depth_minus_w), main0_z the same function without it
        NSString *fma = @"    out.pos.z = fma(out.pos.z, 0.5f, out.pos.w * 0.5f);\n";
        NSRange m = [msl rangeOfString:@"vertex main0_out main0(" options:NSBackwardsSearch];
        NSString *body = m.location != NSNotFound ? [msl substringFromIndex:m.location] : nil;
        if (body && [body containsString:fma]) {
            body = [[body stringByReplacingOccurrencesOfString:fma withString:@""] stringByReplacingOccurrencesOfString:@"vertex main0_out main0(" withString:@"vertex main0_out main0_z("];
            msl = [msl stringByAppendingFormat:@"\n%@", body]; entry_z = @"\"entry_z\": \"main0_z\", ";
        }
    }
    NSMutableString *c1 = [NSMutableString stringWithCapacity:nk.c1_size * 2];
    for (uint32_t i = 0; i < nk.c1_size; i++) [c1 appendFormat:@"%02x", nk.c1[i]];
    static const char *sname[] = {"vs", "ps", "", "", "", "cs"}, *kind[] = {"vertex", "fragment", "", "", "", "kernel"};
    NSString *tg = st == 5 ? [NSString stringWithFormat:@"[%u, %u, %u]", local[0], local[1], local[2]] : @"null";
    NSString *js = [NSString stringWithFormat:@"{\"key\": \"%@_b\", \"route\": \"B\", \"chain\": \"%s\", \"translator\": \"maxmetal %s\", \"stage\": \"%s\", \"entry\": \"main0\", %@"
                    "\"msl_entry_kind\": \"%s\", \"threadgroup_size\": %@, \"c1\": \"%@\", %s}", key, DMSH_CHAIN, MAXMETAL_REV, sname[st], entry_z, kind[st], tg, c1, si->refl];
    maxwell_free(si);
    NSData *jd = [js dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *j = [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil];
    if (![j isKindOfClass:NSDictionary.class]) return fail(key, why, nwhy, @"maxmetal's reflection is not JSON");
    atomic_fetch_add(&g_n, 1); atomic_fetch_add(&g_ns, now_ns() - t0);
    os_unfair_lock_lock(&g_lock); g_tr[key] = @{@"json": j, @"msl": msl}; NSString *dir = g_dir; os_unfair_lock_unlock(&g_lock);
    if (dir) {   // the library name's files, as the offline chain writes them; the reflection last, so a half-written pair is never taken
        NSString *base = [dir stringByAppendingPathComponent:[key stringByAppendingString:@"_b"]];
        if (![msl writeToFile:[base stringByAppendingString:@".metal"] atomically:YES encoding:NSUTF8StringEncoding error:nil] || ![jd writeToFile:[base stringByAppendingString:@".json"] atomically:YES])
            snprintf(why, nwhy, "translated, but not kept in %s", dir.UTF8String);
    }
    return st;
}

id<MTLLibrary> nvn_mm_library(id<MTLDevice> dev, NSString *pc, int *layer, NSError **err) {
    os_unfair_lock_lock(&g_lock); id<MTLLibrary> lib = g_lib[pc]; os_unfair_lock_unlock(&g_lock);
    NSString *msl = [pc hasSuffix:@"_b"] ? tr_get([pc substringToIndex:pc.length - 2])[@"msl"] : nil;
    if (layer) *layer = [msl containsString:@"render_target_array_index"];
    if (lib) return lib;
    if (!msl) { if (err) *err = [NSError errorWithDomain:@"dm" code:1 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@ has no run-time translation", pc]}]; return nil; }
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.preserveInvariance = YES;   // as the offline chain's -fpreserve-invariance (dmsh_build.py MAC_METAL): positions agree across shaders
    for (int attempt = 1;; attempt++) {   // the compiler service may drop the connection under load (MacNativeGames' totk_msl_library): try again
        NSError *e = nil; uint64_t t0 = now_ns();
        lib = [dev newLibraryWithSource:msl options:o error:&e];
        if (lib) { atomic_fetch_add(&g_cn, 1); atomic_fetch_add(&g_cns, now_ns() - t0); break; }
        if (attempt == 3 || ![e.localizedDescription containsString:@"XPC_ERROR_CONNECTION_INTERRUPTED"]) { if (err) *err = e; return nil; }
    }
    os_unfair_lock_lock(&g_lock); if (!g_lib[pc]) g_lib[pc] = lib; lib = g_lib[pc]; os_unfair_lock_unlock(&g_lock);
    return lib;
}

void nvn_mm_stats(uint64_t *translated, uint64_t *translate_ns, uint64_t *compiled, uint64_t *compile_ns) {
    *translated = atomic_load(&g_n); *translate_ns = atomic_load(&g_ns); *compiled = atomic_load(&g_cn); *compile_ns = atomic_load(&g_cns);
}
