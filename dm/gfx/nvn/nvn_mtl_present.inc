// nvn_mtl.m, part: present
// Present path: the blit pipeline and encoders, the display link, the present thread, external textures, snapshots, present queueing and waiting, and the small stats accessors.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
static void present_pipeline(MTLPixelFormat format) {
    if (!g_blit_pso || g_blit_fmt != format) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "struct Crop { float4 rect; float4 edge; };\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        if (!lib) MDIE("present shader compile failed %s", e.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = format;
        g_blit_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!g_blit_pso) MDIE("present pipeline failed %s", e.localizedDescription.UTF8String);
        g_blit_fmt = format;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.supportArgumentBuffers = YES;
        g_blit_samp = [g_dev newSamplerStateWithDescriptor:sd];
    }
}
// Present samples only the valid source rectangle declared by NVN; kept separate from the raw swapchain readback screenshot. Edges are clamped to the texel centers of the valid area, so bilinear does not read the black texels outside.
// Present samples the raw bytes stored in the swapchain texture (the Switch's scan-out sends the stored bytes as they are): an sRGB source is replaced by a non-sRGB view of the same storage,
// otherwise sampling decodes to linear first, the BGRA8Unorm layer does not encode again, and the whole picture comes out dark. Other formats are returned as they are (Mario's swapchain is RGBA8 and does not come here).
// View cache of 4 entries: swapchain textures live long and are few; a view holds its parent, so an equal parent pointer means the same object. A replaced view is released after the submissions that used it complete (Metal 4 does not retain resources)
static id<MTLTexture> present_raw_src(id<MTLTexture> src) {
    MTLPixelFormat pf = src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
    if (pf == MTLPixelFormatInvalid) return src;
    static __strong id<MTLTexture> views[4]; static int next; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    id<MTLTexture> v = nil;
    for (int i = 0; i < 4 && !v; i++) if (views[i] && views[i].parentTexture == src) v = views[i];
    if (!v && (v = [src newTextureViewWithPixelFormat:pf])) { if (views[next]) defer_obj_unref(views[next]); views[next] = v; next = (next + 1) & 3; }
    pthread_mutex_unlock(&mu);
    if (!v) { RED_ONCE(69, "present: cannot create a non-sRGB view of the sRGB swapchain texture (format %lu); presenting the decoded values", (unsigned long)src.pixelFormat); return src; }
    return v;
}
// The rectangle of the canvas (dw x dh) the picture (cw x ch) is drawn into: the picture's aspect ratio kept and centred, black bars in the rest (no stretching when the screen's ratio
// differs from the game's, e.g. iPhone about 19.5:9 against the game's 16:9); when the ratios differ by under 1 % (the remainder from a game width that is a multiple of 8 in the
// full-screen setup, e.g. 1952x900 against 2868x1320 differs by 0.17 %) the whole canvas, leaving no border of a few pixels
static MTLViewport present_viewport(double cw, double ch, double dw, double dh) {
    double s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / (cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    return vp;
}
// The valid source rectangle of a present (x, y, width, height; the crop of nvnWindowSetCrop, zero = the whole texture); 0 when it lies outside the texture
static int present_crop_rect(id<MTLTexture> src, const int crop[4], NSUInteger r[4]) {
    r[0] = 0; r[1] = 0; r[2] = src.width; r[3] = src.height;
    if (!crop || !(crop[2] || crop[3])) return 1;
    if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
        (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
        MLOG("[RED] present crop out of bounds: %d,%d %dx%d / source texture %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
    }
    r[0] = crop[0]; r[1] = crop[1]; r[2] = crop[2]; r[3] = crop[3]; return 1;
}
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]) {
    NSUInteger r[4]; if (!present_crop_rect(src, crop, r)) return 0;
    NSUInteger cx = r[0], cy = r[1], cw = r[2], ch = r[3];
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("present crop: source texture %lux%lu, valid %lu,%lu %lux%lu → target %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
    present_pipeline(dst.pixelFormat);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    double dw = dst.width, dh = dst.height;
    MTLViewport vp = present_viewport(cw, ch, dw, dh);
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    static double last_vp[4];   // Log one line when the viewport changes (used by the real-device screen-level acceptance screen_check.py to check the aspect ratio)
    if (memcmp(last_vp, &vp, sizeof last_vp)) { memcpy(last_vp, &vp, sizeof last_vp);
        MLOG("present viewport %.0f,%.0f %.0fx%.0f / canvas %.0fx%.0f (game image %lux%lu)", vp.originX, vp.originY, vp.width, vp.height, dw, dh, (unsigned long)src.width, (unsigned long)src.height); }
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setViewport:vp];
    [en setRenderPipelineState:g_blit_pso]; [en setFragmentTexture:present_raw_src(src) atIndex:0]; [en setFragmentSamplerState:g_blit_samp atIndex:0];
    float uv[8] = {(float)cx / src.width, (float)cy / src.height, (float)cw / src.width, (float)ch / src.height,
                   (cx + 0.5f) / src.width, (cy + 0.5f) / src.height, (cx + cw - 0.5f) / src.width, (cy + ch - 0.5f) / src.height};
    if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    [en setFragmentBytes:uv length:sizeof uv atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    return 1;
}
// ---------- Upscaling in the present (DM_PRESENT_UPSCALE) ----------
// Opt-in spatial scaling of the valid content to its letterboxed viewport. Copy the crop to an origin-aligned texture, scale in perceptual mode (present_raw_src supplies gamma
// bytes), then draw 1:1 with the usual origin and output format. Queue barriers serialize both the frame's rendering and reuse of the scaler's textures across presents.
// Replacing those textures waits for earlier presents; throttle replacement during resizing, and keep standard scaling for refused sizes. Offscreen captures own separate state.
#define FX_REMAKE_PRESENTS 30
typedef struct { id<MTL4FXSpatialScaler> sc; id<MTLTexture> in, out; uint64_t made_at; int made; NSUInteger refused[6]; } fx_state;
static fx_state g_fx_present;   // only the present thread encodes or replaces the scaler
static id<MTL4Compiler> g_fx_comp;
static int g_fx_mode = -1;   // 1 = metalfx, 0 = off
static int fx_mode(void) {
    if (g_fx_mode >= 0) return g_fx_mode;
    const char *e = getenv("DM_PRESENT_UPSCALE"); int v = 0;
    if (e && (!strcmp(e, "metalfx") || !strcmp(e, "1"))) v = 1;
    else if (e && (!strcmp(e, "off") || !strcmp(e, "0"))) v = 0;
    else if (e && *e) MLOG("[RED] DM_PRESENT_UPSCALE=%s: metalfx or off; keeping %s", e, v ? "metalfx" : "off");
    if (v && !(g_q4 && [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev])) { MLOG("present upscaling: this GPU has no Metal 4 MetalFX spatial scaler; the present scales bilinearly"); v = 0; }
    MLOG("present upscaling (DM_PRESENT_UPSCALE): %s", v ? "metalfx, MetalFX's spatial scaler where the picture is shown larger than the game renders it" : "off, bilinear");
    return g_fx_mode = v;
}
// MetalFX scales the content (cw x ch) to its rectangle on the canvas (vp) when the rectangle is at least as large in both directions and larger in one
static int fx_wanted(MTLViewport vp, NSUInteger cw, NSUInteger ch) {
    NSUInteger w = (NSUInteger)vp.width, h = (NSUInteger)vp.height;
    return cw && ch && w >= cw && h >= ch && (w > cw || h > ch);
}
// Encodes the upscale of the rectangle r of raw into cb (see above); returns the scaled picture, which the present pass draws 1:1 into vp, or nil (the bilinear pass).
// remake_wait: fx is the present thread's, whose textures presents still on the GPU may read: a new scaler waits for them and for FX_REMAKE_PRESENTS presents since the last
static id<MTLTexture> fx_encode(fx_state *fx, id<MTL4CommandBuffer> cb, id<MTLTexture> raw, const NSUInteger r[4], MTLViewport vp, MTLPixelFormat out_pf, int remake_wait) {
    NSUInteger cw = r[2], ch = r[3], ow = (NSUInteger)vp.width, oh = (NSUInteger)vp.height;
    if (fx_mode() != 1 || !fx_wanted(vp, cw, ch)) return nil;
    if (!fx->sc || fx->in.width != cw || fx->in.height != ch || fx->in.pixelFormat != raw.pixelFormat || fx->out.width != ow || fx->out.height != oh || fx->out.pixelFormat != out_pf) {
        NSUInteger key[6] = {cw, ch, raw.pixelFormat, ow, oh, out_pf};
        if (!memcmp(key, fx->refused, sizeof key)) return nil;
        if (remake_wait && fx->made && g_present_n < fx->made_at + FX_REMAKE_PRESENTS) return nil;
        if (remake_wait) { pthread_mutex_lock(&g_pq_mu); while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu); }
        res_remove(fx->in); res_remove(fx->out); fx->sc = nil; fx->in = fx->out = nil;
        uint64_t t0 = now_ns(); NSError *e = nil;
        if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.colorTextureFormat = raw.pixelFormat; d.outputTextureFormat = out_pf; d.inputWidth = cw; d.inputHeight = ch; d.outputWidth = ow; d.outputHeight = oh;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        id<MTL4FXSpatialScaler> sc = g_fx_comp ? [d newSpatialScalerWithDevice:g_dev compiler:g_fx_comp] : nil;
        fx->made_at = g_present_n; fx->made++;
        if (!sc) { memcpy(fx->refused, key, sizeof key);
            MLOG("present upscaling: MetalFX makes no spatial scaler for %lux%lu format %lu → %lux%lu format %lu%s; this size is scaled bilinearly", (unsigned long)cw, (unsigned long)ch, (unsigned long)raw.pixelFormat,
                 (unsigned long)ow, (unsigned long)oh, (unsigned long)out_pf, g_fx_comp ? "" : " (no Metal 4 compiler)"); return nil; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:raw.pixelFormat width:cw height:ch mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = sc.colorTextureUsage | MTLTextureUsageShaderRead;
        id<MTLTexture> in = [g_dev newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:out_pf width:ow height:oh mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = sc.outputTextureUsage | MTLTextureUsageShaderRead;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:td];
        if (!in || !out) { memcpy(fx->refused, key, sizeof key); MLOG("[RED] present upscaling: cannot create the scaler's textures (%lux%lu, %lux%lu)", (unsigned long)cw, (unsigned long)ch, (unsigned long)ow, (unsigned long)oh); return nil; }
        in.label = @"present upscale input"; out.label = @"present upscale output";
        res_add(in); res_add(out); res_commit();
        fx->sc = sc; fx->in = in; fx->out = out;
        MLOG("present upscaling: MetalFX spatial scaler %lux%lu → %lux%lu (format %lu → %lu, scaler #%d, made in %.1f ms)", (unsigned long)cw, (unsigned long)ch, (unsigned long)ow, (unsigned long)oh,
             (unsigned long)raw.pixelFormat, (unsigned long)out_pf, fx->made, (now_ns() - t0) / 1e6);
    }
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];   // after the frame's work
    [ce copyFromTexture:raw sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(r[0], r[1], 0) sourceSize:MTLSizeMake(cw, ch, 1) toTexture:fx->in destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // the scaler's encoders come after the copy
    [ce endEncoding];
    fx->sc.colorTexture = fx->in; fx->sc.inputContentWidth = cw; fx->sc.inputContentHeight = ch; fx->sc.outputTexture = fx->out;
    [fx->sc encodeToCommandBuffer:cb];
    return fx->out;
}
// The present pass: src's valid rectangle drawn into dst (the drawable, or a screenshot's texture) as present_encode draws it, through MetalFX first where fx_encode takes it
// (fx: the scaler state to use, nil = bilinear). uvb: the pass's crop constants (one buffer per present allocator, written by the CPU)
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], id<MTLBuffer> uvb, fx_state *fx) {
    NSUInteger r[4]; if (!present_crop_rect(src, crop, r)) return 0;
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height;
    MTLViewport vp = present_viewport(r[2], r[3], dw, dh);
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    id<MTLTexture> tex = present_raw_src(src), up = fx ? fx_encode(fx, cb, tex, r, vp, dst.pixelFormat, fx == &g_fx_present) : nil;
    if (up) { tex = up; r[0] = r[1] = 0; r[2] = up.width; r[3] = up.height; }   // the scaled picture, the viewport's size: drawn 1:1
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { MLOG("[RED] failed to create the M4 present render encoder"); return 0; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [en setArgumentTable:g_present_args atStages:MTLRenderStageFragment];
    [g_present_args setTexture:tex.gpuResourceID atIndex:0];
    [g_present_args setSamplerState:g_blit_samp.gpuResourceID atIndex:0];
    float *uv = uvb.contents; NSUInteger tw = tex.width, th = tex.height, cx = r[0], cy = r[1], cw = r[2], ch = r[3];
    uv[0] = (float)cx / tw; uv[1] = (float)cy / th; uv[2] = (float)cw / tw; uv[3] = (float)ch / th;
    uv[4] = (cx + 0.5f) / tw; uv[5] = (cy + 0.5f) / th; uv[6] = (cx + cw - 0.5f) / tw; uv[7] = (cy + ch - 0.5f) / th;
    if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    [g_present_args setAddress:uvb.gpuAddress atIndex:0];
    [en setViewport:vp]; [en setRenderPipelineState:g_blit_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    m4_end(en);
    return up ? PRESENT_METALFX : PRESENT_STANDARD;
}
// The present's picture drawn into an offscreen texture (out: private, render target) the way present_now draws it into the drawable, MetalFX included, on the Metal 4 queue;
// returns whether it was submitted. Completion reports the encoding kind, or PRESENT_FAILED on a GPU error. Resources remain resident and retained until GPU completion.
static int present_offscreen_m4(id<MTLTexture> src, const int crop[4], id<MTLTexture> out, void (^done)(int)) {
    id<MTL4CommandAllocator> al = [g_dev newCommandAllocator]; id<MTL4CommandBuffer> cb = [g_dev newCommandBuffer];
    id<MTLBuffer> uvb = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
    if (!al || !cb || !uvb) { MLOG("[RED] offscreen present: no command allocator or buffer"); return 0; }
    id<MTLTexture> raw = present_raw_src(src);
    fx_state fx = {0};
    res_add(out); res_add(uvb); res_commit();
    [cb beginCommandBufferWithAllocator:al];
    int kind = present_encode_m4(cb, raw, out, crop, uvb, &fx);
    [cb endCommandBuffer];
    if (!kind) { res_remove(out); res_remove(uvb); res_remove(fx.in); res_remove(fx.out); return 0; }
    MTL4CommitOptions *options = [MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        (void)al; (void)cb; (void)raw; (void)fx.sc;   // keep the Metal 4 commands, source view and scaler alive too
        res_remove(out); res_remove(uvb); res_remove(fx.in); res_remove(fx.out);
        if (f.error) MLOG("[RED] offscreen present: %s", f.error.description.UTF8String);
        done(f.error ? PRESENT_FAILED : kind);
    }];
    [g_q4 commit:&cb count:1 options:options];
    return 1;
}
// Diagnostic readback and tests wait for the same asynchronous capture used by whole-window screenshots. A timeout leaves cleanup with the GPU completion handler.
static int present_offscreen_wait(id<MTLTexture> src, const int crop[4], id<MTLTexture> out) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block int kind = PRESENT_FAILED;
    if (!present_offscreen_m4(src, crop, out, ^(int k) { kind = k; dispatch_semaphore_signal(sem); })) return 0;
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) { MLOG("[RED] offscreen present: the GPU did not finish within 10 s"); return 0; }
    return kind;
}
// The present command buffer's GPU time (its commit feedback: from its first to its last command, the MetalFX scaler and the present pass), every 600 presents as p50 / p95, apart
// for presents through MetalFX and through the bilinear pass. Under g_pq_mu (the feedback handler)
static void present_gpu_note(double s, int fx) {
    static double v[2][600]; static int n[2];
    if (!(s > 0 && s < 1)) return;
    v[fx][n[fx]++] = s * 1e3;
    if (n[fx] < 600) return;
    double *a = v[fx]; for (int i = 1; i < 600; i++) { double x = a[i]; int j = i - 1; while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; } a[j + 1] = x; }
    MLOG("present command buffer GPU time over 600 presents %s: p50 %.3f ms, p95 %.3f ms, max %.3f ms", fx ? "through MetalFX" : "through the bilinear pass", a[299], a[569], a[599]);
    n[fx] = 0;
}
static double g_link_it_tin;   // Enqueue time of the frame the display-link callback is presenting (read and written only on the display-link thread)
static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t ticket, const int crop[4], id<CAMetalDrawable> supplied, double target_ts) {
    if (g_present_stall_ns) { struct timespec ts = {0, (long)g_present_stall_ns}; nanosleep(&ts, NULL); }
    g_present_n++;
    if (bootprof() && (g_present_n == 1 || g_present_n % 120 == 0)) {
        if (g_present_n == 1) g_bp_first_present = bp_now() - g_bp_t0;
        MLOG("[bootprof] t=%.3f s present #%llu: metallibs loaded %llu / %.1f ms; pipelines created %llu / %.1f ms (all synchronous on the submit thread); prewarmed and used: shaders %llu / render pipelines %llu / compute pipelines %llu; "
             "texture uploads %.1f MB / %llu first uploads + %llu re-uploads / %.1f ms; first present t=%.3f s", (bp_now() - g_bp_t0) / 1e9, g_present_n,
             g_bp_lib_n, g_bp_lib_ns / 1e6, g_bp_pso_n, g_bp_pso_ns / 1e6, g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps,
             g_bp_up_bytes / 1048576.0, g_pkg_uploads, g_pkg_reuploads,
             g_up_total_ns / 1e6, g_bp_first_present / 1e9);
    }
    PD("X ser=%llu", t->serial);
    id<MTLTexture> src = tex_mtl(t);
    if (!src) { RED_ONCE(8, "present texture could not get an MTLTexture"); return 0; }
    // A texture made on first resolution joins the residency set (res_add), which a submission commits; a window texture presented before any submission has used it was still
    // outside the committed set when the display command buffer sampled it. Super Smash Bros. Ultimate presents its window texture in its first frames before drawing into it:
    // the display queue page-faulted (kIOGPUCommandBufferCallbackErrorPageFault) and macOS then ignored every later command buffer of the process, so the window stayed pink
    // (headless runs never present, and every game before drew into a window texture before presenting it). A commit with nothing new costs nothing
    res_commit();
    // DM_NVN_REGLUM=x0,y0,x1,y1:file (normalized coordinates): read back this region on every present and record "present sequence number, present time ns, region mean luminance, subsampled grayscale thumbnail (hex)" (diagnostic: verify that animation speed is independent of frame rate).
    // A separate command buffer is queued after this frame's rendering (the same queue executes in submit order); does not wait for the GPU and does not disturb frame pacing
    static int rl = -1; static float rr[4]; static FILE *rlf; static pthread_mutex_t rlmu = PTHREAD_MUTEX_INITIALIZER;
    if (rl < 0) { const char *e = getenv("DM_NVN_REGLUM"), *c = e ? strchr(e, ':') : NULL;
        rl = c && sscanf(e, "%f,%f,%f,%f", &rr[0], &rr[1], &rr[2], &rr[3]) == 4 && (rlf = fopen(c + 1, "w")) != NULL;
        if (e && !rl) MLOG("[RED] DM_NVN_REGLUM argument or file invalid: %s", e); }
    if (rl && (src.pixelFormat == MTLPixelFormatRGBA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)) {
        NSUInteger x0 = (NSUInteger)(rr[0] * src.width), y0 = (NSUInteger)(rr[1] * src.height), w = (NSUInteger)(rr[2] * src.width) - x0, h = (NSUInteger)(rr[3] * src.height) - y0;
        id<MTLBuffer> rb = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        unsigned long long n = g_present_n, tns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; const uint8_t *q = rb.contents; uint64_t sum = 0;
            uint8_t g[4096]; NSUInteger ng = 0;   // Subsampled grayscale thumbnail (every 4th row / column, at most 4096 points), recorded along with the mean, for per-frame image correlation analysis
            for (NSUInteger y = 0; y < h; y += 4) for (NSUInteger x = 0; x < w && ng < sizeof g; x += 4) { const uint8_t *px = q + 4 * (y * w + x); g[ng++] = (uint8_t)((px[0] + px[1] + px[2]) / 3); }
            for (NSUInteger k = 0; k < w * h; k++) sum += q[4 * k] + q[4 * k + 1] + q[4 * k + 2];
            pthread_mutex_lock(&rlmu); fprintf(rlf, "%llu\t%llu\t%.4f\t", n, tns, sum / (3.0 * w * h));
            for (NSUInteger k = 0; k < ng; k++) fprintf(rlf, "%02x", g[k]);
            fputc('\n', rlf); fflush(rlf); pthread_mutex_unlock(&rlmu); }];
        [cb commit];
    }
    // DM_NVN_SHOT=frame number[,frame number...]:path (%d in the path is replaced with the frame number)
    static long shots[4096]; static int nshot = -1; static const char *shot_path;   // up to 4096 frames (a burst over a frame range, for scoring frame by frame)
    if (nshot < 0) {
        nshot = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { shot_path = c + 1; for (const char *q = e; q < c && nshot < 4096; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q); }
    }
    for (int i = 0; i < nshot; i++) if ((long)g_present_n == shots[i]) { char path[1024]; snprintf(path, sizeof path, shot_path, (int)g_present_n); mtl_finish_gpu(); shot(src, path, 1, crop); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (g_present_n >= g_shotq[i].frame) { mtl_finish_gpu(); shot(src, g_shotq[i].path, 1, crop); g_shotq[i] = g_shotq[--g_nshotq]; } else i++;
    pthread_mutex_unlock(&g_shotq_mu);

    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
    if (g_present_layer_resident != layer) { [g_q4 addResidencySet:layer.residencySet]; g_present_layer_resident = layer; }
#if TARGET_OS_OSX
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;   // Windowed mode follows the display's vsync to prevent tearing
#endif
    present_pipeline(layer.pixelFormat);   // Same as the original path: the first frame compiles the pipeline before taking the drawable
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize);   // Game picture needed by the whole-window screenshot (peek without the lock first; the real take locks inside the function)
    uint64_t dt0 = now_ns();
    id<CAMetalDrawable> dr = supplied ? supplied : [layer nextDrawable];
    if (!supplied) g_drawable_ns += now_ns() - dt0;
    if (!dr) return 0;
    int alloc_i = (int)(ticket % 3);
    if (g_present_alloc_fence[alloc_i]) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_present_alloc_done[alloc_i] < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
    [g_present_alloc4[alloc_i] reset];
    [g_q4 waitForDrawable:dr];
    id<MTL4CommandBuffer> cb = g_present_cb[alloc_i];
    [cb beginCommandBufferWithAllocator:g_present_alloc4[alloc_i]];
    int kind = present_encode_m4(cb, src, dr.texture, crop, g_present_uv[alloc_i], &g_fx_present);
    if (!kind) { [cb endCommandBuffer]; [g_q4 signalDrawable:dr]; return 0; }
    int fx_kind = kind == PRESENT_METALFX;
    [cb endCommandBuffer];
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) MLOG("[RED] display command buffer error: %s", f.error.description.UTF8String);
        pthread_mutex_lock(&g_pq_mu); if (ticket > g_pq_done) g_pq_done = ticket; if (ticket > g_present_alloc_done[alloc_i]) g_present_alloc_done[alloc_i] = ticket; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv);
        present_gpu_note(f.GPUEndTime - f.GPUStartTime, fx_kind); pthread_mutex_unlock(&g_pq_mu);
    }];
    id<MTL4CommandBuffer> buffers[] = {cb};
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    [g_q4 commit:buffers count:1 options:o];
    if (target_ts > 0) {
        static double leads[300], e2e[300];   // e2e: game enqueue -> system-scheduled present (including the time waiting in the queue for the display-link callback), the full span of input latency that our side can read
        static uint64_t lead_n;
        leads[lead_n % 300] = (target_ts - CACurrentMediaTime()) * 1000.0; e2e[lead_n % 300] = g_link_it_tin > 0 ? (target_ts - g_link_it_tin) * 1000.0 : 0;
        lead_n++;
        if (lead_n % 300 == 0) {
            double a[300]; memcpy(a, leads, sizeof a);
            for (int i = 1; i < 300; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
            double b[300]; memcpy(b, e2e, sizeof b);
            for (int i = 1; i < 300; i++) { double v = b[i]; int j = i - 1; while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; } b[j + 1] = v; }
            MLOG("display link up to tick %llu: ticks 300, presents %llu, empty ticks %llu (held for the present interval %llu), old frames dropped by merging %llu; submission ahead of target display p50 %.3f ms / min %.3f ms; game enqueue to target display p50 %.3f ms / min %.3f / max %.3f ms",
                 (unsigned long long)g_link_tick_n, (unsigned long long)g_present_n, (unsigned long long)g_link_empty, (unsigned long long)g_link_held,
                 (unsigned long long)g_link_dropped, a[149], a[0], b[149], b[0], b[299]);
        }
    }
    [g_q4 signalDrawable:dr];
    g_present_alloc_fence[alloc_i] = ticket;
