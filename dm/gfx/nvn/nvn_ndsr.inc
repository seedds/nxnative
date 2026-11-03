// Optional replacement of reviewed agl NDSR spatial kernels. Unknown kernels,
// partial dispatches and unsupported resources always run the original shader.
typedef struct {
    id<MTLFXSpatialScaler> scaler;
    id<MTLTexture> input, output;
    NSUInteger iw, ih, ow, oh;
} ndsr_fx;
static ndsr_fx g_ndsr_fx[4];
static unsigned g_ndsr_next;
static id<MTLSharedEvent> g_ndsr_event;
static uint64_t g_ndsr_event_value;
static void sub_cut_(int force);
static uint64_t g_ndsr_calls;
static uint64_t g_ndsr_keys[32]; static unsigned g_ndsr_nkeys;
static int g_ndsr_loaded;
// DM_NVN_NDSR: metalfx = MetalFX spatial HDR (copies in and out, on its own command buffer); bilinear = one bilinear draw from the input region into the output region
// in the replay's own command stream (copy_t2t_draw): the cheapest, and softer than NDSR. Unset = the game's NDSR kernel
static int ndsr_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_NVN_NDSR"); on = !e ? 0 : !strcmp(e, "metalfx") ? 1 : !strcmp(e, "bilinear") ? 2 : 0;
        if (on) MLOG("NDSR: reviewed 2x kernels replaced by %s (DM_NVN_NDSR)", on == 1 ? "MetalFX spatial" : "a bilinear draw"); }
    return on;
}
static int ndsr_known(const char *key) {
    if (!g_ndsr_loaded) {
        g_ndsr_loaded = 1;
        NSString *path = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"ndsr_spatial.tsv"];
        NSString *data = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        if ([data hasPrefix:@"# NDSR_SPATIAL_2X_V1\n"]) for (NSString *line in [data componentsSeparatedByString:@"\n"]) {
            if (!line.length || [line hasPrefix:@"#"]) continue;
            const char *p = line.UTF8String; char *end;
            uint64_t hash = strtoull(p, &end, 16);
            if (end - p == 16 && !*end && g_ndsr_nkeys < 32) g_ndsr_keys[g_ndsr_nkeys++] = hash;
        }
    }
    if (strlen(key) < 16) return 0;
    char *end; uint64_t hash = strtoull(key, &end, 16);
    if (end - key != 16 || (*end && strcmp(end, "_b"))) return 0;
    for (unsigned i = 0; i < g_ndsr_nkeys; i++) if (hash == g_ndsr_keys[i]) return 1;
    return 0;
}
// c24 contains origin + inclusive relative last texel, not an absolute corner.
static int ndsr_region(const uint32_t v[4], NSUInteger w, NSUInteger h, NVNcopyRegion *r) {
    uint64_t rw = (uint64_t)v[2] + 1, rh = (uint64_t)v[3] + 1;
    if (v[0] > INT_MAX || v[1] > INT_MAX || rw > INT_MAX || rh > INT_MAX ||
        (uint64_t)v[0] + rw > w || (uint64_t)v[1] + rh > h) return 0;
    *r = (NVNcopyRegion){(int)v[0], (int)v[1], 0, (int)rw, (int)rh, 1}; return 1;
}
static ndsr_fx *ndsr_scaler(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh) {
    for (unsigned i = 0; i < 4; i++) { ndsr_fx *f = &g_ndsr_fx[i];
        if (f->scaler && f->iw == iw && f->ih == ih && f->ow == ow && f->oh == oh) return f;
    }
    if (![MTLFXSpatialScalerDescriptor supportsDevice:g_dev]) return NULL;
    MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
    d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh;
    // NDSR reads/writes linear HDR. RG11B10Float is converted without clipping
    // through RGBA16Float, a format supported by MetalFX's HDR spatial scaler.
    d.colorTextureFormat = d.outputTextureFormat = MTLPixelFormatRGBA16Float;
    d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModeHDR;
    id<MTLFXSpatialScaler> scaler = [d newSpatialScalerWithDevice:g_dev];
    if (!scaler) return NULL;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:iw height:ih mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = scaler.colorTextureUsage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> input = [g_dev newTextureWithDescriptor:td];
    td.width = ow; td.height = oh; td.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
    id<MTLTexture> output = [g_dev newTextureWithDescriptor:td];
    if (!input || !output) return NULL;
    ndsr_fx *f = &g_ndsr_fx[g_ndsr_next++ % 4];
    if (f->input) defer_obj_release(f->input);
    if (f->output) defer_obj_release(f->output);
    // Previously queued blocks retain their scaler and its bound resources.
    *f = (ndsr_fx){scaler, input, output, iw, ih, ow, oh};
    res_add(input); res_add(output);
    MLOG("NDSR replacement: MetalFX spatial HDR %lux%lu -> %lux%lu", (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh);
    return f;
}
static int ndsr_encode(id<MTLTexture> input, id<MTLTexture> output, const NVNcopyRegion *sr, const NVNcopyRegion *dr) {
    if (input.textureType != MTLTextureType2D || output.textureType != MTLTextureType2D ||
        input.sampleCount != 1 || output.sampleCount != 1 || input == output ||
        !(output.usage & MTLTextureUsageRenderTarget) || fmt_int_or_ds(input.pixelFormat) || fmt_int_or_ds(output.pixelFormat)) return 0;
    ndsr_fx *f = ndsr_scaler(sr->width, sr->height, dr->width, dr->height); if (!f) return 0;
    // Reserve the classic command buffer before accepting replacement. A nil
    // buffer must never leave the Metal 4 queue waiting for an unsignalled event.
    id<MTLCommandBuffer> fx = [g_q commandBuffer]; if (!fx) return 0;
    if (!g_ndsr_event) g_ndsr_event = [g_dev newSharedEvent];
    if (!g_ndsr_event) return 0;
    rp_flush(); ubd_land_compute(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    NVNcopyRegion ir = {0, 0, 0, sr->width, sr->height, 1}, or = {0, 0, 0, dr->width, dr->height, 1};
    if (!copy_t2t_draw(input, f->input, 0, 0, 0, 0, sr, &ir, 0, 0)) return 0;
    // The local Metal 4 FX implementation asserts on an unset, SDK-inaccessible
    // outputTextureBarrierStages property. Use the public command-buffer scaler
    // on the compatible queue, with GPU event ordering and no CPU/GPU wait.
    // A real segment must commit before that queue can consume the input.
    uint32_t segment = g_seg; sub_cut_(1);
    if (g_seg == segment) return 0;   // deferred/merged work cannot be split here
    id<MTLFXSpatialScaler> scaler = f->scaler; id<MTLTexture> color = f->input, result = f->output;
    NSUInteger iw = f->iw, ih = f->ih;
    id<MTLSharedEvent> event = g_ndsr_event;
    uint64_t ready = ++g_ndsr_event_value, done = ++g_ndsr_event_value, frame = atomic_load(&g_cur_frame);
    rec_signal(event, ready);
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        (void)cb;
        [fx encodeWaitForEvent:event value:ready];
        scaler.colorTexture = color; scaler.outputTexture = result;
        scaler.inputContentWidth = iw; scaler.inputContentHeight = ih;
        [scaler encodeToCommandBuffer:fx];
        [fx encodeSignalEvent:event value:done];
        gpu_tag_f(fx, frame);
        [fx addCompletedHandler:^(id<MTLCommandBuffer> b) { if (b.error) { atomic_store(&g_last_commit_error, 1); MLOG("[RED] NDSR MetalFX execution error: %s", b.error.description.UTF8String); } }];
        [fx commit];
        [g_q4 waitForEvent:event value:done];
    });
    if (!copy_t2t_draw(result, output, 0, 0, 0, 0, &or, dr, 0, 0)) return 0;
    g_ndsr_calls++; args_dirty_all(); return 1;
}
static int ndsr_dispatch(NvShader *cs, const p_dispatch *dp) {
    if (!ndsr_enabled() || !ndsr_known(cs->key) || dp->ind || dp->x <= 0 || dp->y <= 0 || dp->z != 1) return 0;
    const nvn_rbind *image = NULL; int sampled = 0;
    for (int i = 0; i < cs->rf.nb; i++) { const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind == NVN_RB_TEX && b->reg == 0) sampled++;
        if (b->kind == NVN_RB_IMG) { if (image || b->reg != 1 || b->dpf != NVN_DPF_FLOAT || b->atom) return 0; image = b; }
    }
    if (sampled != 1 || !image) return 0;
    uint32_t v[8]; memcpy(v, S.push[1], sizeof v);
    rp_flush(); ubd_land_compute(); image_prepare(prog_get(S.cprog), 0x20);
    int input_id = (int)(S.tex[5][0] & 0xfffff), output_id = (int)(S.img[5][1] & 0xfffff), real = 0;
    uint64_t rid; os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> input = input_id ? tex_by_id(input_id) : nil, output = img_binding(output_id, image, &rid, &real);
    os_unfair_lock_unlock(&g_rmu);
    NVNcopyRegion sr, dr;
    if (!input || !real || !ndsr_region(v, input.width, input.height, &sr) || !ndsr_region(v + 4, output.width, output.height, &dr)) return 0;
    // Reviewed 2x kernel: each workgroup consumes 24x30 and writes 48x60.
    if (dr.width != 2 * (int64_t)sr.width || dr.height != 2 * (int64_t)sr.height || dp->x != (sr.width + 23) / 24 || dp->y != (sr.height + 29) / 30) return 0;
    // A descriptor may name one mip/slice of a larger image. Never upgrade that
    // view into a standalone texture: unsupported output usage keeps NDSR.
    if (!(output.usage & MTLTextureUsageRenderTarget)) return 0;
    if (ndsr_enabled() == 2) {
        if (input == output || !copy_t2t_draw(input, output, 0, 0, 0, 0, &sr, &dr, 1, 0)) return 0;
        g_ndsr_calls++; static int once; if (!once++) MLOG("NDSR replacement: bilinear draw %dx%d -> %dx%d", sr.width, sr.height, dr.width, dr.height);
        PD("X %llu NDSR replaced by a bilinear draw: %dx%d -> %dx%d", g_stat_pass, sr.width, sr.height, dr.width, dr.height);
        return 1;
    }
    if (!ndsr_encode(input, output, &sr, &dr)) return 0;
    PD("X %llu NDSR replaced by MetalFX spatial HDR: %dx%d -> %dx%d", g_stat_pass, sr.width, sr.height, dr.width, dr.height);
    return 1;
}
