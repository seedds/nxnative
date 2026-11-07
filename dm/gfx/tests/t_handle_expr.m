// The optional proof must fail closed on malformed metadata or unknown values.
#import <Foundation/Foundation.h>
#include "../nvn/nvn_refl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failed;
#define CHECK(x) do { checks++; if (!(x)) { failed++; fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
static NSDictionary *proof(NSArray *nodes, NSArray *pools) {
    return @{@"version":@1, @"nodes":nodes, @"pools":pools};
}
static nvn_hp_prog parse(id p, int writable) {
    NSMutableDictionary *j = [@{@"route":@"B", @"bindings":@[
        @{@"kind":@"cbuf", @"reg":@1, @"arg_id":@0, @"msl_name":@"cb", @"msl_type":@"constant C*"},
        @{@"kind":@"sbuf", @"reg":@2, @"arg_id":@1, @"msl_name":@"sb", @"msl_type":@"const device B*", @"rw":@(writable)},
        @{@"kind":@"tex_pool", @"arg_id":@2, @"msl_name":@"pool", @"elem":@"texture2d<float>"}]} mutableCopy];
    if (p) j[@"pool_handles"] = p;
    nvn_refl r; char err[160]; CHECK(nvn_refl_from_json(j, &r, err, sizeof err) == 0);
    nvn_hp_prog result = {0}; if (r.handles) result = *r.handles;
    free(r.c1); free(r.handles); return result;
}
typedef struct { int known; unsigned calls; } read_ctx;
static int read_word(void *v, int cb, unsigned reg, uint64_t off, uint32_t *out) {
    read_ctx *ctx = v; ctx->calls++;
    if (!ctx->known) return 0;
    if (cb && reg == 1 && off == 132) { *out = 7; return 1; }
    if (!cb && reg == 2 && off == 36) { *out = 123; return 1; }
    return 0;
}
int main(int argc, const char **argv) { @autoreleasepool {
    NSArray *roots = @[@{@"binding":@2, @"roots":@[@0]}];
    nvn_hp_prog p = parse(proof(@[@[@"const", @7]], roots), 0);
    uint32_t out[NVN_HP_MAX_ROOTS]; read_ctx ctx = {1, 0};
    CHECK(nvn_hp_valid(&p)); CHECK(nvn_hp_eval(&p, read_word, &ctx, out)); CHECK(out[0] == 7); CHECK(!ctx.calls);
    NSArray *bad = @[[NSNull null], @[], @1, @"x", @{},
        @{@"version":@2, @"nodes":@[@[@"const", @7]], @"pools":roots},
        proof(@[@[@"const", @-1]], roots), proof(@[@[@"const", @4294967296ULL]], roots),
        proof(@[@[@"const", @YES]], roots), proof(@[@[@"const", @1.0]], roots),
        proof(@[@[@"const", @"7"]], roots), proof(@[@[@"future", @7]], roots),
        proof(@[@[@"const"]], roots), proof(@[@[@"const", @7, @0]], roots),
        proof(@[@[@"add_u", @0, @0]], roots), proof(@[@[@"cb", @1, @0, @0]], roots),
        proof(@[@[@"const", @7]], @[]), proof(@[@[@"const", @7]], @[@{@"binding":@3, @"roots":@[@0]}]),
        proof(@[@[@"const", @7]], @[@{@"binding":@2, @"roots":@[@1]}]),
        proof(@[@[@"const", @7]], @[roots[0], roots[0]])];
    for (id b in bad) CHECK(!parse(b, 0).nroots);
    CHECK(!parse(nil, 0).nroots);
    NSArray *mem = @[@[@"const", @8], @[@"cb", @1, @0, @1], @[@"sb", @2, @1, @2]];
    NSArray *memroots = @[@{@"binding":@2, @"roots":@[@2]}];
    p = parse(proof(mem, memroots), 0);
    CHECK(nvn_hp_eval(&p, read_word, &ctx, out)); CHECK(out[0] == 123);
    ctx.known = 0; CHECK(!nvn_hp_eval(&p, read_word, &ctx, out));
    CHECK(!parse(proof(mem, memroots), 1).nroots);
    // Arithmetic overflow, invalid shifts and unknown selected arms stay unknown.
    p = (nvn_hp_prog){.n=3, .nroots=1, .roots={2},
        .nodes={{NVN_HP_CONST, INT32_MAX}, {NVN_HP_CONST, 1}, {NVN_HP_ADD_I, 0, 1}}};
    CHECK(!nvn_hp_eval(&p, read_word, &ctx, out));
    p.nodes[2].op = NVN_HP_ADD_U; CHECK(nvn_hp_eval(&p, read_word, &ctx, out)); CHECK(out[0] == 0x80000000u);
    p.nodes[2].op = NVN_HP_SHL_I; CHECK(!nvn_hp_eval(&p, read_word, &ctx, out));
    p.nodes[0].a = 1; p.nodes[1].a = 32; CHECK(!nvn_hp_eval(&p, read_word, &ctx, out));
    p.nodes[2].op = NVN_HP_SHR_U; CHECK(!nvn_hp_eval(&p, read_word, &ctx, out));
    p = (nvn_hp_prog){.n=4, .nroots=1, .roots={3}, .nodes={
        {NVN_HP_CONST, 1}, {NVN_HP_CONST, 77}, {NVN_HP_CB, 1, 0, 0}, {NVN_HP_SELECT, 0, 1, 2}}};
    CHECK(nvn_hp_eval(&p, read_word, &ctx, out)); CHECK(out[0] == 77);
    p.nodes[0].a = 0; CHECK(!nvn_hp_eval(&p, read_word, &ctx, out));
    // Differential unsigned arithmetic and bit operations at both ends of uint32.
    uint32_t rng = 744;
    for (unsigned i=0; i<10000; i++) {
        rng = rng*1664525u+1013904223u; uint32_t a=rng;
        rng = rng*1664525u+1013904223u; uint32_t b=rng;
        p=(nvn_hp_prog){.n=3,.nroots=1,.roots={2},.nodes={{NVN_HP_CONST,a},{NVN_HP_CONST,b},{NVN_HP_ADD_U,0,1}}};
        CHECK(nvn_hp_valid(&p)); CHECK(nvn_hp_eval(&p,read_word,&ctx,out)); CHECK(out[0]==a+b);
        p.nodes[2].op=NVN_HP_AND; CHECK(nvn_hp_eval(&p,read_word,&ctx,out)); CHECK(out[0]==(a&b));
        p.nodes[2].op=NVN_HP_MIN_U; CHECK(nvn_hp_eval(&p,read_word,&ctx,out)); CHECK(out[0]==(a<b?a:b));
        p.nodes[2].op=NVN_HP_NE; CHECK(nvn_hp_eval(&p,read_word,&ctx,out)); CHECK(out[0]==(a!=b));
        p.nodes[1].a=b&31; p.nodes[2].op=NVN_HP_SHR_U; CHECK(nvn_hp_eval(&p,read_word,&ctx,out)); CHECK(out[0]==(a>>(b&31)));
        p.nodes[2].a=3; CHECK(!nvn_hp_valid(&p)); CHECK(!nvn_hp_eval(&p,read_word,&ctx,out));
    }
    p.n=NVN_HP_MAX_NODES+1; CHECK(!nvn_hp_valid(&p)); CHECK(!nvn_hp_eval(&p,read_word,&ctx,out));
    p.n=1; p.nroots=NVN_HP_MAX_ROOTS+1; CHECK(!nvn_hp_valid(&p)); CHECK(!nvn_hp_eval(&p,read_word,&ctx,out));
    // Optional local integration check: every proof emitted by a full build
    // must survive conversion, rather than silently falling back at load time.
    if (argc == 2) {
        NSString *dir = @(argv[1]); unsigned proofs = 0;
        NSArray *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil];
        CHECK(files != nil);
        for (NSString *file in files) if ([file hasSuffix:@"_b.json"]) {
            NSData *data = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:file]];
            NSDictionary *j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if (!j[@"pool_handles"]) continue;
            nvn_refl r; char err[160];
            CHECK(!nvn_refl_from_json(j,&r,err,sizeof err));
            CHECK(r.handles && nvn_hp_valid(r.handles));
            free(r.c1); free(r.handles); proofs++;
        }
        printf("%u generated shader proofs checked\n", proofs); CHECK(proofs > 0);
    }
    printf("%u checks, %u failed\n", checks, failed); return failed != 0;
} }
