// nvn_mtl.m, part: shot
// Presentation helpers and diagnostics: the swapchain blit state, screenshots, texture dumps and lists, presented-time bookkeeping.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// ---------- Present: draw the swapchain texture to the CAMetalLayer; optional screenshot ----------
static id<MTLRenderPipelineState> g_blit_pso; static MTLPixelFormat g_blit_fmt;
static id<MTLSamplerState> g_blit_samp;
static uint64_t g_present_n;
uint64_t mtl_present_count(void) { return g_present_n; }   // Actual present count (heat attribution DM_HEAT computes the present frame rate)
void mtl_res_snapshot(mtl_res_count *out) {
    memset(out, 0, sizeof *out);
    out->pso = g_pso.n;
    out->cps = g_cps.n;
    out->dss = g_dss.n;
    out->shaders = (uint32_t)(g_shcache ? g_shcache.count : 0);
    os_unfair_lock_lock(&g_c1_lock);
    out->c1_bufs = (uint32_t)(g_c1_banks.count + g_c1_exclusive); out->c1_alloc = g_c1_alloc_bytes; out->c1_data = g_c1_data_bytes;
    os_unfair_lock_unlock(&g_c1_lock);
    pthread_mutex_lock(&g_smu);
    out->live_tex = g_by_serial.n;
    pthread_mutex_unlock(&g_smu);
    pthread_mutex_lock(&g_mu);
    out->pools = (uint32_t)g_npool;
    out->pool_adds = g_pool_adds;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_rmu);
    out->view_slots = (uint32_t)g_ntreg;
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].view) out->view_nonnull++;
    for (int i = 0; i < g_nsreg; i++) if (g_sview[i]) out->sview_nonnull++;
    pthread_mutex_unlock(&g_rmu);
}
// Resource counts (read by long-run stability L3): the host calls it per game tick / event (every 250 ticks, loading screen dismissed, exit), not sampled by wall clock / present count.
// Line format: [nvn] resource counts tick <tick number> <trigger>: <name> <value> ... (names contain no spaces, single-space separated); new fields are appended to the end of this line
void mtl_res_counts(uint32_t tick, const char *why) {
    mtl_res_count c;
    mtl_res_snapshot(&c);
    int dl, du; uint64_t da; nvn_dirty_slot_stats(&dl, &du, &da);
    LOG("[nvn] resource counts tick %u %s: render_pipelines %u compute_pipelines %u shaders %u depth_stencil_states %u views_nonnull %u view_slots %u live_textures %u pools_live %u pools_total %llu sampler_views_nonnull %u dirty_ranges_live %d dirty_ranges_total %llu c1_buffers %u c1_bytes %llu c1_used %llu",
        tick, why, c.pso, c.cps, c.shaders, c.dss, c.view_nonnull, c.view_slots, c.live_tex,
        c.pools, (unsigned long long)c.pool_adds, c.sview_nonnull, dl, (unsigned long long)da, c.c1_bufs, (unsigned long long)c.c1_alloc, (unsigned long long)c.c1_data);
}
// Screenshot on request (used by DM_NVN_SHOT_UI): dm_gfx_shot_at(k, path) = take a screenshot at the kth frame after the current present frame; due requests are checked at every frame's present
#define SHOTQ_MAX 8
static struct { uint64_t frame; char path[1024]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]);
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i);
// Game picture needed by the whole-window screenshot (front end window_shot; no longer does drawViewHierarchy on a Metal layer with the display link attached): at the next present, the present thread draws the same frame
// with the same encoding as the present (present_encode) into an offscreen texture of the same size as the swapchain, and after the GPU finishes hands out a CGImage in the completion callback (the callback does not block the present). Guarded by g_shotq_mu
static void (^g_grab_cb)(CGImageRef img);
void mtl_grab_next_present(void (^cb)(CGImageRef img)) { pthread_mutex_lock(&g_shotq_mu); g_grab_cb = [cb copy]; pthread_mutex_unlock(&g_shotq_mu); }
static void grab_present(id<MTLTexture> src, const int crop[4], CGSize sz) {
    pthread_mutex_lock(&g_shotq_mu); void (^cb)(CGImageRef) = g_grab_cb; g_grab_cb = nil; pthread_mutex_unlock(&g_shotq_mu);
    if (!cb) return;
    size_t w = (size_t)sz.width, h = (size_t)sz.height, row = w * 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = w && h ? [g_dev newTextureWithDescriptor:d] : nil; id<MTLBuffer> buf = out ? [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared] : nil;
    id<MTLCommandBuffer> cb2 = buf ? q_cb() : nil;
    if (!cb2 || !present_encode(cb2, src, out, crop)) { MLOG("[RED] full-window screenshot: offscreen encoding of the game image failed (%zux%zu)", w, h); cb(NULL); return; }
    id<MTLBlitCommandEncoder> b = [cb2 blitCommandEncoder];
    [b copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding];
    [cb2 addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL; cb(img);
        CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs); }];
    [cb2 commit];
}
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = g_present_n + (uint64_t)(k > 0 ? k : 0); strlcpy(g_shotq[g_nshotq].path, path, sizeof g_shotq[0].path); g_nshotq++; }
    else MLOG("[RED] more than %d screenshot requests; dropping %s", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
// DM_NVN_SHOT_OUT=WIDTHxHEIGHT (for picture quality metrics): the present-frame screenshot instead captures the result "upscaled to this resolution by present_encode", i.e. the picture the player sees on screen; screen=0 (texture dump) is unaffected
static void shot(id<MTLTexture> src, const char *path, int screen, const int crop[4]) {
    static int out_w = -1, out_h;
    if (out_w < 0) {
        out_w = 0;
        const char *e = getenv("DM_NVN_SHOT_OUT"); int w = 0, h = 0; char sep = 0, extra = 0;
        if (e && sscanf(e, "%d%c%d%c", &w, &sep, &h, &extra) == 3 && sep == 'x' && w > 0 && h > 0) { out_w = w; out_h = h; }
    }
    id<MTLTexture> read = src;
    if (out_w && screen) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:out_w height:out_h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
        if (!out) { MLOG("[RED] cannot create the screenshot output texture %dx%d", out_w, out_h); return; }
        id<MTLCommandBuffer> cb = q_cb();
        if (!present_encode(cb, src, out, crop)) return;
        [cb commit]; [cb waitUntilCompleted];
        read = out;
    }
    if (read.pixelFormat != MTLPixelFormatRGBA8Unorm && read.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        read.pixelFormat != MTLPixelFormatBGRA8Unorm && read.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) { MLOG("[RED] screenshot does not support format %lu", (unsigned long)read.pixelFormat); return; }
    size_t w = read.width, h = read.height, row = w * 4;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:read sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (screen && read == src && atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) {   // lower-left origin: the window image is stored bottom-up (see g_origin_ll)
        uint8_t *p = buf.contents, *tmp = malloc(row);
        if (tmp) { for (size_t y = 0; y < h / 2; y++) { memcpy(tmp, p + y * row, row); memcpy(p + y * row, p + (h - 1 - y) * row, row); memcpy(p + (h - 1 - y) * row, tmp, row); } free(tmp); } }
    int bgra = read.pixelFormat == MTLPixelFormatBGRA8Unorm || read.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGBitmapInfo bi = bgra ? (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little) : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    // If any step yields null (directory does not exist / not writable, bitmap context cannot be created), report a failure and return; previously it went straight to CFRelease(NULL) and crashed in CFRelease.cold (hit during a 1952-wide TEXDUMP)
    int ok = 0;
    if (!ctx || !img || !dst) MLOG("[RED] screenshot %s failed: %s", path, !ctx ? "cannot create the bitmap context" : !img ? "cannot get the image" : "cannot write (directory missing or not writable)");
    else { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); }
    if (dst) CFRelease(dst); if (url) CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    MLOG("screenshot %zux%zu format %lu → %s %s", w, h, (unsigned long)read.pixelFormat, path, ok ? "ok" : "failed");
    if (g_defer_ab >= 0 && getenv("DM_NVN_DEFER_AB") && !strcmp(getenv("DM_NVN_DEFER_AB"), "1") && ok) { static int shots; if (++shots == 2) { g_defer_ab_on = 1; MLOG("DM_NVN_DEFER_AB: second screenshot written; deferred pass merging on"); } }
    if (g_udc_ab >= 0 && getenv("DM_NVN_UDC_AB") && !strcmp(getenv("DM_NVN_UDC_AB"), "1") && ok) {
        static int shots;
        if (++shots == 2) { g_udc_ab_on = 1; MLOG("DM_NVN_UDC_AB: second screenshot written; DM_NVN_UNWRITTEN_DC on"); }
    }
}
// ---------- DM_NVN_TEXDUMP=frame number:path (%d in the path is replaced with "index*100+mip level"): save each color render target used by that frame as an image ----------
// Diagnostic: before the first render pass of the next frame is opened, wait for the GPU to finish, then draw each one (the mip level used by that frame) by full-screen sampling into RGBA8 and save as PNG (float formats are clamped to [0,1])
static long g_td_frame = -2; static const char *g_td_path; static NSMutableArray *g_td_list; static int g_td_done;
static void td_dump(void);
// DM_NVN_TEXDUMP=size=WxH:path instead picks the frame by content: the first frame that opens a render pass whose first color attachment level is WxH (for one-off work whose frame differs between runs)
static unsigned g_td_w, g_td_h;
void td_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (g_td_frame == -2) { const char *e = getenv("DM_NVN_TEXDUMP"), *c = e ? strchr(e, ':') : NULL; g_td_frame = c ? atol(e) : -1; g_td_path = c ? c + 1 : NULL; g_td_list = [NSMutableArray array];
        if (c && !strncmp(e, "size=", 5) && sscanf(e + 5, "%ux%u", &g_td_w, &g_td_h) == 2) g_td_frame = LONG_MAX; }
    if (g_td_frame < 0 || g_td_done) return;
    long f = (long)atomic_load(&g_cur_frame);
    if (g_td_frame == LONG_MAX) { id<MTLTexture> t0 = nc ? pd.colorAttachments[0].texture : nil; NSUInteger lv = pd.colorAttachments[0].level;
        if (t0 && MAX(t0.width >> lv, 1ul) == g_td_w && MAX(t0.height >> lv, 1ul) == g_td_h) { g_td_frame = f; MLOG("[texdump] first %ux%u render target at frame %ld: dumping this frame", g_td_w, g_td_h, f); } }
    if (f > g_td_frame) { td_dump(); return; }
    if (f != g_td_frame) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) {
        id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        uint64_t ser = i < (NSUInteger)R.rt.n && R.rt.color[i] ? R.rt.color[i]->serial : 0;
        NSUInteger lv = pd.colorAttachments[i].level, sl = t.textureType == MTLTextureType2DArray ? pd.colorAttachments[i].slice : 0;
        BOOL seen = NO; for (NSArray *e in g_td_list) if (e.count < 5 && [e[1] unsignedLongLongValue] == ser && [e[2] unsignedIntegerValue] == lv && [e[3] unsignedIntegerValue] == sl) seen = YES;
        if (!seen) [g_td_list addObject:@[t, @(ser), @(lv), @(sl)]];
    }
}
// The next frame's targets, on request (the Mac's DM_CTL "texdump <path>", hle_hid.c): the path is kept (strdup), the frame number is the one after the current
// The Mac's DM_CTL "texraw <serial> <path>" (hle_hid.c): level 0 / slice 0 of the host texture behind a serial, raw, after the GPU finishes (header line: width height Metal format bytes per row);
// for a texture that is no render target (a default texture, a lookup table) whose content a shader depends on
void dm_gfx_texraw(uint64_t serial, const char *path) {
    __unsafe_unretained id o = tex_get(serial);
    if (!o || ![o conformsToProtocol:@protocol(MTLTexture)]) { MLOG("[texraw] ser=%llu: no host texture", (unsigned long long)serial); return; }
    id<MTLTexture> t = o; mtl_finish_gpu();
    if (t.storageMode == MTLStorageModeMemoryless || t.pixelFormat == MTLPixelFormatInvalid) return;
    NSUInteger w = t.width, h = t.height, row = w * 16, sz = row * h;   // 16 bytes a texel: a multiple of every uncompressed texel size
    id<MTLBuffer> buf = [g_dev newBufferWithLength:sz options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    MTLBlitOption bo = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:sz options:bo];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)t.pixelFormat, (unsigned long)row); fwrite(buf.contents, 1, sz, f); fclose(f); }
    const uint8_t *p = buf.contents;
    MLOG("[texraw] ser=%llu %lux%lu format %lu type %lu → %s; first bytes %02x %02x %02x %02x %02x %02x %02x %02x", (unsigned long long)serial, (unsigned long)w, (unsigned long)h, (unsigned long)t.pixelFormat, (unsigned long)t.textureType, path, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}
