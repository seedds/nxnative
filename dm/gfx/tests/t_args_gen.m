// Argument blocks are valid by binding generation (g_args_gen / g_args_ugen, encode_args_), not by a stage flag the latest encode clears: after a binding change and another shader's
// encode on the same stage, the first shader writes its block again (PR #2249's sequence); with no change, shaders that take turns on a stage keep their blocks; a uniform-only change
// patches each block once. bind_stages never hands back the slot of the bound combined program for a new combination, so OP_BIND_PROGRAM sees the change.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static NvShader *shader(id<MTLLibrary> lib, NSString *name) {
    static uint64_t identity = 0x7200000000000000ull;
    NvShader *s = [NvShader new]; s->stage = 2; s->fn = [lib newFunctionWithName:name]; s->rf.nb = 2;
    snprintf(s->key, sizeof s->key, "%016llx_b", (unsigned long long)identity++);
    s->ae = [s->fn newArgumentEncoderWithBufferIndex:0]; s->arg_len = s->ae.encodedLength;
    s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 1};
    return s;
}
static uint64_t enc_n(void) { uint64_t e, r, p; mtl_args_stats(&e, &r, &p); return e; }
static uint64_t reuse_n(void) { uint64_t e, r, p; mtl_args_stats(&e, &r, &p); return r; }
static uint64_t patch_n(void) { uint64_t e, r, p; mtl_args_stats(&e, &r, &p); return p; }
static void bind_ssbo(void *addr) { p_bind_buf b = {.stage = 2, .index = 0, .addr = (uint64_t)addr, .size = 64}; state_op(OP_BIND_SSBO, &b); }
static void bind_ubo(void *addr) { p_bind_buf b = {.stage = 2, .index = 0, .addr = (uint64_t)addr, .size = 16}; state_op(OP_BIND_UBO, &b); }

static void composite_slots(void) {
    // NCMP + 1 vertex programs and one fragment program: every vertex program with the fragment one is a combination of its own
    enum { N = 65 }; static prog_t pv[N], pf = {.serial = 930000};
    NvShader *fs = [NvShader new]; fs->stage = 1; NvProg *fp = [NvProg new]; fp->sh[1] = fs; serial_put(pf.serial, fp);
    NSMutableArray *keep = [NSMutableArray array];
    for (int k = 0; k < N; k++) { pv[k].serial = 930001 + k; NvShader *vs = [NvShader new]; vs->stage = 0; NvProg *vp = [NvProg new]; vp->sh[0] = vs; serial_put(pv[k].serial, vp); [keep addObject:vs]; }
    memset(S.bst, 0, sizeof S.bst); S.prog = NULL;
    bind_stages(&pf, 2);
    const prog_t *first = NULL;
    for (int k = 0; k < N - 1; k++) { const prog_t *c = bind_stages(&pv[k], 1); if (!k) first = c; S.prog = c; }   // the ring full, the first combination next to be replaced
    S.prog = bind_stages(&pv[0], 1);
    CHECK(S.prog == first, "binding the first combination again finds its slot");
    const prog_t *np = bind_stages(&pv[N - 1], 1);
    CHECK(np != S.prog, "a new combination never takes the bound combination's slot");
    CHECK(np && prog_get(np)->sh[0] == keep[N - 1] && prog_get(np)->sh[1] == fs, "the new combination holds its own stages");
    CHECK(prog_get(S.prog)->sh[0] == keep[0], "the bound combination is left as it was");
    memset(S.bst, 0, sizeof S.bst); S.prog = NULL;
}

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *p [[id(0)]]; constant uint *c [[id(1)]]; };\n"
        "kernel void va(constant Args &a [[buffer(0)]]) { a.p[0] += a.c[0]; }\n"
        "kernel void vb(constant Args &a [[buffer(0)]]) { a.p[1] = 0xb; }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic shaders compile"); if (!lib) return 1;
    NvShader *a = shader(lib, @"va"), *b = shader(lib, @"vb");
    uint8_t *data; if (posix_memalign((void **)&data, getpagesize(), 16384)) return 1;
    memset(data, 0, 16384); mtl_pool_add(data, 16384);
    uint32_t *x = (uint32_t *)data, *y = (uint32_t *)(data + 4096), *u1 = (uint32_t *)(data + 8192), *u2 = (uint32_t *)(data + 12288);
    u1[0] = 1; u2[0] = 16;
    NSError *e = nil;
    id<MTLComputePipelineState> pa = [g_dev newComputePipelineStateWithFunction:a->fn error:&e], pb = [g_dev newComputePipelineStateWithFunction:b->fn error:&e];
    CHECK(pa && pb, "pipelines");
    mtl_sub_begin(); state_reset();
    id<MTL4ComputeCommandEncoder> en = enc_c_begin(NULL); args_dirty_all();
    #define RUN(sh, ps) do { CHECK(encode_args((id<MTLCommandEncoder>)en, sh, 2, 0), #sh " encoded"); enc_c_pso(en, ps); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1)); \
                             [en barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice]; } while (0)
    bind_ssbo(x); bind_ubo(u1);
    uint64_t e0 = enc_n(), r0 = reuse_n();
    RUN(a, pa);   // x[0] += 1
    CHECK(enc_n() == e0 + 1, "the first encode writes a block");
    RUN(a, pa);   // x[0] += 1
    CHECK(enc_n() == e0 + 1 && reuse_n() == r0 + 1, "the same shader with nothing changed reuses its block");
    // the sequence of PR #2249: a binding change, another shader of the stage writes its block, then the first shader again
    bind_ssbo(y);
    CHECK(args_changed(2), "a storage binding changes the stage's generation");
    RUN(b, pb);   // y[1] = 0xb
    CHECK(!args_changed(2), "the other shader's block is written at the new generation");
    uint64_t e1 = enc_n(), r1 = reuse_n();
    RUN(a, pa);   // y[0] += 1
    CHECK(enc_n() == e1 + 1 && reuse_n() == r1, "the first shader writes its block again after another shader's encode");
    // nothing changed: shaders that take turns on the stage keep their blocks
    uint64_t e2 = enc_n(), r2 = reuse_n();
    RUN(b, pb); RUN(a, pa); RUN(b, pb);   // y[0] += 1
    CHECK(enc_n() == e2 && reuse_n() == r2 + 3, "shaders taking turns with no binding change reuse their blocks");
    // a uniform-only change: each shader's block is patched once, then reused
    bind_ubo(u2);
    uint64_t e3 = enc_n(), p3 = patch_n(), r3 = reuse_n();
    RUN(a, pa);   // y[0] += 16
    RUN(b, pb);
    RUN(a, pa);   // y[0] += 16
    CHECK(enc_n() == e3 && patch_n() == p3 + 2 && reuse_n() == r3 + 1, "a uniform change patches each shader's block once");
    enc_c_end(en);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completed"); mtl_drain();
    CHECK(x[0] == 2, "the first two dispatches wrote the first storage binding");
    CHECK(y[0] == 34 && y[1] == 0xb, "after the change every dispatch wrote the new binding, with the uniform buffer bound at the time");
    composite_slots();
    printf("t_args_gen: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
