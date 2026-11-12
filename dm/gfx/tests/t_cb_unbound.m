// A uniform slot with no buffer behind it reads the zero bank and is reported once per shader, stage, slot and reason (cb_miss_first), not once a run: with one counter
// for every case, the first one hid all the others (Ocarina of Time's foliage compute shader a039399e6336de1b named cb3 or cb2, whichever its scene left unbound first).
// The shaders are the game's shape: A reads slot 3 only behind a word of slot 0 (the game's empty task range), so its unbound slot 3 is never read on the GPU.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static NvShader *shader(id<MTLLibrary> lib, NSString *name, const char *key) {
    NvShader *s = [NvShader new]; s->stage = 5; s->fn = [lib newFunctionWithName:name]; snprintf(s->key, sizeof s->key, "%s", key);
    s->ae = [s->fn newArgumentEncoderWithBufferIndex:0]; s->arg_len = s->ae.encodedLength; s->rf.nb = 3;
    s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 3, .aid = 1};
    s->rf.b[2] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 2};
    return s;
}
int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { constant uint *c0 [[id(0)]]; constant uint *c3 [[id(1)]]; device uint *o [[id(2)]]; };\n"
        "kernel void fa(constant Args &a [[buffer(0)]]) { a.o[0] = a.c0[0] + 1; if (a.c0[1] != 0) a.o[1] = a.c3[0] + 1; }\n"
        "kernel void fb(constant Args &a [[buffer(0)]]) { a.o[2] = a.c0[0] + 1; a.o[3] = a.c3[5] + 1; }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic shaders compile"); if (!lib) return 1;
    NvShader *a = shader(lib, @"fa", "7200000000000001_b"), *b = shader(lib, @"fb", "7200000000000002_b");
    CHECK(a->fn && b->fn && a->ae && b->ae, "functions and argument encoders"); if (!a->fn || !b->fn || !a->ae || !b->ae) return 1;
    size_t page = (size_t)getpagesize();
    uint8_t *data; if (posix_memalign((void **)&data, page, 2 * page)) return 1;
    memset(data, 0, 2 * page); mtl_pool_add(data, 2 * page);
    uint32_t *out = (uint32_t *)data, *cb = (uint32_t *)(data + page); for (int i = 0; i < 8; i++) out[i] = 0xdead0000u + i;
    id<MTLComputePipelineState> pa = [g_dev newComputePipelineStateWithFunction:a->fn error:&error], pb = [g_dev newComputePipelineStateWithFunction:b->fn error:&error];
    CHECK(pa && pb, "pipelines"); if (!pa || !pb) return 1;
    mtl_sub_begin(); state_reset();
    id<MTL4ComputeCommandEncoder> en = enc_c_begin(NULL); args_dirty_all();
    S.ssbo[5][0] = (p_bind_buf){.stage = 5, .index = 0, .addr = (uint64_t)out, .size = 64};
    // nothing bound at either uniform slot: both are reported for A, and only once
    CHECK(g_cb_miss_n == 0, "nothing reported before the first encode");
    CHECK(encode_args((id<MTLCommandEncoder>)en, a, 5, 0), "A encoded with both slots unbound");
    enc_c_pso(en, pa); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1));
    CHECK(g_cb_miss_n == 2, "A's two unbound slots are both reported");
    g_args_dirty[5] = 1; CHECK(encode_args((id<MTLCommandEncoder>)en, a, 5, 0), "A encoded again");
    CHECK(g_cb_miss_n == 2, "the same shader and slots are not reported twice");
    // another shader with the same slots unbound: the single counter suppressed these
    [en barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];
    g_args_dirty[5] = 1; CHECK(encode_args((id<MTLCommandEncoder>)en, b, 5, 0), "B encoded with both slots unbound");
    enc_c_pso(en, pb); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1)); enc_c_end(en);
    CHECK(g_cb_miss_n == 4, "a second shader's unbound slots are reported after the first's");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "first submission completed"); mtl_drain();
    CHECK(out[0] == 1 && out[2] == 1 && out[3] == 1, "unbound slots read 0 from the zero bank, at any offset");
    CHECK(out[1] == 0xdead0001u, "a read behind an untaken branch of an unbound slot stores nothing");
    // the game's case: slot 0 bound, its guard word 0, slot 3 left unbound. Then the guard taken with slot 3 bound
    cb[0] = 41; cb[1] = 0; cb[16] = 7;
    mtl_sub_begin(); en = enc_c_begin(NULL); args_dirty_all();
    S.ubo[5][0] = (p_bind_buf){.stage = 5, .index = 0, .addr = (uint64_t)cb, .size = 64};
    CHECK(encode_args((id<MTLCommandEncoder>)en, a, 5, 0), "A encoded with slot 0 bound");
    enc_c_pso(en, pa); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1)); enc_c_end(en);
    CHECK(g_cb_miss_n == 4, "a bound slot is not reported, and the unbound one not again");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "second submission completed"); mtl_drain();
    CHECK(out[0] == 42 && out[1] == 0xdead0001u, "the bound slot is read; the guarded read of the unbound slot is not reached");
    cb[1] = 1;
    mtl_sub_begin(); en = enc_c_begin(NULL); args_dirty_all();
    S.ubo[5][3] = (p_bind_buf){.stage = 5, .index = 3, .addr = (uint64_t)(cb + 16), .size = 64};
    CHECK(encode_args((id<MTLCommandEncoder>)en, a, 5, 0), "A encoded with both slots bound");
    enc_c_pso(en, pa); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1)); enc_c_end(en);
    CHECK(g_cb_miss_n == 4, "nothing is reported with both slots bound");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "third submission completed"); mtl_drain();
    CHECK(out[1] == 8, "control: the guarded read takes the bound slot's value");
    // the key: shader, stage, slot and reason each make a new case; the list is bounded, with one closing line
    CHECK(!cb_miss_first("7200000000000001_b", 5, 3, 0), "known case");
    CHECK(cb_miss_first("7200000000000001_b", 5, 3, 1), "the same slot unmapped is another case");
    CHECK(cb_miss_first("7200000000000001_b", 1, 3, 0), "another stage is another case");
    CHECK(cb_miss_first("7200000000000001_b", 5, 4, 0), "another slot is another case");
    int added = 0; char name[24];
    for (int i = 0; i < 2 * CB_MISS_MAX; i++) { snprintf(name, sizeof name, "%016x_b", 0x73000000 + i); added += cb_miss_first(name, 0, 0, 0); }
    CHECK(g_cb_miss_n == CB_MISS_MAX && added == CB_MISS_MAX - 7, "the list stops at its limit");
    CHECK(g_cb_miss_over == 2 * CB_MISS_MAX - added, "cases past the limit are counted, with one closing line");
    CHECK(!cb_miss_first("7200000000000002_b", 5, 0, 0), "a listed case stays known past the limit");
    printf("t_cb_unbound: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