// The Mac's DM_CTL "texlist <NVN target>" (hle_hid.c): the registered textures of that target (2 = 3D), with their storage (guest address, size) and block-linear layout, for "memdump"
void dm_gfx_texlist(int target) {
    pthread_mutex_lock(&g_rmu);
    for (int i = 1; i < g_ntreg; i++) { const tex_t *t = &g_treg[i].t; if (!g_treg[i].ok || t->d.target != target) continue;
        MLOG("[texlist] tid=%d ser=%llu target %d %ux%ux%u format %#x levels %u flags %#x storage %p size %#llx block height/depth log2 %u/%u", i, (unsigned long long)t->serial, t->d.target,
             t->d.width, t->d.height, t->d.depth, t->d.format, t->d.levels, t->d.flags, (void *)t->mem, (unsigned long long)t->L.size, t->L.tileH, t->L.tileD); }
    pthread_mutex_unlock(&g_rmu);
}
void dm_gfx_texdump_next(const char *path) {
    if (g_td_frame == -2) td_note(nil, 0);   // read DM_NVN_TEXDUMP first, so it does not override the request
    g_td_path = strdup(path); g_td_list = [NSMutableArray array]; g_td_done = 0; g_td_frame = (long)atomic_load(&g_cur_frame) + 1;
}
static void td_dump(void) {
    g_td_done = 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_finish_gpu();
    static id<MTLRenderPipelineState> ps; static id<MTLSamplerState> sp;
    if (!ps) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { float4 c = t.sample(s, in.uv); return float4(c.rgb, 1); }\n";
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sp = [g_dev newSamplerStateWithDescriptor:sd];
        if (!ps) { MLOG("[RED] TEXDUMP pipeline failed %s", e.localizedDescription.UTF8String); return; }
    }
    MLOG("[texdump] frame %ld: %lu color targets", g_td_frame, (unsigned long)g_td_list.count);
    // DM_NVN_TEXRAW=index,...: save raw bytes by texture index (including 3D; file: %d in the path is replaced with the index, suffix .raw, first-line text header: width height depth format bytes-per-row)
    for (const char *q = getenv("DM_NVN_TEXRAW"); q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
        if (!strncmp(q, "info", 4)) {   // DM_NVN_TEXRAW=info: the textures remembered by DM_NVN_TEXINFO
            static char buf[256]; size_t o = 0; for (int i = 0; i < g_texinfo_n; i++) o += snprintf(buf + o, sizeof buf - o, "%s%llu", i ? "," : "", (unsigned long long)g_texinfo_ser[i]);
            if (o) { static char env[300]; snprintf(env, sizeof env, "%s", buf); setenv("DM_NVN_TEXRAW", env, 1); q = getenv("DM_NVN_TEXRAW"); MLOG("[texraw] info → %s", q); } else continue;
        }
        uint64_t ser = (uint64_t)atoll(q); id o = serial_get(ser);
        id<MTLTexture> t = [o isKindOfClass:[NvPkg class]] ? ((NvPkg *)o)->tex : o;
        if (!t) { MLOG("[texraw] ser=%llu not in the serial table", ser); continue; }
        int bw, bh; NSUInteger w = t.width, h = t.height, dz = t.depth;
        size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : 4; (void)bw; (void)bh;
        size_t row = w * bpp, img = row * h;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:img * dz options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        MTLBlitOption bo = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;   // depth-stencil: the 32-bit float depth plane only
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, dz) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:img options:bo];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)ser); strlcat(path, ".raw", sizeof path);
        FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu %zu\n", (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, row); fwrite(buf.contents, 1, img * dz, f); fclose(f); }
        MLOG("[texraw] ser=%llu %lux%lux%lu format %lu → %s", ser, (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, path);
    }
    for (NSArray *e in g_td_list) {
        id<MTLTexture> t = e[0]; uint64_t ser = [e[1] unsignedLongLongValue]; NSUInteger lv = [e[2] unsignedIntegerValue], sl = [e[3] unsignedIntegerValue];
        // 2D arrays are saved by the attached layer (file index ser*100 + layer*10 + level; layer 0 is named like 2D)
        if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) { MLOG("[texdump] ser=%llu type %lu skipped", ser, (unsigned long)t.textureType); continue; }
        if (getenv("DM_NVN_SNAPRAW")) {   // Also save the raw bytes (converting an HDR target to an 8-bit image truncates; first-line text header: width height format bytes-per-row)
            NSUInteger rw = MAX(t.width >> lv, 1u), rh = MAX(t.height >> lv, 1u);
            size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR16Float || t.pixelFormat == MTLPixelFormatR16Unorm ? 2 : 4;
            id<MTLBuffer> rb = [g_dev newBufferWithLength:rw * rh * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> rc = q_cb(); id<MTLBlitCommandEncoder> bb = [rc blitCommandEncoder];
            [bb copyFromTexture:t sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rw, rh, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:rw * bpp destinationBytesPerImage:rw * rh * bpp];
            [bb endEncoding]; [rc commit]; [rc waitUntilCompleted];
            char rp_[1024]; snprintf(rp_, sizeof rp_, g_td_path, (int)(ser * 100 + lv));
            if (e.count > 4) { char tail[64]; snprintf(tail, sizeof tail, "_s%03d", [e[4] intValue]); strlcat(rp_, tail, sizeof rp_); }   // TEXSNAP copies of one target: one file per pass
            strlcat(rp_, ".raw", sizeof rp_);
            FILE *f = fopen(rp_, "wb"); if (f) { fprintf(f, "%lu %lu %lu %zu\n", (unsigned long)rw, (unsigned long)rh, (unsigned long)t.pixelFormat, rw * bpp); fwrite(rb.contents, 1, rw * rh * bpp, f); fclose(f); }
        }
        id<MTLTexture> v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(lv, 1) slices:NSMakeRange(sl, 1)];
        NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
        id<MTLTexture> o = [g_dev newTextureWithDescriptor:od];
        id<MTLCommandBuffer> cb = q_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setFragmentTexture:v atIndex:0]; [en setFragmentSamplerState:sp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)(ser * 100 + sl * 10 + lv));
        if (e.count > 4) { snprintf(path, sizeof path, g_td_path, 0); char *dot = strrchr(path, '.'); char ext[16] = ""; if (dot) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
            char tail[64]; snprintf(tail, sizeof tail, "_s%03d_%llu_%lu%s", [e[4] intValue], (unsigned long long)ser, (unsigned long)lv, ext); strlcat(path, tail, sizeof path); }   // TEXSNAP copy
        MLOG("[texdump] ser=%llu lv=%lu layer %lu format %lu %lux%lu", ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)t.pixelFormat, (unsigned long)w, (unsigned long)h);
        shot(o, path, 0, NULL);
    }
    [g_td_list removeAllObjects];
}
// DM_NVN_TEXSNAP=in-frame index,... (together with the frame number and path of DM_NVN_TEXDUMP): as soon as these render passes end, copy their color attachments and save them together with the end-of-frame dump
// (the same target is often cleared and reused within a frame, so the end-of-frame state cannot reflect the output of intermediate passes); saved as <TEXDUMP path with %d = 0>_s<in-frame index>_<serial>_<level>.png
static int g_ts_idx[64], g_ts_n = -1, g_ts_on; static uint64_t g_ts_first, g_ts_curf = ~0ull;
static __strong id<MTLTexture> g_ts_att[8]; static uint64_t g_ts_ser[8]; static NSUInteger g_ts_lv[8]; static int g_ts_na;
void ts_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (g_ts_n < 0) { g_ts_n = 0; const char *e = getenv("DM_NVN_TEXSNAP");
        for (const char *q = e; q && *q && g_ts_n < 64; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) g_ts_idx[g_ts_n++] = atoi(q); }
    g_ts_on = 0; if (!g_ts_n || g_td_frame < 0 || g_td_done) return;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_ts_curf) { g_ts_curf = f; g_ts_first = g_stat_pass; }
    if ((long)f != g_td_frame) return;
    int idx = (int)(g_stat_pass - g_ts_first); for (int i = 0; i < g_ts_n; i++) if (g_ts_idx[i] == idx) g_ts_on = idx + 1;
    g_ts_na = 0; if (!g_ts_on) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ts_att[g_ts_na] = t; g_ts_ser[g_ts_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ts_lv[g_ts_na] = pd.colorAttachments[i].level; g_ts_na++; }
}
void ts_after(void) {
    if (!g_ts_on) return;
    // The copy goes into the replay stream (Metal 4 copy encoder, after the pass that just ended). A command buffer of the classic queue, committed here, ran before the
    // Metal 4 command buffer holding the pass was even committed, so every snapshot showed the attachments as they were before the pass (found on Tears of the Kingdom)
    for (int i = 0; i < g_ts_na; i++) {
        id<MTLTexture> t = g_ts_att[i]; if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) continue;   // 2D arrays take slice 0
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:t.mipmapLevelCount > 1];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; d.mipmapLevelCount = t.mipmapLevelCount;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; res_add(c);
        NSUInteger lv = g_ts_lv[i], w = MAX((NSUInteger)1, t.width >> lv), h = MAX((NSUInteger)1, t.height >> lv);
        enc_b_copy_texture(blit(), t, 0, lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), c, 0, lv, MTLOriginMake(0, 0, 0));
        [g_td_list addObject:@[c, @(g_ts_ser[i]), @(g_ts_lv[i]), @0, @(g_ts_on - 1)]];   // the end-of-frame fields (texture, serial, level, layer) plus the in-frame pass index: saved as <path with %d = 0>_s<index>_<serial>_<level>.png
    }
    g_ts_on = 0;
}
// Ring of actual presentation times: present sequence number -> presentedTime (seconds; -1 = dropped by the system, not presented)
#define PRING 1024
typedef struct { uint64_t n; double t; } pres_ent;
static pres_ent g_pres[PRING];
// Get the actual presentation time of the nth present (returns 0 if not ready, -1 if dropped)
double mtl_presented_time(uint64_t n) { pres_ent e = g_pres[n % PRING]; return e.n == n ? e.t : 0; }
static _Atomic uint64_t g_drawable_ns;   // Accumulated wall clock waiting for nextDrawable in present (timing instrumentation)
uint64_t mtl_drawable_ns_take(void) { return atomic_exchange(&g_drawable_ns, 0); }
// Maximum refresh rate (Hz) of the screen when there is a window; returns 0 without a window. Mac uses CoreGraphics display modes (thread-safe C interface; the present configuration is read on the game thread)
int mtl_display_hz(void) {
    if (!front_metal_layer()) return 0;
#if TARGET_OS_OSX
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    return (int)(hz + 0.5);
#else
    Class c = NSClassFromString(@"UIScreen"); id s = c ? [c valueForKey:@"mainScreen"] : nil;
    return s ? [[s valueForKey:@"maximumFramesPerSecond"] intValue] : 0;
#endif
}
static _Atomic uint64_t g_present_stall_ns;   // For unit tests: an extra pause before each present on the present thread (simulating a drawable that stays unavailable)
static CAMetalLayer *g_present_layer_resident;
void mtl_present_test_stall(uint64_t ns) { g_present_stall_ns = ns; }
