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
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("[RED] present crop out of bounds: %d,%d %dx%d / source texture %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("present crop: source texture %lux%lu, valid %lu,%lu %lux%lu → target %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
    present_pipeline(dst.pixelFormat);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    // Keep the game picture's aspect ratio and center it, leaving black bars in the rest (no stretching when the screen ratio differs from the game's, e.g. iPhone about 19.5:9 versus the game's 16:9);
    // when the ratio difference is < 1% (the remainder from the game width being a multiple of 8 in the full-screen setup, e.g. 1952x900 versus 2868x1320 differs by 0.17%) fill directly, leaving no few-pixel border
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
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
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i) {
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("[RED] present crop out of bounds: %d,%d %dx%d / source texture %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { MLOG("[RED] failed to create the M4 present render encoder"); return 0; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en setArgumentTable:g_present_args atStages:MTLRenderStageFragment];
    [g_present_args setTexture:present_raw_src(src).gpuResourceID atIndex:0];
    [g_present_args setSamplerState:g_blit_samp.gpuResourceID atIndex:0];
    float *uv = g_present_uv[uv_i].contents;
    uv[0] = (float)cx / src.width; uv[1] = (float)cy / src.height; uv[2] = (float)cw / src.width; uv[3] = (float)ch / src.height;
    uv[4] = (cx + 0.5f) / src.width; uv[5] = (cy + 0.5f) / src.height; uv[6] = (cx + cw - 0.5f) / src.width; uv[7] = (cy + ch - 0.5f) / src.height;
    if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    [g_present_args setAddress:g_present_uv[uv_i].gpuAddress atIndex:0];
    [en setViewport:vp]; [en setRenderPipelineState:g_blit_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    m4_end(en);
    return 1;
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
        while (g_pq_done < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
    [g_present_alloc4[alloc_i] reset];
    [g_q4 waitForDrawable:dr];
    id<MTL4CommandBuffer> cb = g_present_cb[alloc_i];
    [cb beginCommandBufferWithAllocator:g_present_alloc4[alloc_i]];
    if (!present_encode_m4(cb, src, dr.texture, crop, alloc_i)) { [cb endCommandBuffer]; [g_q4 signalDrawable:dr]; return 0; }
    [cb endCommandBuffer];
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) MLOG("[RED] display command buffer error: %s", f.error.description.UTF8String);
        pthread_mutex_lock(&g_pq_mu); if (ticket > g_pq_done) g_pq_done = ticket; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
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
                 (unsigned long long)g_link_tick_n, (unsigned long long)g_present_n, (unsigned long long)(g_link_tick_n - g_present_n), (unsigned long long)g_link_held,
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
typedef struct { tex_t t; double period; uint64_t frame; int crop[4]; double t_in; uint64_t ev_val; } pq_item;   // t_in: the time the game handed this frame into the present queue (CACurrentMediaTime); ev_val: see g_ext_ev
// External producer (the Vulkan layer, dm/gfx/vk) renders on its own queue and signals this event when a frame's rendering is done: the present of that frame waits for the value on g_q4
static id<MTLSharedEvent> g_ext_ev;
static void ext_wait(uint64_t v) { if (v && g_ext_ev) [g_q4 waitForEvent:g_ext_ev value:v]; }
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
// The display link watchdog only recognizes the "active" state: when the App loses active status (pulling down Control Center / notifications, incoming call, system popup) and in the background the system does not call back anyway, and the picture is covered too -- the frame-drop fallback stays as before,
// but it is not counted as stuck and no failure is reported; the stall is measured from the later of "the most recent callback / the most recent activation" (A16 logs: 8–17 seconds inactive and 17 seconds in the background were both reported as "stuck in foreground")
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // The host's hle_oe.c provides the strong definition; when graphics unit tests do not link the host it is treated as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
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
    pthread_mutex_lock(&g_link_mu);
    g_link_tick_ns = now_ns();
    g_link_tick_n++;
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    if (!g_dm_present_link || g_pq_read == g_pq_in) { pthread_mutex_unlock(&g_pq_mu); return; }
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
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    int submitted;
    ext_wait(it.ev_val);
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
static int link_on(void) { return g_dm_present_link && g_link != nil; }
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
    link.preferredFrameLatency = g_present_latency;
    link.delegate = delegate;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    CFRunLoopRun();   // When switching back to mode 0 the display link object is invalidated and this thread exits (nextDrawable cannot get a drawable while the layer still has the display link object attached, so pausing alone is not enough)
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // Called every frame by the game's present thread: aligns the display link object's state to g_dm_present_link and the power-saving mode (can change at run time)
    static int applied; int want = g_dm_present_link;
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; return; }
    if (want && mtl_fps_target() == 30) want = 3;   // Power-saving mode: display link at 30 Hz
    if (want) front_screen_match(want == 3 ? 30 : want == 2 ? 120 : 60);
    // The screen's rate changed (switched by front_screen_match, or another screen): the range again, virtual ticks or none. Taken only once the link object exists, so a change
    // while its thread starts (after link_main read the rate) is not lost
    static int hz_was; int hz = front_screen_max_fps(); CAMetalDisplayLink *lk = g_link;
    if (lk && hz != hz_was) { hz_was = hz; if (want && want == applied) { lk.preferredFrameRateRange = link_range(want); MLOG("display link: the screen runs at %d Hz now, range set again (frame %llu)", hz, (unsigned long long)atomic_load(&g_cur_frame)); } }
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
        ext_wait(it.ev_val);
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
// Present snapshots (Mac, DM_PRESENT_SNAP=1). A game with two swapchain textures (Breath of the Wild) gets one back only when the display link has taken its previous present on a
// tick, so the next frame's CPU work starts on the tick and the GPU idles from the end of one frame to the first work of the next. macOS reads that idle time as room and holds the GPU
// at its lowest clock (444 MHz on an M2 Max) even while frames miss the screen, and there 12 ms of GPU work a frame plus the time to the frame's first submission often missed 16.7 ms.
// With snapshots each present is copied on the game's queue, right after the frame's work, into a texture of the host's own (a ring of SNAP_N), the display link shows the copy, and
// the game's texture is free at once: the game runs up to a frame ahead of the screen, and a frame whose GPU work runs long is absorbed instead of shown late. A ring slot is reused
// once its present has been processed; the presenting thread's wait for it paces the game (mtl_snap_take_wait: the frame step counts it as waiting, not as the frame's cost)
#define SNAP_N 2
static struct { id<MTLTexture> tex; uint64_t serial, ticket, ev; } g_snap[SNAP_N];
static id<MTL4CommandAllocator> g_snap_alloc[SNAP_N]; static id<MTL4CommandBuffer> g_snap_cb[SNAP_N];
static id<MTLResidencySet> g_snap_res; static id<MTLSharedEvent> g_snap_ev; static uint64_t g_snap_v, g_snap_next;
static _Atomic uint64_t g_snap_wait_ns;
int mtl_snap_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_PRESENT_SNAP"); v = e && atoi(e) > 0; if (v) MLOG("present snapshots (DM_PRESENT_SNAP=1): each present is copied into a host texture and the game's texture is free at once"); } return v; }
uint64_t mtl_snap_take_wait(void) { return atomic_exchange(&g_snap_wait_ns, 0); }
// Copy the presented texture into the next ring slot; returns the slot's serial (0 = no snapshot: the game's texture is presented as before)
static uint64_t snap_take(const tex_t *t) {
    // With the encoding thread (DM_NVN_ENC_THREAD, the default on iOS) the frame's work may still sit in its record queue when another thread presents (Breath of the Wild's
    // Presentation Thread): a copy committed now would run before it, and a GPU wait for the frame's fence would hold the queue in front of that very commit. Such a frame is presented as before
    if (rec_on() && !on_sub_thread()) { static int once; if (!once++) MLOG("present snapshots: off for presents from another thread while Metal encoding runs on its own thread (DM_NVN_ENC_THREAD)"); return 0; }
    id<MTLTexture> src = tex_mtl(t); if (!src) return 0;
    if (!g_snap_ev) {
        g_snap_ev = [g_dev newSharedEvent];
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new]; rd.initialCapacity = SNAP_N; NSError *e = nil;
        g_snap_res = [g_dev newResidencySetWithDescriptor:rd error:&e]; if (!g_snap_res) MDIE("present snapshots: no residency set: %s", e.localizedDescription.UTF8String);
        [g_q4 addResidencySet:g_snap_res];
        for (int i = 0; i < SNAP_N; i++) {
            g_snap_alloc[i] = [g_dev newCommandAllocator]; g_snap_cb[i] = [g_dev newCommandBuffer];
            if (!g_snap_alloc[i] || !g_snap_cb[i]) MDIE("present snapshots: no command allocator / buffer");
        }
    }
    int k = (int)(g_snap_next++ % SNAP_N);
    uint64_t w0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (g_snap[k].ticket) mtl_present_wait(g_snap[k].ticket);   // its last present has been processed: the display's blit from it is on the queue before this copy
    if (g_snap[k].ev && ![g_snap_ev waitUntilSignaledValue:g_snap[k].ev timeoutMS:2000]) RED_ONCE(111, "present snapshots: the previous copy into a ring slot did not finish within 2 s");
    atomic_fetch_add(&g_snap_wait_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - w0);
    id<MTLTexture> dst = g_snap[k].tex;
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height mipmapped:NO];
        d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
        if (dst) [g_snap_res removeAllocation:dst];
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
    [ce copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [ce endEncoding]; [cb endCommandBuffer];
    if (on_sub_thread()) rec_drain();   // after everything the encoding thread still holds of the frame (presents from the submitting thread)
    [g_q4 commit:&cb count:1];
    g_snap[k].ev = ++g_snap_v; [g_q4 signalEvent:g_snap_ev value:g_snap[k].ev];
    return g_snap[k].serial;
}
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t ev_val) {
    if (g_classic || atomic_load(&g_present_discard)) return 0;   // Ticket number 0 = no pending present; the swapchain texture acquire does not wait (g_classic: no Metal 4 queue to present on)
    if (t) { xv_check(t->serial, 0, 0, "present"); if (g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = t->serial; }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // Test switch DM_TEST_LINK_STALL=start seconds,duration seconds: counting from the first present, pause the display link at that point (imitating the display link not calling back while in the foreground) and release it when the duration is up. For verifying the fallback; not set in the product
        static double t0, at = -1, dur; static int st; double now = CACurrentMediaTime();
        if (at < 0) { at = 0; const char *e = getenv("DM_TEST_LINK_STALL"); if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0; }
        if (st == 1 && link_on() && now - t0 >= at) { st = 2; mtl_link_pause(1); MLOG("DM_TEST_LINK_STALL: pausing the display link for %.1f s (frame %llu)", dur, (unsigned long long)atomic_load(&g_cur_frame)); }
        else if (st == 2 && now - t0 >= at + dur) { st = 3; mtl_link_pause(0); MLOG("DM_TEST_LINK_STALL: resuming the display link (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame)); }
    }
    tex_t snap; uint64_t ss = 0; int slot = (int)(g_snap_next % SNAP_N);
    if (t && !ev_val && mtl_snap_on() && (ss = snap_take(t))) { memset(&snap, 0, sizeof snap); snap.magic = MAGIC('T', 'E', 'X', 0); snap.serial = ss; t = &snap; }
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // Wait only when the queue is full; normally AcquireTexture blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime(), ev_val};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);   // The current frame's state is enqueued with the ticket number, rather than reading the mutable window late on the present thread
    uint64_t tk = ++g_pq_in;
    if (ss) g_snap[slot].ticket = tk;
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
        if (active && !g_stall_logged && now - from >= STALL_NS) { g_stall_logged = 1;
            MLOG("display link silent for %.0f ms (active): dropping %llu queued frames not yet displayed, the game continues (stall #%llu; back to normal when the link returns)", (now - from) / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n); }
        if (active && !g_stall_red && now - from >= STALL_RED_NS) { g_stall_red = 1; MLOG("[RED] display link silent for %.1f s while active; presenting relies entirely on fallback frame drops, the picture is frozen", (now - from) / 1e9); }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
// Wait until all queued presents have been processed (used by mtl_finish = nvnQueueFinish etc.): goes through mtl_present_wait, so there is still a fallback when the display link stops (real device: the game called QueueFinish once while the display link was stopped,
// and this used to wait here without a timeout; nobody took the 3 queued frames and the game stalled; those two runs on Mac did not hit this call)
void mtl_present_flush(void) { pthread_mutex_lock(&g_pq_mu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pq_mu); mtl_present_wait(h); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip) {
    *resolve = g_vbuf_resolve; *reuse = g_vbuf_reuse; *set = g_vbuf_set; *skip = g_vbuf_skip;
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
    static uint64_t started_tick;   // Display-link tick number when the previous frame started work (only the game's present thread calls this function)
    // How many display-link ticks one frame takes: the game's present interval (vsyncs, 60 Hz) times 1 with a 60 Hz display link, times 2 with a 120 Hz one, halved and rounded up at 30 Hz.
    // A frame presented with interval N blocks the game for N vsyncs on the Switch, as the display callback holds it N ticks: games that scale their time step by the interval (BotW's VFR: interval / 2)
    // ran at double speed while they produced interval-2 frames every 60 Hz tick (iPhone log: Link sprinting with B, the interval flapping 1 ↔ 2)
    if (vsyncs < 1) vsyncs = 1;
    const uint64_t step = g_link_mode == 3 ? (uint64_t)(vsyncs + 1) / 2 : (uint64_t)vsyncs * (g_link_mode == 2 ? 2 : 1);
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

