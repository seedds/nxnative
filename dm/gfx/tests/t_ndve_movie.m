// A clean 1080p movie texture reaches the bound 1440p output through the native copy.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static tex_t movie_texture(uint64_t serial, unsigned w, unsigned h, unsigned flags) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                       .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1,
                       .flags = flags};
    nvn_tex_layout(&t.d, &t.L);
    mtl_tex_init(&t);
    return t;
}

typedef struct { uint8_t bytes[16384]; size_t n; } movie_chain;
static uint8_t *emit(movie_chain *c, unsigned op, const void *a, size_t n) {
    size_t size = (sizeof(op_hdr) + n + 7) & ~7ull;
    assert(c->n + size <= sizeof c->bytes);
    uint8_t *p = c->bytes + c->n;
    op_hdr h = {op, (uint32_t)size}; memcpy(p, &h, sizeof h);
    if (n) memcpy(p + sizeof h, a, n);
    c->n += size; return p;
}
static void chain_checks(id<MTLTexture> decoded, NvShader *output_shader) {
    static const char *const keys[] = {
        "ba079f68ae288dba_b", "a8b4fb48fee0ccf8_b", "e2eebd7ba18fe35f_b",
        "e6be82d835fbe4d3_b", "9442573ee4ace011_b", "0a56002cdc7e1fce_b",
        "0a56002cdc7e1fce_b", "d15370a382630d4e_b", "e6be82d835fbe4d3_b",
        "7f7ff0f36b54d90f_b", "a8b4fb48fee0ccf8_b", "5a7d69b067a7f011_b", "4f41ccf24f163e0d_b",
    };
    prog_t programs[13] = {0}; NvShader *shaders[13];
    movie_chain chain = {0};
    p_bind_handles dst = {.stage=5, .first=1, .n=1, .h={102}};
    uint8_t *dst_op = emit(&chain, OP_BIND_IMG, &dst, sizeof dst);
    uint8_t *dispatches[13];
    for (unsigned i = 0; i < 13; i++) {
        programs[i].serial = 900000 + i;
        NvProg *p = [NvProg new];
        shaders[i] = i == 12 ? output_shader : [NvShader new];
        strlcpy(shaders[i]->key, keys[i], sizeof shaders[i]->key);
        shaders[i]->stage = 5; p->sh[5] = shaders[i];
        serial_put(programs[i].serial, p);
        p_bind_program bind = {.prog=&programs[i], .stages=0x20};
        emit(&chain, OP_BIND_PROGRAM, &bind, sizeof bind);
        p_dispatch dispatch = {.x=1, .y=1, .z=1};
        dispatches[i] = emit(&chain, OP_DISPATCH, &dispatch, sizeof dispatch);
    }
    emit(&chain, OP_END, NULL, 0);
    mtl_sub_begin(); state_reset(); S.img[5][1] = 0;
    g_ndve_decoded = decoded;
    shaders[3]->key[0] = 'x'; ndve_direct_chain(chain.bytes);
    assert(!g_ndve_skip_end && g_ndve_decoded == decoded);
    shaders[3]->key[0] = keys[3][0];
    p_dispatch *indirect = (void *)(dispatches[4] + sizeof(op_hdr));
    indirect->ind = 1; ndve_direct_chain(chain.bytes);
    assert(!g_ndve_skip_end && g_ndve_decoded == decoded); indirect->ind = 0;
    p_bind_handles *image = (void *)(dst_op + sizeof(op_hdr));
    image->h[0] = 0; ndve_direct_chain(chain.bytes);
    assert(!g_ndve_skip_end && g_ndve_decoded == decoded); image->h[0] = 102;
    ((op_hdr *)dst_op)->op = OP_COPY_B2B; ndve_direct_chain(chain.bytes);
    assert(!g_ndve_skip_end && g_ndve_decoded == decoded); ((op_hdr *)dst_op)->op = OP_BIND_IMG;
    ndve_direct_chain(chain.bytes);
    assert(g_ndve_skip_end == dispatches[12] && !g_ndve_decoded);
    uint64_t before = g_gp_disp;
    mtl_exec(chain.bytes);
    assert(!g_ndve_skip_end && g_gp_disp == before);
    assert(S.cprog == &programs[12] && S.img[5][1] == 102);
    assert(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error());
    puts("NDVE chain: strict matching, rejected variants, skipped dispatches and preserved bindings passed");
}

int main(void) { @autoreleasepool {
    mtl_init();
    tex_t src = movie_texture(101, 1920, 1080, NVN_TEXTURE_FLAGS_IMAGE), dst = movie_texture(102, 2560, 1440, 0);
    mtl_reg_image(101, &src, NULL);
    mtl_reg_image(102, &dst, NULL);
    id<MTLTexture> in = tex_mtl(&src), out = tex_mtl(&dst);
    id<MTLCommandBuffer> clear = q_cb();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = in;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    [[clear renderCommandEncoderWithDescriptor:rp] endEncoding];
    [clear commit];
    [clear waitUntilCompleted];
    if (clear.error) return 1;

    NvShader *convert = [NvShader new], *scale = [NvShader new];
    convert->rf.nb = scale->rf.nb = 1;
    convert->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .reg = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .rw = 1};
    scale->rf.b[0] = convert->rf.b[0];
    scale->rf.b[0].reg = 1;
    S.img[5][0] = 101;
    S.img[5][1] = 102;
    g_ndve_decoded = in;
    mtl_sub_begin();
    g_ndve_decoded = ndve_image(convert, 0, S.img[5][0], NULL);
    tex_t output_desc = {0};
    id<MTLTexture> target = ndve_image(scale, 1, S.img[5][1], &output_desc);
    if (!target || (target.usage & MTLTextureUsageShaderWrite)) return 1;
    if (!ndve_scale(g_ndve_decoded, target, &output_desc)) return 1;
    g_ndve_decoded = nil;
    if (!mtl_wait(mtl_sub_commit(), 5000000000ull) || mtl_last_commit_error()) return 1;

    chain_checks(in, scale);

    id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 * 3 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> read = q_cb();
    id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
    const MTLOrigin samples[] = {MTLOriginMake(0, 0, 0), MTLOriginMake(1280, 720, 0), MTLOriginMake(2559, 1439, 0)};
    for (unsigned i = 0; i < 3; i++)
        [blit copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:samples[i] sourceSize:MTLSizeMake(1, 1, 1)
                     toBuffer:pixels destinationOffset:256 * i destinationBytesPerRow:256 destinationBytesPerImage:256];
    [blit endEncoding];
    [read commit];
    [read waitUntilCompleted];
    if (read.error) return 1;
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t *rgba = (const uint8_t *)pixels.contents + 256 * i;
        if (abs(rgba[0] - 64) > 1 || abs(rgba[1] - 128) > 1 || abs(rgba[2] - 191) > 1 || rgba[3] != 255) {
            fprintf(stderr, "NDVE sample %u: %u %u %u %u\n", i, rgba[0], rgba[1], rgba[2], rgba[3]);
            return 1;
        }
    }
    puts("NDVE movie: render target without shader-write usage and three output samples passed");
    return 0;
} }
