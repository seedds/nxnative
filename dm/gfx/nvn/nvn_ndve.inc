// nn::ndve's movie network (Ocarina of Time's title and opening movies) run by the host's kernels (dm/gfx/msl/ndve_engine.metal) instead of its translated dispatches:
// DM_NDVE_ENGINE=1. Included by nvn_mtl.m; shaders.md, "The NDVE movie network by the host". A movie frame is 14 compute dispatches: the colour conversion 49dff010
// (NV12 -> RGBA8), eleven of a U-Net over the picture packed 2x2 at 960x540 (16 / 32 / 64 channels, HMMA.16816.F16 convolutions), 5a7d69b0 (the last convolution,
// added to the picture, into an RGB10A2 image) and the resampler 4f41ccf2 (to 2560x1440). The host skips the eleven, computes the whole network at 5a7d69b0 from the
// picture it reads, with the weights and biases the game bound, into the image it writes, and runs its own resampler at 4f41ccf2. 49dff010 stays the game's.
// The kernels do the translated shaders' arithmetic in their order (each HMMA as dmsh_build.py's HMMA_BODY does it), so the output bits are theirs; the translated
// shaders also have a lane race that changes a few outputs from run to run, and the kernels give the result every run agrees with elsewhere.
// Recognition: the shader keys in order and the network's weight table (each layer's weights and biases at fixed offsets from the first layer's). Anything else
// (another network, other bindings or sizes, a dispatch out of order, no library) leaves the game's dispatches running, logged once; a deviation after the sequence was
// taken (some dispatches already skipped) turns the host's network off for good, and that frame's movie picture is not updated.

enum { NDVE_N = 14, NDVE_WBYTES = 0x3c320 };
static const char k_ndve_keys[NDVE_N][17] = {"49dff010b9832251", "ba079f68ae288dba", "a8b4fb48fee0ccf8", "e2eebd7ba18fe35f", "e6be82d835fbe4d3", "9442573ee4ace011",
    "0a56002cdc7e1fce", "0a56002cdc7e1fce", "d15370a382630d4e", "e6be82d835fbe4d3", "7f7ff0f36b54d90f", "a8b4fb48fee0ccf8", "5a7d69b067a7f011", "4f41ccf24f163e0d"};
// each network dispatch's weights / biases (storage buffer registers 2 / 3), bytes from the first layer's weights (the table of the movies' network)
static const uint32_t k_ndve_w[NDVE_N] = {0, 0x0, 0x1300, 0x2600, 0x3700, 0x8000, 0xc100, 0x1e200, 0x30300, 0x34400, 0x38d00, 0x39e00, 0x3b100, 0};
static const uint32_t k_ndve_b[NDVE_N] = {0, 0x1200, 0x2500, 0x3600, 0x7f00, 0xc000, 0x1e100, 0x30200, 0x34300, 0x38c00, 0x39d00, 0x3b000, 0x3c300, 0};

// The host's network: its kernels (ndve_engine.metal), its tensors (f16 NHWC with a zero border), one layer table
typedef struct { uint32_t W, H, tiles_x, tiles_y; } ndve_args;
enum { NT_X0, NT_A, NT_S16, NT_B32, NT_S32, NT_E32, NT_C64, NT_D64, NT_N };
static const int k_ndve_t[NT_N][3] = {{960, 540, 16}, {960, 540, 16}, {960, 540, 16}, {480, 270, 32}, {480, 270, 32}, {480, 270, 32}, {240, 135, 64}, {240, 135, 64}};
enum { NK_PACK, NK_C16, NK_C32, NK_C64, NK_D16, NK_D32, NK_U64, NK_U32, NK_OUT, NK_LUMA, NK_RS, NK_N };
static const char *k_ndve_kern[NK_N] = {"pack_input", "conv3s_16_16", "conv3s_32_32", "conv3s_64_64", "down_16_32", "down_32_64", "up_64_32", "up_32_16", "out_16", "rs_luma", "resample"};
static const int k_ndve_ry[NK_N] = {0, 4, 2, 1, 2, 1, 1, 2, 4, 0, 0};
typedef struct { int k, in, out, skip, layer; } ndve_layer;   // layer: index into k_ndve_w / k_ndve_b (the game's dispatch it stands for)
static const ndve_layer k_ndve_l[] = {
    {NK_PACK, -1, NT_X0, -1, 0}, {NK_C16, NT_X0, NT_A, -1, 1}, {NK_C16, NT_A, NT_S16, -1, 2}, {NK_D16, NT_S16, NT_B32, -1, 3}, {NK_C32, NT_B32, NT_S32, -1, 4},
    {NK_D32, NT_S32, NT_C64, -1, 5}, {NK_C64, NT_C64, NT_D64, -1, 6}, {NK_C64, NT_D64, NT_C64, -1, 7}, {NK_U64, NT_C64, NT_B32, NT_S32, 8}, {NK_C32, NT_B32, NT_E32, -1, 9},
    {NK_U32, NT_E32, NT_A, NT_S16, 10}, {NK_C16, NT_A, NT_X0, -1, 11}, {NK_OUT, NT_X0, -1, -1, 12}};
