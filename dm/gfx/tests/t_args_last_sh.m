// A shader reuses its argument block only when it was the stage's latest encode (args_last_sh): a binding change followed by another variant of the program must not leave the first with a stale block.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
uint64_t mtl_args_refused(void);
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static NvShader *variant(id<MTLLibrary> lib, NSString *name, int n) {
    NvShader *s = [NvShader new]; s->stage = 2; s->fn = [lib newFunctionWithName:name]; s->rf.nb = 1;
    static uint64_t identity = 0x7100000000000000ull; snprintf(s->key, sizeof s->key, "%016llx_b", (unsigned long long)(identity + n)); identity++;
    s->ae = [s->fn newArgumentEncoderWithBufferIndex:0]; s->arg_len = s->ae.encodedLength;
    s->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP, .rw=1, .reg=0, .aid=0};
    return s;
}
int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *p [[id(0)]]; };\n"
        "kernel void va(constant Args &a [[buffer(0)]]) { a.p[0] += 1; }\n"
        "kernel void vb(constant Args &a [[buffer(0)]]) { a.p[1] = 0xb; }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic shaders compile"); if (!lib) return 1;
    NvShader *a = variant(lib, @"va", 0), *b = variant(lib, @"vb", 1);
    uint8_t *data; if (posix_memalign((void **)&data, getpagesize(), 8192)) return 1;
    memset(data, 0, 8192); mtl_pool_add(data, 8192);
    uint32_t *x = (uint32_t *)data, *y = (uint32_t *)(data + 4096);
    NSError *e = nil;
    id<MTLComputePipelineState> pa = [g_dev newComputePipelineStateWithFunction:a->fn error:&e];
    id<MTLComputePipelineState> pb = [g_dev newComputePipelineStateWithFunction:b->fn error:&e];
    CHECK(pa && pb, "pipelines");
    mtl_sub_begin(); state_reset();
    id<MTL4ComputeCommandEncoder> en = enc_c_begin(NULL); args_dirty_all();
    uint64_t enc0, reuse0, pt0; mtl_args_stats(&enc0, &reuse0, &pt0); uint64_t ref0 = mtl_args_refused();
    S.ssbo[2][0] = (p_bind_buf){.stage=2, .index=0, .addr=(uint64_t)x, .size=64};
    CHECK(encode_args((id<MTLCommandEncoder>)en, a, 2, 0), "A encoded");
    enc_c_pso(en, pa); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1));
    uint64_t enc1, reuse1; mtl_args_stats(&enc1, &reuse1, &pt0);
    CHECK(enc1 == enc0 + 1 && reuse1 == reuse0, "the first encode builds a block");
    // the same shader, nothing changed: reused
    CHECK(encode_args((id<MTLCommandEncoder>)en, a, 2, 0), "A again");
    uint64_t enc2, reuse2; mtl_args_stats(&enc2, &reuse2, &pt0);
    CHECK(enc2 == enc1 && reuse2 == reuse1 + 1, "the same shader reuses its block when nothing changed");
    // a binding change (OP_BIND_SSBO's effect), then the other variant encodes and clears the flag
    S.ssbo[2][0] = (p_bind_buf){.stage=2, .index=0, .addr=(uint64_t)y, .size=64}; g_args_dirty[2] = 1;
    CHECK(encode_args((id<MTLCommandEncoder>)en, b, 2, 0), "B encoded");
    enc_c_pso(en, pb); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1));
    CHECK(g_args_dirty[2] == 0, "B's encode cleared the stage's dirty flag");
    uint64_t enc3, reuse3; mtl_args_stats(&enc3, &reuse3, &pt0);
    // back to A: its block predates the change, so it is encoded again, not reused
    CHECK(encode_args((id<MTLCommandEncoder>)en, a, 2, 0), "A after B");
    uint64_t enc4, reuse4; mtl_args_stats(&enc4, &reuse4, &pt0);
    CHECK(enc4 == enc3 + 1 && reuse4 == reuse3, "A re-encodes after B instead of reusing the stale block");
    CHECK(mtl_args_refused() == ref0 + 1, "the refused reuse is counted");
    enc_c_pso(en, pa); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1)); enc_c_end(en);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completed"); mtl_drain();
    CHECK(x[0] == 1, "A's first dispatch wrote the first buffer once");
    CHECK(y[0] == 1 && y[1] == 0xb, "A's second dispatch wrote the changed binding, B's wrote beside it");
    // a program switch dirties every stage, so a block is reused only after its own shader's encode
    args_dirty_all(); CHECK(g_last_sh[2] == NULL, "args_dirty_all forgets the stage's latest shader");
    printf("t_args_last_sh: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
