// A samplerless buffer fetch still uses the complete type-1 handle: either uniform word may hold the texture ID. Exercise production reflection and argument encoding,
// both orders, a one-word handle, pool-relative IDs, and an inline update to the second word before the next dispatch. Synthetic buffers only.
#include "../nvn/nvn_mtl.m"
#include <unistd.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    alarm(30); mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A { texture_buffer<uint> t [[id(0)]]; };\n"
        "kernel void k(constant A&a [[buffer(0)]], device uint*out [[buffer(1)]]) { out[0]=a.t.read(0u).x; }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic library compiles"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvShader *sh = [NvShader new]; sh->stage = 5; strcpy(sh->key, "0123456789abcdef_b");
    sh->fn = [lib newFunctionWithName:@"k"]; sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len = sh->ae.encodedLength;
    NSDictionary *meta = @{@"route": @"B", @"bindings": @[@{@"kind": @"tbuf_ubo", @"reg": @1, @"off": @0, @"sreg": @2, @"soff": @0,
        @"arg_id": @0, @"msl_name": @"t", @"msl_type": @"texture_buffer<uint>"}]};
    char message[256] = "";
    CHECK(!nvn_refl_from_json(meta, &sh->rf, message, sizeof message), "two-word buffer reflection converts");
    CHECK(sh->rf.b[0].soff == 1 && sh->rf.b[0].sreg == 2, "a second word at byte zero is retained");
    id<MTLComputePipelineState> pso = [g_dev newComputePipelineStateWithFunction:sh->fn error:&error];
    CHECK(pso != nil, "compute pipeline compiles"); if (!pso) return 1;
    uint8_t *mem = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED, "pool allocated"); if (mem == MAP_FAILED) return 1;
    memset(mem, 0, 65536); mtl_pool_add(mem, 65536);
    tex_t tex[2];
    for (int i = 0; i < 2; i++) {
        *(uint32_t *)(void *)(mem + 8192 + i * 4096) = 100u + (uint32_t)i;
        tex[i] = (tex_t){.magic = MAGIC('T','E','X',0), .serial = 99720u + (uint64_t)i, .mem = mem + 8192 + i * 4096, .off = 8192 + i * 4096};
        tex[i].d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32UI, .width = 4, .height = 1, .depth = 1, .levels = 1, .samples = 1};
        mtl_tex_init(&tex[i]); mtl_reg_tex(701 + i, &tex[i], NULL);
    }
    id<MTLBuffer> result = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(result);
    for (int mode = 0; mode < 3; mode++) {
        uint32_t *first = (uint32_t *)(void *)mem, *second = (uint32_t *)(void *)(mem + 4096);
        *first = mode == 0 ? 1u : 7u << 20; *second = mode == 0 ? 7u << 20 : 1u;
        if (mode == 2) *first = 1u;
        sh->rf.b[0].soff = mode == 2 ? 0 : 1;
        state_reset(); mtl_sub_begin(); S.tex_base = 700;
        S.ubo[5][1] = (p_bind_buf){.addr = (uint64_t)mem, .size = 4};
        S.ubo[5][2] = (p_bind_buf){.addr = (uint64_t)mem + 4096, .size = 4}; args_dirty_all();
        for (int draw = 0; draw < 2; draw++) {
            if (draw) { uint32_t next = 2; mtl_sub_update(&next, (uint64_t)mem + (mode == 0 || mode == 2 ? 0 : 4096), 4); }
            id<MTL4ComputeCommandEncoder> en = dispatch_enc(); enc_c_pso(en, pso);
            CHECK(encode_args((id<MTLCommandEncoder>)en, sh, 5, 0), "production buffer arguments encode");
            enc_c_args(en, result.gpuAddress + 4 * draw, 1); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1));
        }
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "GPU submission completes");
        const uint32_t *out = result.contents;
        CHECK(out[0] == 100, "the texture ID resolves in either word, or in one word alone");
        CHECK(out[1] == 101, "the next dispatch reads the updated texture word");
    }
    mtl_pool_del(mem);
    printf("t_tbuf_handle: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
