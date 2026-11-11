// Optional profile contract for a final spatial stage. No game identity lives here.
// Unsupported bindings or allocations return before encoding any output write.
static _Atomic uint64_t g_up_recipe_drawn, g_up_recipe_kept;
static void up_recipe_report(void) {
    MLOG("experimental spatial stage totals: intercepted=%llu kept_native=%llu", (unsigned long long)atomic_load(&g_up_recipe_drawn), (unsigned long long)atomic_load(&g_up_recipe_kept));
}
typedef struct { char key[19]; unsigned iw, ih, ow, oh, ti, oi, gx, gy, gz; } up_recipe;
static const up_recipe *up_recipe_profile(void) {
    static up_recipe r; static int read, valid;
    if (!read) {
        read = 1;
        char mode[24], tail; const char *s = DM_GAME_UPSCALER_RECIPE;
        valid = sscanf(s, "%18[^,],%23[^,],%u,%u,%u,%u,%u,%u,%u,%u,%u%c", r.key, mode, &r.iw, &r.ih, &r.ow, &r.oh, &r.ti, &r.oi, &r.gx, &r.gy, &r.gz, &tail) == 11 && !strcmp(mode, "sqrt_linear");
    }
    static int report_registered;
    const char *experimental = getenv("DM_UPSCALER_EXPERIMENTAL"), *upscaler = getenv("DM_UPSCALER");
    if (valid && !report_registered && experimental && !strcmp(experimental,"1") && upscaler && !strcmp(upscaler,"metalfx")) { atexit(up_recipe_report); report_registered=1; }
    return valid ? &r : NULL;
}
static int up_recipe_named(NvShader *cs) {
    const char *v = getenv("DM_UPSCALER_EXPERIMENTAL");
    const up_recipe *r = up_recipe_profile();
    return r && v && !strcmp(v, "1") && !strcmp(cs->key, r->key);
}
static id<MTLTexture> up_recipe_root(id<MTLTexture> t) { id<MTLTexture> root = t; while (root.parentTexture) root = root.parentTexture; return root; }
static int up_recipe_valid(const up_recipe *r, id<MTLTexture> in, id<MTLTexture> out) {
    if (!r || !in || !out || up_recipe_root(in) == up_recipe_root(out)) return 0;
    // Distinct views on one buffer or heap may alias; refuse conservatively.
    if ((in.buffer && in.buffer == out.buffer) || (in.heap && in.heap == out.heap)) return 0;
    return in.textureType == MTLTextureType2D && out.textureType == MTLTextureType2D && in.sampleCount == 1 && out.sampleCount == 1 &&
        in.pixelFormat == MTLPixelFormatRGB9E5Float && out.pixelFormat == MTLPixelFormatRG11B10Float &&
        in.width == r->iw && in.height == r->ih && out.width == r->ow && out.height == r->oh &&
        (in.usage & MTLTextureUsageShaderRead) && (out.usage & MTLTextureUsageShaderWrite) &&
        in.storageMode != MTLStorageModeMemoryless && out.storageMode != MTLStorageModeMemoryless;
}
static id<MTLComputePipelineState> g_up_recipe_decode, g_up_recipe_encode;
static id<MTL4ArgumentTable> g_up_recipe_table;
static int up_recipe_pipelines(void) {
    static int failed;
    if (failed) return 0;
    if (g_up_recipe_table) return 1;
    NSError *err = nil; id<MTLLibrary> lib = host_lib("upscale_recipe", &err);
    id<MTLFunction> a = [lib newFunctionWithName:@"square_decode"], b = [lib newFunctionWithName:@"native_encode"];
    g_up_recipe_decode = a ? [g_dev newComputePipelineStateWithFunction:a error:&err] : nil;
    g_up_recipe_encode = b ? [g_dev newComputePipelineStateWithFunction:b error:&err] : nil;
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2;
    if (g_up_recipe_decode && g_up_recipe_encode) g_up_recipe_table = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&err]);
    if (!g_up_recipe_table) { failed = 1; MLOG("[RED] experimental upscaler utility unavailable: %s; keeping native dispatch", err.localizedDescription.UTF8String); }
    return g_up_recipe_table != nil;
}
static void up_recipe_compute(id<MTLComputePipelineState> ps, id<MTLTexture> in, id<MTLTexture> out) {
    id<MTL4ComputeCommandEncoder> ce = [g_cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
    [g_up_recipe_table setTexture:in.gpuResourceID atIndex:0]; [g_up_recipe_table setTexture:out.gpuResourceID atIndex:1];
    [ce setArgumentTable:g_up_recipe_table]; [ce setComputePipelineState:ps];
    [ce dispatchThreadgroups:MTLSizeMake((out.width+15)/16, (out.height+15)/16, 1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
}
static int up_recipe_draw(const up_recipe *r, id<MTLTexture> in, id<MTLTexture> out) {
    if (!up_recipe_valid(r, in, out) || !up_recipe_pipelines()) return 0;
    up_mfx_scaler *e = up_mfx_scaler_for(r->iw, r->ih, r->ow, r->oh, 2);
    if (!e) return 0;
    // Capture strong object values, never the mutable cache entry, for deferred encoding.
    id<MTL4FXSpatialScaler> sc = e->sc; id<MTLTexture> color = e->in, result = e->out;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; } ubd_land_compute();
    up_recipe_compute(g_up_recipe_decode, in, color);
    NSUInteger iw = r->iw, ih = r->ih;
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        sc.colorTexture = color; sc.outputTexture = result; sc.inputContentWidth = iw; sc.inputContentHeight = ih;
        [sc encodeToCommandBuffer:cb];
    });
    up_recipe_compute(g_up_recipe_encode, result, out);
    // MTL4 command buffers do not retain resources; image views and scaler survive the GPU fence.
    defer_obj_unref(in); defer_obj_unref(out); defer_obj_unref(sc);
    g_last_end_relaxed = 0; args_dirty_all();
    return 1;
}
static int up_recipe_encode(NvShader *cs, const p_dispatch *dp) {
    const up_recipe *r = up_recipe_profile();
    if (!r || !dp || dp->ind || dp->x != r->gx || dp->y != r->gy || dp->z != r->gz) { atomic_fetch_add(&g_up_recipe_kept,1); g_up_mfx_kept++; return 0; }
    int ti = -1, oi = -1, nt = 0, no = 0;
    for (int i = 0; i < cs->rf.nb; i++) {
        const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind == NVN_RB_TEX) { nt++; if (b->aid == r->ti) ti = i; }
        if (b->kind == NVN_RB_IMG) { no++; if (b->aid == r->oi && b->rw && !b->atom && b->reg < 32) oi = i; }
        if (b->kind == NVN_RB_SBUFP && b->rw) { atomic_fetch_add(&g_up_recipe_kept,1); g_up_mfx_kept++; return 0; }
    }
    if (nt != 1 || no != 1 || ti < 0 || oi < 0) { atomic_fetch_add(&g_up_recipe_kept,1); g_up_mfx_kept++; return 0; }
    // c24 is S.push[1]: origin and inclusive last texel, separately for input/output.
    uint32_t want[8] = {0,0,r->iw-1,r->ih-1,0,0,r->ow-1,r->oh-1};
    if (memcmp(S.push[1], want, sizeof want)) { atomic_fetch_add(&g_up_recipe_kept,1); g_up_mfx_kept++; return 0; }
    int tin = (int)(tex_slot_h(S.tex[5], S.img[5], cs->rf.b[ti].reg) & 0xfffff), tout = (int)(S.img[5][cs->rf.b[oi].reg] & 0xfffff);
    id<MTLTexture> in = nil, out = nil; tex_t src = {0}, dst = {0}; int real = 0; uint64_t rid = 0;
    os_unfair_lock_lock(&g_rmu);
    int ok = tin > 0 && tin < g_ntreg && tout > 0 && tout < g_ntreg && tin != tout && g_treg[tin].ok && g_treg[tout].ok && !g_treg[tin].gone && !g_treg[tout].gone;
    if (ok) { src = g_treg[tin].t; dst = g_treg[tout].t; in = tex_by_id(tin); out = img_binding(tout, &cs->rf.b[oi], &rid, &real); }
    os_unfair_lock_unlock(&g_rmu);
    uintptr_t lo = (uintptr_t)src.mem, hi = (uintptr_t)dst.mem;
    // Refuse unmapped or possibly aliased guest storage before touching output pixels.
    if (!ok || !real || !lo || !hi || !src.L.size || !dst.L.size || lo > UINTPTR_MAX-src.L.size || hi > UINTPTR_MAX-dst.L.size ||
        (lo < hi+dst.L.size && hi < lo+src.L.size) || !nvn_host_readable(lo,src.L.size) || !nvn_host_readable(hi,dst.L.size) ||
        nvn_sparse_alias(lo,lo+src.L.size) || nvn_sparse_alias(hi,hi+dst.L.size) || !up_recipe_draw(r,in,out)) {
        static int refused; if (!refused++) MLOG("experimental upscaler: native fallback (mapped=%d, real image=%d, input/output %p/%p, sparse alias=%d/%d); contract or utility unavailable",ok,real,(__bridge void *)in,(__bridge void *)out,lo?nvn_sparse_alias(lo,lo+src.L.size):0,hi?nvn_sparse_alias(hi,hi+dst.L.size):0);
        atomic_fetch_add(&g_up_recipe_kept,1); g_up_mfx_kept++; return 0; }
    zskip_del(dst.serial); sparse_gpu_wrote(&dst);
    uint64_t drawn = atomic_fetch_add(&g_up_recipe_drawn,1)+1;
    if (drawn%300==0) up_recipe_report();
    g_up_mfx_n++; static int logged; if (!logged++) MLOG("upscaler: experimental profile stage %s intercepted, %ux%u RGB9E5 sqrt(linear) -> square -> MetalFX Linear -> %ux%u RG11B10; preceding native AA preserved", cs->key,r->iw,r->ih,r->ow,r->oh);
    return 1;
}