#if TARGET_OS_SIMULATOR   // The iOS simulator SDK has no presentation-time callback and no minimum display duration (used only for picture acceptance, display-link ticks are not counted)
    (void)period; [dr present];
#else
    uint64_t pn = ticket;   // Record the present time by the game's present sequence number (ticket number): hle's present statistics are taken by "the game's nth present"; in the display link path the actual present count != the game's present count (old frames get merged and dropped)
    [dr addPresentedHandler:^(id<MTLDrawable> d) {   // Actual presentation time (the time this frame is really put on screen, CACurrentMediaTime base)
        CFTimeInterval tt = d.presentedTime;
        g_pres[pn % PRING] = (pres_ent){pn, tt > 0 ? tt : -1};
    }];
    // Each frame is displayed for at least one present period: when the GPU completion time jitters around a 120 Hz refresh boundary, the paired "25 ms + 8.33 ms" stutter no longer appears
    if (target_ts > 0) [dr present];
    else if (period > 0) [dr presentAfterMinimumDuration:period]; else [dr present];
#endif
    (void)frame;
    return 1;
}
// ---------- Present thread ----------
// NVN semantics: QueuePresentTexture only queues the present and returns immediately (the game often calls it while holding a lock; iOS real device: synchronously waiting for nextDrawable inside it
// takes about 30 ms, the lock is held about 90% of the time, and the loading thread starves). Taking the drawable, blit and present are all done on this thread in enqueue order.
// Ticket number = which enqueue it is (starting from 1); g_pq_done = the ticket number that has been processed (blit committed to the GPU queue).
// Swapchain texture return: before AcquireTexture gets a texture it waits for the ticket of that texture's previous present to be processed (mtl_present_wait), after which the commands the game draws into it
// are necessarily ordered after that blit on the GPU queue.
#define PQ 8
typedef struct { tex_t t; double period; uint64_t frame; int crop[4]; double t_in; uint64_t ev_val, snap_v; } pq_item;   // snap_v: the snapshot copy this present shows (snap_commit_wait)   // t_in: the time the game handed this frame into the present queue (CACurrentMediaTime); ev_val: see g_ext_ev
// External producer (the Vulkan layer, dm/gfx/vk) renders on its own queue and signals this event when a frame's rendering is done: the present of that frame waits for the value on g_q4
static id<MTLSharedEvent> g_ext_ev;
static void ext_wait(uint64_t v) { if (v && g_ext_ev) [g_q4 waitForEvent:g_ext_ev value:v]; }
static void snap_commit_wait(uint64_t v);
static pq_item g_pq[PQ];
static uint64_t g_pq_in;
static int present_now(const tex_t *, double, uint64_t, uint64_t, const int[4], id<CAMetalDrawable>, double);
static double g_link_vperiod, g_link_vhalf;   // virtual ticks (link_range): seconds per counted tick (0 = every callback is a tick) and half a refresh of the screen