enum { NDVE_NL = sizeof k_ndve_l / sizeof k_ndve_l[0] };
static struct {
    _Atomic int state;   // 0 not started, 1 building (background), 2 ready, -1 failed
    id<MTLComputePipelineState> ps[NK_N];
    id<MTLBuffer> t[NT_N], args, luma;
    id<MTL4ArgumentTable> at;
    int pos;             // the next dispatch of the sequence expected (0 = none taken), 13 = 4f41ccf2 next
    int off;             // permanently off after a deviation in the middle of a sequence
    uint64_t w0;         // guest address of the first layer's weights in this sequence
    uint64_t frames, skipped, rs_frames;
    uint64_t wrs;        // GPU address of the resampler's weights this frame (storage buffer register 2 of 4f41ccf2)
} g_nd;
static int ndve_mode(void) { static int m = -1; if (m < 0) m = sw_mode("DM_NDVE_ENGINE", 0); return m; }   // 1, or ab (DM_AB phases)
static int ndve_index(NvShader *cs, int from) { for (int i = from; i < NDVE_N; i++) if (!strncmp(cs->key, k_ndve_keys[i], 16)) return i; return -1; }
// Pipelines and tensors, made once in the background; the game's dispatches run until they are ready
static void ndve_build(void) {
    int z = 0; if (!atomic_compare_exchange_strong(&g_nd.state, &z, 1)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil; id<MTLLibrary> lib = host_lib("ndve_engine", &e);
        if (!lib) { MLOG("[RED] NDVE engine: no library (%s); the game's network runs", e.localizedDescription.UTF8String); g_nd.state = -1; return; }
        for (int k = 0; k < NK_N; k++) {
            id<MTLFunction> f = [lib newFunctionWithName:@(k_ndve_kern[k])];
            g_nd.ps[k] = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
            if (!g_nd.ps[k]) { MLOG("[RED] NDVE engine: no pipeline for %s; the game's network runs", k_ndve_kern[k]); g_nd.state = -1; return; }
        }
        id<MTLCommandBuffer> cb = [[g_dev newCommandQueue] commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        for (int i = 0; i < NT_N; i++) {   // private, zeroed once: the borders stay zero (the kernels write interiors only)
            size_t n = (size_t)(k_ndve_t[i][0] + 2) * (k_ndve_t[i][1] + 2 + 16) * k_ndve_t[i][2] * 2;
            g_nd.t[i] = [g_dev newBufferWithLength:n options:MTLResourceStorageModePrivate];
            [be fillBuffer:g_nd.t[i] range:NSMakeRange(0, n) value:0];
        }
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        for (int i = 0; i < NT_N; i++) res_add(g_nd.t[i]);
        g_nd.luma = [g_dev newBufferWithLength:1920 * 1080 * 2 options:MTLResourceStorageModePrivate]; res_add(g_nd.luma);
        g_nd.args = [g_dev newBufferWithLength:256 * NDVE_NL options:MTLResourceStorageModeShared]; res_add(g_nd.args);
        for (int i = 0; i < NDVE_NL; i++) {
            const ndve_layer *L = &k_ndve_l[i]; const int *T = k_ndve_t[L->in >= 0 ? L->in : NT_X0]; ndve_args a = {960, 540, 0, 0};
            if (L->k != NK_PACK) {
                const int down = L->k == NK_D16 || L->k == NK_D32;   // a.W, a.H: the output's size, the input's for the transposed convolutions
                a.W = (uint32_t)(down ? T[0] / 2 : T[0]); a.H = (uint32_t)(down ? T[1] / 2 : T[1]);
                a.tiles_x = a.W / 8; a.tiles_y = (a.H + k_ndve_ry[L->k] - 1) / k_ndve_ry[L->k];
            }
            memcpy((uint8_t *)g_nd.args.contents + 256 * i, &a, sizeof a);
        }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 6; ad.maxTextureBindCount = 3;
        g_nd.at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        if (!g_nd.at) { MLOG("[RED] NDVE engine: no argument table; the game's network runs"); g_nd.state = -1; return; }
        g_nd.state = 2;
        MLOG("NDVE engine ready (DM_NDVE_ENGINE): the movie network's 12 dispatches after the colour conversion run as the host's kernels");
    } });
}
static int ndve_fail(NvShader *cs, const char *why) {
    if (g_nd.pos > 1) { g_nd.off = 1; MLOG("[RED] NDVE engine: %s at %s after %d of the network's dispatches were skipped; the engine is off from now on (this frame's movie picture is not updated)", why, cs->key, g_nd.pos - 1); }
    else { static int once; if (!once++) MLOG("NDVE engine: %s at %s; the game's network runs (logged once)", why, cs->key); }
    g_nd.pos = 0; return 0;
}
// The image (img) or texture (tex) binding of a stage-5 reflection entry, or nil
static id<MTLTexture> ndve_tex(NvShader *cs, int kind, int reg) {
    for (int i = 0; i < cs->rf.nb; i++) {
        const nvn_rbind *b = &cs->rf.b[i]; if (b->kind != kind || b->reg != reg) continue;
        int tid = kind == NVN_RB_TEX ? (int)(tex_slot_h(S.tex[5], S.img[5], b->reg) & 0xfffff) : b->reg < 32 ? (int)(S.img[5][b->reg] & 0xfffff) : 0;
        id<MTLTexture> t = nil; uint64_t rid = 0; int real = 0;
        os_unfair_lock_lock(&g_rmu);
        if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) { if (kind == NVN_RB_IMG) { t = img_binding(tid, b, &rid, &real); if (!real) t = nil; } else t = tex_by_id(tid); }
        os_unfair_lock_unlock(&g_rmu);
        return t;
    }
    return nil;
}
// The network: 13 kernels in one compute encoder, ordered by barriers; input = the picture 5a7d69b0 reads, output = the image it writes
static void ndve_encode(id<MTLTexture> rgba, id<MTLTexture> out, uint64_t wgpu) {
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL);   // waits for the work before it (queue barrier)
    [ce setArgumentTable:g_nd.at];
    id<MTL4ArgumentTable> at = g_nd.at;
    for (int i = 0; i < NDVE_NL; i++) {
        const ndve_layer *L = &k_ndve_l[i];
        if (i) [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];
        [ce setComputePipelineState:g_nd.ps[L->k]];
        [at setAddress:g_nd.args.gpuAddress + 256 * i atIndex:4];
        const ndve_args *a = (const ndve_args *)((uint8_t *)g_nd.args.contents + 256 * i);
        if (L->k == NK_PACK) {
            [at setTexture:rgba.gpuResourceID atIndex:0]; [at setAddress:g_nd.t[L->out].gpuAddress atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake((960 + 31) / 32, (540 + 3) / 4, 1) threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];
            continue;
        }
        [at setAddress:g_nd.t[L->in].gpuAddress atIndex:0];
        if (L->out >= 0) [at setAddress:g_nd.t[L->out].gpuAddress atIndex:1];
        [at setAddress:wgpu + k_ndve_w[L->layer] atIndex:2]; [at setAddress:wgpu + k_ndve_b[L->layer] atIndex:3];
        if (L->skip >= 0) [at setAddress:g_nd.t[L->skip].gpuAddress atIndex:5];
        if (L->k == NK_OUT) { [at setTexture:rgba.gpuResourceID atIndex:0]; [at setTexture:out.gpuResourceID atIndex:1]; }
        const int staged = L->k == NK_C16 || L->k == NK_C32 || L->k == NK_C64;   // conv3s_*: a threadgroup = 32 x RY outputs
        uint32_t groups = staged ? (a->W + 31) / 32 * a->tiles_y : (a->tiles_x * a->tiles_y + 3) / 4;
        [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    }
    [ce barrierAfterStages:MTLStageDispatch | MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // what follows (4f41ccf2) waits for the image
    enc_c_end(ce);
    args_dirty_all();
}
// The resampler 4f41ccf2: luma pre-pass, then one SIMD-group an 8x8 output block
static int ndve_resample(NvShader *cs) {
    id<MTLTexture> pic = ndve_tex(cs, NVN_RB_TEX, 0), dith = ndve_tex(cs, NVN_RB_TEX, 3), out = ndve_tex(cs, NVN_RB_IMG, 1);
    const int32_t *c = (const int32_t *)S.push[1];
    if (!pic || !dith || !out || pic.width != 1920 || pic.height != 1080 || pic.pixelFormat != MTLPixelFormatRGB10A2Unorm || dith.width != 512 || dith.height != 512 ||
        out.width != 2560 || out.height != 1440 || out.pixelFormat != MTLPixelFormatRGBA8Unorm || out.textureType != MTLTextureType2D ||
        c[0] || c[1] || c[2] != 1919 || c[3] != 1079 || c[4] || c[5] || c[6] != 2559 || c[7] != 1439 || c[20] != 1) {
        static int once; if (!once++) MLOG("NDVE engine: the resampler's bindings or constants are not the title movie's (2560x1440 RGBA8 from 1920x1080, dither on); the game's resampler runs (logged once)");
        return 0;
    }
    __strong id<MTLBuffer> cb = nil; size_t co = arena_alloc(256, &cb); memcpy((uint8_t *)cb.contents + co, S.push[1], 96);
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL);
    id<MTL4ArgumentTable> at = g_nd.at; [ce setArgumentTable:at];
    [ce setComputePipelineState:g_nd.ps[NK_LUMA]]; [at setTexture:pic.gpuResourceID atIndex:0]; [at setAddress:g_nd.luma.gpuAddress atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(60, 135, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];
    [ce setComputePipelineState:g_nd.ps[NK_RS]];
    [at setAddress:g_nd.luma.gpuAddress atIndex:0]; [at setAddress:g_nd.wrs atIndex:2]; [at setAddress:cb.gpuAddress + co atIndex:3];
    [at setTexture:pic.gpuResourceID atIndex:0]; [at setTexture:dith.gpuResourceID atIndex:1]; [at setTexture:out.gpuResourceID atIndex:2];
    [ce dispatchThreadgroups:MTLSizeMake(80, 180, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    enc_c_end(ce);
    args_dirty_all();
    return 1;
}
// 1 when the dispatch was taken (skipped or replaced by the engine), 0 to run it
static int ndve_engine_dispatch(NvShader *cs, const p_dispatch *dp) {
    if (!ndve_mode() || g_nd.off) return 0;
    int i = ndve_index(cs, 0); if (i < 0) return 0;
    if (g_nd.state == 0) ndve_build();
    if (!g_nd.pos && !sw_on(ndve_mode())) return 0;   // an A/B phase changes only between sequences
    if (i == 0) { g_nd.pos = 0; return 0; }   // the colour conversion runs; a new frame's sequence may start
    if (i == NDVE_N - 1) {   // the resampler: by the engine right after the engine's network, else the game's
        int take = g_nd.pos == 13;
        g_nd.pos = 0;
        if (!take) return 0;
        const p_bind_buf *w = &S.ssbo[5][2]; void *wb = NULL; uint64_t wo = 0;
        if (!w->addr || w->size < 0x3000 || !mtl_gpu_addr(w->addr, 0x3000, &wb, &wo, &g_nd.wrs)) { static int once; if (!once++) MLOG("NDVE engine: the resampler's weights are not bound; the game's resampler runs"); return 0; }
        if (!ndve_resample(cs)) return 0;
        if (!g_nd.rs_frames++) MLOG("NDVE engine: first movie frame resampled by the engine");
        return 1;
    }
    // dispatch i of the sequence: the keys repeat (a8b4, e6be, 0a56 twice), so position decides
    if (g_nd.pos == 0) { if (i != 1) return 0; if (g_nd.state != 2) return 0; }
    else if (ndve_index(cs, g_nd.pos) != g_nd.pos) return ndve_fail(cs, "a dispatch out of the network's order");
    const int p = g_nd.pos ? g_nd.pos : 1;
    const p_bind_buf *w = &S.ssbo[5][2], *b = &S.ssbo[5][3];
    if (p == 1) g_nd.w0 = w->addr;
    if (w->addr != g_nd.w0 + k_ndve_w[p] || b->addr != g_nd.w0 + k_ndve_b[p] || !w->size || !b->size) return ndve_fail(cs, "weights or biases not where the network's table has them");
    if (p < 12) { g_nd.pos = p + 1; g_nd.skipped++; return 1; }
    // 5a7d69b0: the whole network now
    id<MTLTexture> rgba = ndve_tex(cs, NVN_RB_TEX, 4), out = ndve_tex(cs, NVN_RB_IMG, 0);
    void *wb = NULL; uint64_t wo = 0, wgpu = 0;
    if (!rgba || !out || rgba.width != 1920 || rgba.height != 1080 || out.width != 1920 || out.height != 1080 || rgba.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        out.pixelFormat != MTLPixelFormatRGB10A2Unorm || rgba.textureType != MTLTextureType2D || out.textureType != MTLTextureType2D)
        return ndve_fail(cs, "the picture or the image is not the 1920x1080 RGBA8 / RGB10A2 pair");
    if (!mtl_gpu_addr(g_nd.w0, NDVE_WBYTES, &wb, &wo, &wgpu)) return ndve_fail(cs, "the weight table is in no memory pool");
    ndve_encode(rgba, out, wgpu);
    g_nd.pos = 13;   // the resampler next
    if (!g_nd.frames++) MLOG("NDVE engine: first movie frame by the engine (%llu dispatches skipped)", (unsigned long long)g_nd.skipped);
    (void)dp; return 1;
}
