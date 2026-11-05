// A clean 1080p movie texture reaches the bound 1440p output through the native copy.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static tex_t movie_texture(uint64_t serial, unsigned w, unsigned h) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8,
                       .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1,
                       .flags = NVN_TEXTURE_FLAGS_IMAGE};
    nvn_tex_layout(&t.d, &t.L);
    mtl_tex_init(&t);
    return t;
}

int main(void) { @autoreleasepool {
    mtl_init();
    tex_t src = movie_texture(101, 1920, 1080), dst = movie_texture(102, 2560, 1440);
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
    if (g_ndve_decoded) return 1;
    g_ndve_decoded = ndve_image(convert, 0, NULL);
    if (!g_ndve_decoded || !ndve_direct_draw(scale) || g_ndve_decoded) return 1;
    if (!mtl_wait(mtl_sub_commit(), 5000000000ull) || mtl_last_commit_error()) return 1;

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
    puts("NDVE movie: submission lifetime and three output samples passed");
    return 0;
} }