@interface NvMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

// Fallback for the display link going silent while in the foreground (two smoke-test hangs before merge: the display link stopped calling back while the app was in the foreground, nobody takes the queued frames, and the game waits forever for its previous present to be processed before "acquiring the next swapchain texture"):
// when mtl_present_wait has waited longer than STALL_NS and the display link has not called back for that long, drop the waiting frame and earlier queued frames as when entering the background (accounted as dropped frames) and let the game keep running; when the display link comes back things return to normal automatically.
// One stall logs one hint line, and one line on recovery (neither is a failure); only when the same stall lasts longer than STALL_RED_NS is it a failure. All under g_pq_mu
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n; static int g_stall_red, g_stall_logged;
// When the display link stays silent for LINK_RESCUE_NS while the app is active, presents stop waiting for it: mode 0 (the present thread takes drawables itself), so the
// picture keeps moving instead of standing still on dropped frames (d2r-mobile afad65c, via MacNativeGames cf1e629: an iPhone's link stopped while active and the picture
// froze for 80 s; macOS also pauses the link of a window it takes for covered). The link is tried again after g_rescue_back_ns (link_sync), which doubles on each new
// stall up to 60 s. The requested g_dm_present_link stays unchanged; all rescue state changes take g_pq_mu, and link_sync handles the link object on the enqueue thread.
#define LINK_RESCUE_NS 500000000ull
static _Atomic int g_rescue_mode;   // Also read by link_on without g_pq_mu, to stop pacing on silent ticks immediately
static uint64_t g_rescue_back_at, g_rescue_back_ns = 3000000000ull, g_rescue_n;
// The display link watchdog only recognizes the "active" state: when the App loses active status (pulling down Control Center / notifications, incoming call, system popup) and in the background the system does not call back anyway, and the picture is covered too -- the frame-drop fallback stays as before,
// but it is not counted as stuck and no failure is reported; the stall is measured from the later of "the most recent callback / the most recent activation" (A16 logs: 8–17 seconds inactive and 17 seconds in the background were both reported as "stuck in foreground")
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // The host's hle_oe.c provides the strong definition; when graphics unit tests do not link the host it is treated as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    double cb = CACurrentMediaTime();
    pthread_mutex_lock(&g_pq_mu);
    if (g_stall_t0) { uint64_t since = dm_focus_in_since(), t = now_ns();
        MLOG("display link recovered: stalled %.0f ms (%.0f ms of it active), fallback dropped %llu frames (stall #%llu)", (t - g_stall_t0) / 1e6, (t - (g_stall_t0 > since ? g_stall_t0 : since)) / 1e6, (unsigned long long)g_stall_drop, (unsigned long long)g_stall_n); g_stall_t0 = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    double vp = g_link_vperiod, tol = 0.002;   // tol: how early a tick may come and still end a hold (below)
    if (vp > 0) {   // virtual ticks: only the refresh nearest each slot of a steady clock at the wanted rate counts (link_range)
        static double next, vp_was; double t = update.targetPresentationTimestamp;
        if (vp != vp_was || t > next + vp) { next = t; vp_was = vp; }   // the first callback, a new rate, or after a stall: the clock starts again here
        if (t < next - g_link_vhalf) return;
        next += vp; tol += 2 * g_link_vhalf;   // a tick lies up to half a refresh off its slot, so two of them up to one refresh nearer each other than the slots
    }
    static double cb_prev, gap_max; static int nticks;   // for g_link_ring: callbacks counted as ticks since the previous present, and the longest interval between them
    if (cb_prev > 0 && cb - cb_prev > gap_max) gap_max = cb - cb_prev;
    cb_prev = cb; nticks++;
    pthread_mutex_lock(&g_link_mu);
    __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED);
    g_link_tick_n++;
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    if (!g_dm_present_link || g_rescue_mode || g_pq_read == g_pq_in) { if (g_dm_present_link && !g_rescue_mode) g_link_empty++; pthread_mutex_unlock(&g_pq_mu); return; }
    // The previous frame's present interval was >= 2 (period > 25 ms): keep it on screen for its whole period before taking the next one, as the Switch flips only every N vertical syncs. This also holds while the link runs at 60 Hz (Mario 1.2.1 sets 2 for single frames, see nvn_game_fps in hle_nvn.c);
    //   before, every tick took the newest frame, so that frame, carrying the extra time step the game advanced for it, was shown for only 16.7 ms and the game looked like it jumped ahead
    if (g_link_hold_until > 0 && update.targetPresentationTimestamp < g_link_hold_until - tol) { g_link_held++; pthread_mutex_unlock(&g_pq_mu); return; }
    uint64_t first = g_pq_read + 1, ticket = g_pq_in;
    // In order, one frame per tick and at most one behind the newest: two waiting, the older now and the newer on the next tick; three or more, the older ones are dropped. Taking the newest
    // dropped the one before whenever two frames landed in the same refresh, as frames of a 60 fps game that take 16-17.5 ms do, and the next frame then stayed up 33 ms
    // (Dark Souls Remastered, from PR #31: presents within 14.7-18.7 ms 98.2 -> 99.98 %, about 2 ms more from enqueue to display). DM_PRESENT_FIFO=0 takes the newest, as before
    static int fifo = -1; if (fifo < 0) { const char *e = getenv("DM_PRESENT_FIFO"); fifo = !(e && !strcmp(e, "0")); MLOG("display link: %s", fifo ? "frames in order, at most one behind" : "the newest frame each tick (DM_PRESENT_FIFO=0)"); }
    if (fifo && ticket > first) ticket = ticket - first >= 2 ? g_pq_in - 1 : first;
    pq_item it = g_pq[(ticket - 1) % PQ]; g_link_it_tin = it.t_in;
    g_pq_read = ticket;
    if (ticket > first) { g_link_dropped += ticket - first; for (uint64_t k = first; k < ticket; k++) g_pres[k % PRING] = (pres_ent){k, -1}; }   // Old frames merged and dropped are recorded as "not presented", so the present statistics honestly count them as dropped frames
    if (ticket > 1 && g_pq_done < ticket - 1) g_pq_done = ticket - 1;
    g_link_ring[ticket % PRING] = (link_ent){ticket, update.targetPresentationTimestamp, (update.targetPresentationTimestamp - cb) * 1e3, gap_max * 1e3, nticks}; nticks = 0; gap_max = 0;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    int submitted;
    ext_wait(it.ev_val); snap_commit_wait(it.snap_v);
    @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, update.drawable, update.targetPresentationTimestamp); }
    g_link_hold_until = submitted && it.period > 0.025 ? update.targetPresentationTimestamp + it.period : 0;
    if (!submitted) {
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        pthread_cond_broadcast(&g_pq_cv);
        pthread_mutex_unlock(&g_pq_mu);
    }
}
@end

static CAMetalDisplayLink *g_link; static CFRunLoopRef g_link_rl; static int g_link_started;
static int g_link_owns_layer, g_classic_busy;   // Under g_pq_mu: nextDrawable throws while the layer has the display link object attached, so the present thread that takes its own drawable and the display link object are mutually exclusive
static int link_on(void) { return g_dm_present_link && !atomic_load(&g_rescue_mode) && g_link != nil; }
// Screens whose refresh rate is not a multiple of the link's (144 or 165 Hz against 60 or 30): asked for 60 Hz, the system fires the link on every third refresh of a 165 Hz screen,
// 55 Hz, and a game paced by the link runs at 55 fps (27.5 at 30). There the link runs at the screen's rate and only the refreshes nearest a steady clock of the wanted rate count as
// ticks (g_link_vperiod, NvMetalDisplayLinkDelegate): 60 a second on average, each frame shown on the refreshes nearest its 1/60 s slot. DM_LINK_VTICK=0 keeps the system's choice.
// In fullscreen the Mac front end switches such a screen to a rate that is a multiple where it has one (front_screen_match), and the range is set again for it (link_sync)
__attribute__((weak)) int front_screen_max_fps(void) { return 0; }   // front_mac.m; graphics unit tests and iOS have none
__attribute__((weak)) void front_screen_match(int fps) { (void)fps; }   // front_mac.m; graphics unit tests and iOS have none
static CAFrameRateRange link_range(int mode) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_LINK_VTICK"); on = !(e && !strcmp(e, "0")); }
    int rate = mode == 3 ? 30 : mode == 2 ? 120 : 60, hz = on ? front_screen_max_fps() : 0;
    if (mode != 2 && hz > rate && hz % rate) {
        if (g_link_vperiod != 1.0 / rate) MLOG("display link at the screen's %d Hz, counting a tick every 1/%d s (the screen's rate is not a multiple of it)", hz, rate);
        g_link_vhalf = 0.5 / hz; g_link_vperiod = 1.0 / rate;
        return CAFrameRateRangeMake(rate, hz, hz);
    }
    g_link_vperiod = 0;
    return mode == 3 ? CAFrameRateRangeMake(30, 30, 30) : mode == 2 ? CAFrameRateRangeMake(60, 120, 120) : CAFrameRateRangeMake(60, 60, 60);
}
// The display link's frame latency: 1 in a window, 2 in fullscreen (DM_PRESENT_LATENCY sets one for both). In a window the system calls back about 50 ms before the target display
// time; in fullscreen it shows the layer directly and, at latency 1, calls back only one 60 Hz period (16 ms) ahead. There, on ProMotion screens (M4 Pro, M5 Max), it shows some
// presents one 120 Hz refresh late and the link skips its next tick, though the callbacks run on time and no GPU work is in the way; the frame step read the late frames as load
// and stepped to 20 fps on frames costing 0.1-0.6 vsyncs, and the game presented 15 frames a second (issues #347, #348). Latency 2 gives 33 ms and removed most of the misses on
// both machines, for one more frame of input lag in fullscreen only. A workaround: why the system misses at 16 ms is not known (docs/reference/frame-rate.md)
__attribute__((weak)) int front_fullscreen(void) { return 0; }   // front_mac.m; graphics unit tests and iOS have none
static int link_latency(void) { return g_present_latency > 0 ? g_present_latency : front_fullscreen() ? 2 : 1; }
static const char *link_latency_why(void) { return g_present_latency > 0 ? "DM_PRESENT_LATENCY" : front_fullscreen() ? "fullscreen" : "window"; }
static int g_link_mode = 1;   // Current mode of the display link object: 1 / 2 same as g_dm_present_link, 3 = power-saving mode 30 Hz
static void *link_main(void *arg) {   // Display link thread: started when switching to display-link mode, exits when switching back to 0
    (void)arg;
    pthread_setname_np("nvn-present-link");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; while (g_classic_busy) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu);   // Wait for the present thread to finish handing over the frame it holds
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    link.preferredFrameRateRange = link_range(g_link_mode);
    link.preferredFrameLatency = link_latency(); MLOG("display link: frame latency %d (%s)", link_latency(), link_latency_why());
    link.delegate = delegate;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    CFRunLoopRun();   // When switching back to mode 0 the display link object is invalidated and this thread exits (nextDrawable cannot get a drawable while the layer still has the display link object attached, so pausing alone is not enough)
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // Called every frame by the game's present thread: aligns the display link object's state to g_dm_present_link and the power-saving mode (can change at run time)
    static int applied;
    pthread_mutex_lock(&g_pq_mu);
    int want = g_dm_present_link;
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; pthread_mutex_unlock(&g_pq_mu); return; }
    if (g_rescue_mode && (want != g_rescue_mode || now_ns() >= g_rescue_back_at)) {
        g_rescue_mode = 0; g_stall_t0 = 0; __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED);
        MLOG("display link: rescue ended, trying pacing mode %d (frame %llu)", want, (unsigned long long)atomic_load(&g_cur_frame));
    }
    if (g_rescue_mode) want = 0;
    pthread_mutex_unlock(&g_pq_mu);
    if (want && mtl_fps_target() == 30) want = 3;   // Power-saving mode: display link at 30 Hz
    if (want) front_screen_match(want == 3 ? 30 : want == 2 ? 120 : 60);
    // The screen's rate changed (switched by front_screen_match, or another screen): the range again, virtual ticks or none. Taken only once the link object exists, so a change
    // while its thread starts (after link_main read the rate) is not lost
    static int hz_was; int hz = front_screen_max_fps(); CAMetalDisplayLink *lk = g_link;
    if (lk && hz != hz_was) { hz_was = hz; if (want && want == applied) { lk.preferredFrameRateRange = link_range(want); MLOG("display link: the screen runs at %d Hz now, range set again (frame %llu)", hz, (unsigned long long)atomic_load(&g_cur_frame)); } }
    int lat = link_latency();   // the window entered or left fullscreen: the latency for it (a link created since took the current one, so a window's link is never set again)
    if (lk && lk.preferredFrameLatency != lat) { lk.preferredFrameLatency = lat; MLOG("display link: frame latency %d (%s; frame %llu)", lat, link_latency_why(), (unsigned long long)atomic_load(&g_cur_frame)); }
    if (want == applied) return;
    g_link_mode = want;
    if (!g_link_started) {
        if (!want) { applied = 0; return; }
        if (!front_metal_layer()) return;
        g_link_started = 1; pthread_t th; if (pthread_create(&th, NULL, link_main, NULL)) MDIE("cannot start the display link thread"); pthread_detach(th);
        return;   // The display link object is created on a new thread; align again next frame
    }
    CAMetalDisplayLink *link = g_link; if (!link) return;
    if (want) link.preferredFrameRateRange = link_range(want);
    else { CFRunLoopRef rl = g_link_rl; g_link = nil; CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{ [link invalidate]; pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); CFRunLoopStop(rl); }); CFRunLoopWakeUp(rl); }
    MLOG("present pacing mode %d → %d (frame %llu)", applied, want, (unsigned long long)atomic_load(&g_cur_frame));
    applied = want;
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
// Before going to the background (mtl_drain): when the display link drives presents, frames queued but not yet presented can only be taken by the display-link callback, and in the background the system no longer calls back --
// waiting as-is for "the present queue to empty" would wait forever, the enter-background callback would not return, and the system would kill the process (the process disappears within 22 seconds after switching to another App).
// These frames will not be seen anyway: discard them directly (accounted as dropped frames) and only wait for the one present already submitted to the GPU to finish.
static void present_drop_for_suspend(void) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_link_owns_layer) {
        uint64_t n = g_pq_in - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= g_pq_in; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = g_pq_in;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < g_pq_in) g_pq_done = g_pq_in;
        pthread_cond_broadcast(&g_pq_cv);
        if (n) MLOG("entering the background: dropping %llu queued frames not yet displayed (the display link does not fire in the background)", (unsigned long long)n);
    }
    pthread_mutex_unlock(&g_pq_mu);
}
static void *present_main(void *arg) {
    (void)arg;
    pthread_setname_np("nvn-present");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp); atomic_store(&g_present_qos, (uint32_t)qc);
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_read == g_pq_in || g_link_owns_layer) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // In display-link mode the display-link callback takes them; an empty queue is judged by "taken"; "done" has to wait for the feedback of the present submit and lags behind taken
        pq_item it = g_pq[g_pq_read % PQ];
        uint64_t ticket = ++g_pq_read; g_classic_busy = 1;
        pthread_mutex_unlock(&g_pq_mu);
        int submitted;
        ext_wait(it.ev_val); snap_commit_wait(it.snap_v);
        @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, nil, 0); }
        pthread_mutex_lock(&g_pq_mu); g_classic_busy = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
        if (!submitted) {
            pthread_mutex_lock(&g_pq_mu);
            if (ticket > g_pq_done) g_pq_done = ticket;
            pthread_cond_broadcast(&g_pq_cv);
            pthread_mutex_unlock(&g_pq_mu);
        }
    }
    return NULL;
}
uint32_t mtl_present_qos(void) { return atomic_load(&g_present_qos); }
static void present_start(void) { pthread_t th; if (pthread_create(&th, NULL, present_main, NULL)) MDIE("cannot start the present thread"); pthread_detach(th); }
static _Atomic int g_present_discard;
void mtl_link_pause(int on);
// The short period when the front end goes to the background and waits for the game to save: presents are dropped directly (the display link does not call back in the background, enqueueing would block the game thread; nobody sees the picture anyway).
// When turned on, also clear the frames already queued: the system first stops the display-link callback and only afterwards notifies about going to the background, and in that gap the game has already filled the swapchain and is stuck at "before acquiring the next texture, wait for its previous present to be processed"
// (hle_nvn.c nvnWindowAcquireTexture -> mtl_present_wait). Without clearing, the game cannot advance a single tick during those 2.5 seconds of waiting for the save, and the save is not written (the same pattern three times in player logs: 3 frames queued, 0 submits)
void mtl_present_discard(int on) { atomic_store(&g_present_discard, on != 0); if (on) present_drop_for_suspend(); }
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t ev_val);
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) { return present_enqueue(t, period, crop, 0); }
void *mtl_device(void) { mtl_init(); return (__bridge void *)g_dev; }
void *mtl_device_classic(void) { g_classic_ok = 1; return mtl_device(); }   // the GL and Vulkan layers: a device without Metal 4 is taken too, with presents dropped (mtl_init)
// An external producer's texture (the Vulkan layer's swapchain images): a serial the present path resolves (serial_put), resident for the Metal 4 queue, never collected by the cross-submit ledger
uint64_t mtl_ext_tex(void *tex) {
    static _Atomic uint64_t n; id<MTLTexture> t = (__bridge id<MTLTexture>)tex; uint64_t ser = (1ull << 62) + atomic_fetch_add(&n, 1) + 1;
    res_add(t); serial_put(ser, t); mtl_xv_window_tex(ser); return ser;
}
// Present an external texture after the producer's event reaches ev_val (its rendering of the frame is done); returns the ticket for mtl_present_wait
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) {
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = serial;
    if (ev && !g_ext_ev) g_ext_ev = (__bridge id<MTLSharedEvent>)ev;
    return present_enqueue(&t, period, crop, ev ? ev_val : 0);
}
extern _Atomic uint64_t dm_present_ns;   // hle_thread.c (DM_HITCH_DUMP); the NVN, Vulkan and GL presents all come through present_enqueue
// Present snapshots (DM_PRESENT_SNAP). A game with two swapchain textures (Breath of the Wild) gets one back only when the display link has taken its previous present on a
// tick, so the next frame's CPU work starts on the tick and the GPU idles from the end of one frame to the first work of the next. macOS reads that idle time as room and holds the GPU
// at its lowest clock (444 MHz on an M2 Max) even while frames miss the screen, and there 12 ms of GPU work a frame plus the time to the frame's first submission often missed 16.7 ms.
// With snapshots each present is copied on the game's queue, right after the frame's work, into a texture of the host's own (a ring of g_snap_n), the display link shows the copy, and
// the game's texture is free at once: the game runs up to a frame ahead of the screen, and a frame whose GPU work runs long is absorbed instead of shown late. A ring slot is reused
// once its present has been processed and its copy has completed; AcquireTexture waits for that capacity and counts it as pacing, not as the frame's cost.
// A present that reaches a busy slot (several textures acquired ahead of time) keeps the original texture and its ticket instead of waiting while the game may hold a lock.
// With Metal encoding on its own thread (DM_NVN_ENC_THREAD) the copy is a block of the encode thread's queue (rec_own_push): it runs behind everything the game submitted before
// the present, the presenting thread does not wait for the encode thread, and the frame's present waits only until that block is encoded (snap_commit_wait), before its blit.
// The ring's length is the number of frames the game may run ahead of the screen: DM_PRESENT_SNAP=<n> (2 to SNAP_MAX; 1 = 2). Off by default; the iOS launcher's Frame buffering toggle enables it.
#define SNAP_MAX 4
// old: the slot's texture before the presented size or format last changed, resident until the slot's next use (snap_take)
static struct { id<MTLTexture> tex, old; uint64_t serial; _Atomic uint64_t ticket, ev; } g_snap[SNAP_MAX];
static id<MTL4CommandAllocator> g_snap_alloc[SNAP_MAX]; static id<MTL4CommandBuffer> g_snap_cb[SNAP_MAX];
static id<MTLResidencySet> g_snap_res; static id<MTLSharedEvent> g_snap_ev; static uint64_t g_snap_v;
static _Atomic uint64_t g_snap_next, g_snap_cmt;   // g_snap_cmt: the last copy the encode thread has committed (its g_snap_v)
static int g_snap_n = 2;
int mtl_snap_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_PRESENT_SNAP");
    int n = e ? atoi(e) : DM_GAME_PRESENT_SNAP ? 2 : 0;   // the profile's present_snap (a ring of 2)
    v = n > 0; g_snap_n = n < 2 ? 2 : n > SNAP_MAX ? SNAP_MAX : n;
    if (v) MLOG("present snapshots (DM_PRESENT_SNAP=%d): each present is copied into a ring of %d host textures and the game's texture is free at once", n, g_snap_n);
    return v;
}
static void snap_commit_wait(uint64_t v) { if (v) rec_note_wait(&g_snap_cmt, v); }
static void snap_drain(void) {
    // Display tickets may have been dropped during suspend, but their copies still belong to the Metal 4 queue.
    uint64_t v = g_snap_v; if (!v) return;
    snap_commit_wait(v);
    if (![g_snap_ev waitUntilSignaledValue:v timeoutMS:10000]) MDIE("present snapshots: GPU did not drain within 10 s");
}
uint64_t mtl_snap_acquire_wait(void) {
    if (!mtl_snap_on() || atomic_load(&g_present_discard)) return 0;
    int k = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
    uint64_t ticket = atomic_load(&g_snap[k].ticket), ev = atomic_load(&g_snap[k].ev), w0 = ws_t0();
    pthread_mutex_lock(&g_pq_mu); int blocked = ticket > g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    if (ticket) mtl_present_wait(ticket);
    if (ev && !atomic_load(&g_present_discard) && g_snap_ev.signaledValue < ev) {
        blocked = 1;
        // On timeout present falls back to its original texture; the busy allocator is never reset.
        if (![g_snap_ev waitUntilSignaledValue:ev timeoutMS:2000]) RED_ONCE(111, "present snapshots: the previous copy into a ring slot did not finish within 2 s");
    }
    if (ws_on()) ws_put(WS_SNAP, w0, blocked);
    return ws_t0() - w0;
}
// Copy the presented texture into the next ring slot; returns the slot's serial (0 = no snapshot: the game's texture is presented as before)
static uint64_t snap_take(const tex_t *t) {
    int k = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
    pthread_mutex_lock(&g_pq_mu); int busy = atomic_load(&g_snap[k].ticket) > g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    uint64_t ev = atomic_load(&g_snap[k].ev);
    if (busy || (ev && g_snap_ev.signaledValue < ev)) return 0;
    // The texture this slot held before it was last replaced: every present that read it had been committed when the replacing copy was (the busy check then), and the queue
    // signals g_snap_ev only after all work committed before the signal, so those presents have run and it can leave the residency set
    if (g_snap[k].old) { [g_snap_res removeAllocation:g_snap[k].old]; [g_snap_res commit]; g_snap[k].old = nil; }
    id<MTLTexture> src = tex_mtl(t); if (!src) return 0;
    // The copy reads the source before present_now runs, so it needs the same commit as present_now: a window texture presented before any submission has used it was made just
    // now and is outside the committed residency set. DRAGON QUEST XI S presents three times before its first submission; on an A17 Pro the copy faulted silently (its command buffer
    // has no error handler) and every later command buffer of the process was ignored (GitHub issue #626). A commit with nothing new costs nothing
    res_commit();
    if (!g_snap_ev) {
        g_snap_ev = [g_dev newSharedEvent];
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.initialCapacity = SNAP_MAX; NSError *e = nil;
        g_snap_res = [g_dev newResidencySetWithDescriptor:rd error:&e]; if (!g_snap_res) MDIE("present snapshots: no residency set: %s", e.localizedDescription.UTF8String);
        [g_q4 addResidencySet:g_snap_res];
        for (int i = 0; i < g_snap_n; i++) {
            g_snap_alloc[i] = [g_dev newCommandAllocator]; g_snap_cb[i] = [g_dev newCommandBuffer];
            if (!g_snap_alloc[i] || !g_snap_cb[i]) MDIE("present snapshots: no command allocator / buffer");
        }
    }
    id<MTLTexture> dst = g_snap[k].tex;
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height mipmapped:NO];
        d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
        // The old texture stays resident (and referenced) until the slot's next use: the busy check above means its last present was committed, not that the GPU has run it,
        // and a texture removed here was sampled outside the residency set by a display command buffer still queued
        g_snap[k].old = dst;
        dst = [g_dev newTextureWithDescriptor:d]; if (!dst) { RED_ONCE(112, "present snapshots: no texture"); return 0; }
        [g_snap_res addAllocation:dst]; [g_snap_res commit];
        if (!g_snap[k].serial) { static _Atomic uint64_t n; g_snap[k].serial = (1ull << 61) + atomic_fetch_add(&n, 1) + 1; }
        serial_put(g_snap[k].serial, dst); g_snap[k].tex = dst;
        MLOG("present snapshots: ring slot %d %lux%lu format %lu", k, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat);
    }
    [g_snap_alloc[k] reset];
    id<MTL4CommandBuffer> cb = g_snap_cb[k];
    [cb beginCommandBufferWithAllocator:g_snap_alloc[k]];
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];   // after the frame's work and the slot's last blit to the screen
    [ce copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // the game's next writes to its texture come after the copy
    [ce endEncoding]; [cb endCommandBuffer];
    uint64_t v = ++g_snap_v;
    if (rec_on() && g_rb_started) {   // a block of the encode thread's queue, behind the frame's work (see above)
        if (on_sub_thread()) rec_publish();   // what the submitting thread recorded outside a submission goes first
        rec_blk *b = rec_own_begin();
        rec_t *r = rec_own_put(b, RO_COMMIT_REAL, 2); r->a[0] = 0; r->a[1] = rec_obj(cb);
        r = rec_own_put(b, RO_SIGNAL, 1); r->a[0] = rec_obj(g_snap_ev); r->a[1] = v;
        r = rec_own_put(b, RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&g_snap_cmt; r->a[1] = v;
        rec_own_push(b);
    } else {
        pthread_mutex_lock(&g_res_mu); [g_q4 commit:&cb count:1]; [g_q4 signalEvent:g_snap_ev value:v]; pthread_mutex_unlock(&g_res_mu);
        atomic_store(&g_snap_cmt, v);
    }
    g_snap[k].ev = v;
    return g_snap[k].serial;
}
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t ev_val) {
    atomic_store(&dm_present_ns, now_ns());
    if (g_classic || atomic_load(&g_present_discard)) return 0;   // Ticket number 0 = no pending present; the swapchain texture acquire does not wait (g_classic: no Metal 4 queue to present on)
    if (t) { xv_check(t->serial, 0, 0, "present"); if (g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = t->serial; }
    // A texture whose content comes from its storage is uploaded when it is first resolved (and again when its pages change), in an upload batch only the submit thread may open.
    // Bastion fills both window textures with WriteTexels at boot (main+0x9374) and presents one before drawing into it: resolved on the present thread, the upload aborted the host.
    // Resolve it here, where the game queues the present, and commit the batch, so the display command buffer comes after the upload on the queue and the present thread finds it uploaded
    if (t && !ev_val && on_sub_thread() && tex_from_storage(t)) { tex_mtl(t); up_flush(); }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // Test switch DM_TEST_LINK_STALL=start seconds,duration seconds: counting from the first present, pause the display link at that point (imitating the display link not calling back while in the foreground) and release it when the duration is up. For verifying the fallback; not set in the product
        static double t0, at = -1, dur; static int st; double now = CACurrentMediaTime();
        if (at < 0) { at = 0; const char *e = getenv("DM_TEST_LINK_STALL"); if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0; }
        if (st == 1 && link_on() && now - t0 >= at) { st = 2; mtl_link_pause(1); MLOG("DM_TEST_LINK_STALL: pausing the display link for %.1f s (frame %llu)", dur, (unsigned long long)atomic_load(&g_cur_frame)); }
        else if (st == 2 && now - t0 >= at + dur) { st = 3; mtl_link_pause(0); MLOG("DM_TEST_LINK_STALL: resuming the display link (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame)); }
    }
    if (ws_on()) { static uint64_t f0; uint64_t f = atomic_load(&g_cur_frame); if (!f0) f0 = f; else if (f >= f0 + 300) { ws_report(f, f - f0); f0 = f; } }
    tex_t snap; uint64_t ss = 0; int slot = 0;
    if (t && !ev_val && mtl_snap_on()) {
        slot = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
        if ((ss = snap_take(t))) { memset(&snap, 0, sizeof snap); snap.magic = MAGIC('T', 'E', 'X', 0); snap.serial = ss; t = &snap; }
    }
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // Wait only when the queue is full; normally AcquireTexture blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime(), ev_val, ss ? g_snap[slot].ev : 0};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);   // The current frame's state is enqueued with the ticket number, rather than reading the mutable window late on the present thread
    uint64_t tk = ++g_pq_in;
    if (ss) { g_snap[slot].ticket = tk; atomic_fetch_add(&g_snap_next, 1); }
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return ss ? 0 : tk;   // a snapshot: the game's texture is free now (its later draws come after the copy on the queue)
}
// Do not wait during discard: the frame enqueued only after the flag was set (the present thread had just passed the flag check) has nobody to take it, and waiting for it would stall again; it is left for the following mtl_drain to clear
// Do not wait indefinitely in display-link mode: see the note at STALL_NS. Only frames "not yet taken by the display link" are dropped; the one already on its way to the screen is waited for until handed to the GPU (same dropping as when entering the background)
void mtl_present_wait(uint64_t tk) {
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_done < tk && !atomic_load(&g_present_discard)) {
        if (!g_link_owns_layer) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }
        if (g_pq_read >= tk) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }   // This one has already been taken by the display link and is being presented: wait for it to be handed to the GPU (the GPU completion callback will wake us)
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)STALL_NS; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        // Already determined to be in a stall (when the display link comes back the callback first clears g_stall_t0, then records the display-link tick time): no longer wait 100 ms in vain every frame, drop directly, and the game proceeds by its own display-link wait timeout (about two ticks)
        if (!g_stall_t0 && (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT || g_pq_done >= tk)) continue;
        uint64_t now = now_ns(), last = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED);
        if (g_pq_read >= tk) continue;
        if (now - last < STALL_NS) { pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts); continue; }   // The display link is still running, it is just not this one's turn yet: keep waiting (bounded, no busy spinning)
        uint64_t n = tk - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= tk; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = tk;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < tk) g_pq_done = tk;
        pthread_cond_broadcast(&g_pq_cv);
        if (!g_stall_t0) { g_stall_t0 = last ? last : now; g_stall_drop = 0; g_stall_red = 0; g_stall_logged = 0; g_stall_n++; }
        g_stall_drop += n;
        int active = dm_focus_now() == 1; uint64_t since = dm_focus_in_since(), from = g_stall_t0 > since ? g_stall_t0 : since;
        uint64_t elapsed = now >= from ? now - from : 0;   // Focus can return after the earlier now sample, including while waiting for an in-flight present
        if (active && !g_stall_logged && elapsed >= STALL_NS) { g_stall_logged = 1;
            MLOG("display link silent for %.0f ms (active): dropping %llu queued frames not yet displayed, the game continues (stall #%llu; back to normal when the link returns)", (now - from) / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n); }
        if (active && !g_rescue_mode && g_dm_present_link && elapsed >= LINK_RESCUE_NS) {
            g_rescue_mode = g_dm_present_link; g_rescue_n++; g_rescue_back_at = now + g_rescue_back_ns;
            MLOG("display link silent for %.0f ms while active: the present thread takes drawables itself (rescue #%llu), the link is tried again in %.0f s", (now - from) / 1e6, (unsigned long long)g_rescue_n, g_rescue_back_ns / 1e9);
            g_rescue_back_ns = g_rescue_back_ns < 30000000000ull ? g_rescue_back_ns * 2 : 60000000000ull;
        }
        if (active && !g_stall_red && elapsed >= STALL_RED_NS) { g_stall_red = 1; MLOG("[RED] display link silent for %.1f s while active; presenting relies entirely on fallback frame drops, the picture is frozen", elapsed / 1e9); }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
// Wait until all queued presents have been processed (used by mtl_finish = nvnQueueFinish etc.): goes through mtl_present_wait, so there is still a fallback when the display link stops (real device: the game called QueueFinish once while the display link was stopped,
// and this used to wait here without a timeout; nobody took the 3 queued frames and the game stalled; those two runs on Mac did not hit this call)
void mtl_present_flush(void) { pthread_mutex_lock(&g_pq_mu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pq_mu); mtl_present_wait(h); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; cp_report(); }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) {
    *resolve = g_vbuf_resolve; *reuse = g_vbuf_reuse; *set = g_vbuf_set; *skip = g_vbuf_skip; *copies = g_vbuf_copies; *copy_bytes = g_vbuf_copy_bytes;
}
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
uint64_t mtl_ds_omit(void) { return g_stat_ds_omit; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = g_bw_load; *store = g_bw_store; *full = g_pass_full_rw; }
uint64_t mtl_udc_saved_bytes(void) { return g_udc_saved_bytes; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_link_active(void) { return link_on(); }
int mtl_link_stalled(void) { return __atomic_load_n(&g_stall_t0, __ATOMIC_RELAXED) != 0; }   // the display link has gone silent and mtl_present_wait drops frames (see STALL_NS) until it calls back
void mtl_link_pause(int on) { CAMetalDisplayLink *l = g_link; if (l) l.paused = on != 0; }   // For testing (DM_TEST_BG): imitates the system stopping the display-link callback in the background
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    // Wait for the display link only when running faster than the display: if a display-link tick has already passed since the previous frame started work (this frame is already behind), start immediately instead of waiting in vain for the next display-link tick --
    // otherwise when the frame time is slightly more than one display-link period it would "miss the tick -> wait another whole tick", locking the frame rate straight to half (measured on Mac under windowed load: runs of dropped frames)
    // How many display-link ticks one frame takes: the game's present interval (vsyncs, 60 Hz) times 1 with a 60 Hz display link, times 2 with a 120 Hz one, halved and rounded up at 30 Hz.
    // A frame presented with interval N blocks the game for N vsyncs on the Switch, as the display callback holds it N ticks: games that scale their time step by the interval (BotW's VFR: interval / 2)
    // ran at double speed while they produced interval-2 frames every 60 Hz tick (iPhone log: Link sprinting with B, the interval flapping 1 ↔ 2)
    if (vsyncs < 1) vsyncs = 1;
    return mtl_link_wait_ticks(g_link_mode == 3 ? (vsyncs + 1) / 2 : vsyncs * (g_link_mode == 2 ? 2 : 1), timeout_ns);
}
// The display link's rate: 120 Hz (DM_PRESENT_LINK=2), 60, or 30 in power-saving mode
int mtl_link_rate(void) { return g_link_mode == 3 ? 30 : g_link_mode == 2 ? 120 : 60; }
// The same wait counted in the display link's own ticks: one per frame for a frame rate as high as the link's (present_interval display at DM_PRESENT_HZ=120 on a 120 Hz link)
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    static uint64_t started_tick;   // Display-link tick number when the previous frame started work (only the game's present thread calls this function)
    const uint64_t step = ticks < 1 ? 1 : (uint64_t)ticks;
    pthread_mutex_lock(&g_link_mu);
    if (g_link_tick_n >= started_tick + step) { started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu); return 1; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < started_tick + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    started_tick = g_link_tick_n;
    pthread_mutex_unlock(&g_link_mu); return 1;
}

// The display's vsync for vi::GetDisplayVsyncEvent (hle_sdk16.c): returns 1 at the first display-link tick of the next 60 Hz vsync after *seen (the tick count it last returned, updated),
// 0 when the link does not run, runs at 30 Hz (power saving: the 60 Hz event cannot come from it) or gives no tick within the timeout; the caller then keeps its own clock
int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) {
    if (!mtl_link_active() || g_link_mode == 3) return 0;
    const uint64_t step = g_link_mode == 2 ? 2 : 1;
    pthread_mutex_lock(&g_link_mu);
    if (*seen > g_link_tick_n || g_link_tick_n > *seen + 4 * step) *seen = g_link_tick_n;   // first call, or far behind (a stall): from the current tick on, no burst
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < *seen + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    *seen += step;
    pthread_mutex_unlock(&g_link_mu); return 1;
}

static id<CAMetalDrawable> g_prefetched_drawable; // Used for compositing straight onto the drawable
id<CAMetalDrawable> mtl_present_take_prefetched(void) { return g_prefetched_drawable; }

