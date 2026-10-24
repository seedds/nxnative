// nvn_mtl.m, part: args
// Per-draw support: depth-stencil states, dummy resources, scratch buffers, argument encoding (encode_args) and the quad / fan index buffers.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
static id<MTLDepthStencilState> dss_get(int has_depth, int has_stencil) {
    struct { ds_st ds; uint32_t vm[2], wm[2]; int hd, hs; } k;
    memset(&k, 0, sizeof k); k.ds = S.ds; memcpy(k.vm, S.st_vmask, 8); memcpy(k.wm, S.st_wmask, 8); k.hd = has_depth; k.hs = has_stencil;
    if (!g_dss.klen) g_dss.klen = sizeof k;
    id<MTLDepthStencilState> s = hm_get_id(&g_dss, &k);
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (has_depth && S.ds.dt) { d.depthCompareFunction = (MTLCompareFunction)(S.ds.df - 1); d.depthWriteEnabled = S.ds.dw; }
    if (has_stencil && S.ds.st) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_f - 1); f.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_f - 1);
        f.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_f - 1); f.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_f - 1);
        f.readMask = S.st_vmask[0]; f.writeMask = S.st_wmask[0];
        b.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_b - 1); b.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_b - 1);
        b.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_b - 1); b.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_b - 1);
        b.readMask = S.st_vmask[1]; b.writeMask = S.st_wmask[1];
        d.frontFaceStencil = f; d.backFaceStencil = b;
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    hm_put_id(&g_dss, &k, s);
    return s;
}
// Argument buffer: filled item by item per the B reflection (uniform slot r, separate texture / sampler slot r, storage buffer slot r, driver segments c0 / c2, c1, support_buffer)
// Unbound resources read 0: give all-zero dummy resources (NVN descriptors 0..255 are reserved for the driver; the game binding handle 0 means 'this slot is empty')
static uint64_t g_dummy_img_rid[8][4], g_dummy_tbuf_rid[4];
static id<MTLTexture> dummy_tex(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    MTLTextureType tt = (MTLTextureType)dtt;
    MTLPixelFormat pf = dpf == NVN_DPF_DEPTH ? MTLPixelFormatDepth32Float : dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> t = cache[dtt & 7][dpf & 3];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = pf; d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    t = [g_dev newTextureWithDescriptor:d];   // Shared storage is all zeros when newly created
    if (pf != MTLPixelFormatDepth32Float) { uint8_t z[64] = {0}; NSUInteger faces = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1;
        for (NSUInteger f = 0; f < faces; f++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16]; }
    cache[dtt & 7][dpf & 3] = t; g_dummy_tex_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t);
    return t;
}
// Write sink for unbound image slots: kept separate from the dummy textures that read 0 (writes must not pollute them); one is shared per same type and same pixel class
static id<MTLTexture> dummy_img(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    id<MTLTexture> t = cache[dtt & 7][dpf & 3]; if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)dtt; d.pixelFormat = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
    d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [g_dev newTextureWithDescriptor:d]; cache[dtt & 7][dpf & 3] = t; g_dummy_img_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t); return t;
}
static id<MTLBuffer> dummy_buf(void);
static id<MTLTexture> dummy_tbuf(int dpf) {   // Unbound typed buffer: all zeros (pixel class matches the shader's texture_buffer<T>)
    static __strong id<MTLTexture> cache[4];
    id<MTLTexture> t = cache[dpf & 3]; if (t) return t;
    MTLPixelFormat pf = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    id<MTLBuffer> b = dummy_buf();
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:b.length / 16 resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:b.length];
    cache[dpf & 3] = t; g_dummy_tbuf_rid[dpf & 3] = t.gpuResourceID._impl; return t;
}
static id<MTLSamplerState> dummy_samp(void) {
    static id<MTLSamplerState> s; if (!s) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES; s = [g_dev newSamplerStateWithDescriptor:d]; }
    return s;
}
static uint64_t dummy_samp_rid(void) { static uint64_t rid; if (!rid) rid = dummy_samp().gpuResourceID._impl; return rid; }
static id<MTLBuffer> dummy_buf(void) { static id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t dummy_buf_gpu(void) { static uint64_t gpu; if (!gpu) gpu = dummy_buf().gpuAddress; return gpu; }
// Image atomics that are not followed into the image's storage (a block-linear image, or storage in no memory pool) go to a scratch buffer at least as large as the image. SPIRV-Cross indexes
// them by the image bound in the same slot (spvImage2DAtomicCoord: x + y × its width), so in the 64 KB dummy buffer they ran up to the image's size past its end, into whatever GPU memory followed:
// Pokémon Legends: Z-A's atomic max into an r32ui 2D array (texture ID 302, fragment shader 2bd5037003980716) wrote into zero-copy pools over the game's heap, which broke pointers in the game's
// objects and in the host's program objects, and into unmapped addresses, a GPU page fault (GitHub issue #101). The results are discarded as before; the buffer only grows
static __strong id<MTLBuffer> g_atom_scratch;
static id<MTLBuffer> atom_scratch(const tex_t *t) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint64_t layers = t->d.target == 8 ? 6 : t->d.depth > 1 ? t->d.depth : 1;   // 3D, 2D array and cube array: depth (a cube array's counts layer-faces, tex_desc)
    uint64_t need = (uint64_t)MAX(t->d.width, 1u) * MAX(t->d.height, 1u) * layers * (uint64_t)MAX(bpb, 4);
    if (need < t->L.size) need = t->L.size;
    if (g_atom_scratch.length >= need) return g_atom_scratch;
    uint64_t len = (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    id<MTLBuffer> b = [g_dev newBufferWithLength:len options:MTLResourceStorageModePrivate];
    if (!b) { static int once; if (!once++) MLOG("[RED] image atomics: cannot create a %llu MB scratch buffer, they go to the 64 KB dummy buffer", (unsigned long long)(len >> 20)); return nil; }
    res_add(b); if (g_atom_scratch) defer_obj_release(g_atom_scratch); g_atom_scratch = b;
    MLOG("image atomics that are not followed: scratch buffer of %llu MB (texture %ux%u, %llu layers)", (unsigned long long)(len >> 20), t->d.width, t->d.height, (unsigned long long)layers);
    return b;
}
// Snapshot substitution for same-pass feedback sampling (registered by fb_prepare, used by encode_args; see the notes before draw)
static uint64_t g_draw_seq, g_fb_seq, g_fb_count; static int g_fb_n, g_fb_rebind;
static __unsafe_unretained id<MTLTexture> g_fb_src[4]; static __strong id<MTLTexture> g_fb_use[4]; static uint64_t g_fb_rid[4];
static uint8_t g_args_dirty[6] = {1, 1, 1, 1, 1, 1};
static uint64_t g_args_reg_seen, g_args_dirty_seen;
static inline void args_dirty_all(void) { memset(g_args_dirty, 1, sizeof g_args_dirty); }
static inline void args_sync_dirty(void) {
    uint64_t reg = g_reg_gen, dirty = nvn_dirty_now();
    if (reg != g_args_reg_seen || dirty != g_args_dirty_seen) {
        g_args_reg_seen = reg; g_args_dirty_seen = dirty; args_dirty_all();
    }
}
static void bind_args(id<MTLCommandEncoder> en, int nvn_stage, int metal_vertex, id<MTLBuffer> ab, uint64_t gpu, size_t off, NSUInteger ai) {
    if (nvn_stage == 5 || nvn_stage == 2) enc_c_args((id<MTLComputeCommandEncoder>)en, gpu, ai);
    else enc_r_args((id<MTLRenderCommandEncoder>)en, metal_vertex ? MTLRenderStageVertex : MTLRenderStageFragment, gpu, ai);
}
// Static constant buffers of route B: the Ryujinx support_buffer (layout in Ryujinx.Graphics.Shader/SupportBuffer.cs: alpha_test u32, is_bgra u32[8],
// viewport_inverse f4, viewport_size f4, frag_scale_count i32, render_scale f32[73]..., each field aligned to 16 bytes) and the all-0 driver constant segment (64 KB = the upper bound declared by shaders)
static id<MTLBuffer> support_buf(void) {
    static __strong id<MTLBuffer> b; if (b) return b;
    b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b);   // All 0 when newly created
    const int render_scale_off = 192;   // Measured MSL layout: alpha_test 0, is_bgra[8] 16, viewport_inverse 144, viewport_size 160, frag_scale_count 176, render_scale float4[73] 192 (take .x)
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)b.contents + render_scale_off + 16 * i) = 1.0f;
    return b;
}
static uint64_t support_gpu(void) { static uint64_t gpu; if (!gpu) gpu = support_buf().gpuAddress; return gpu; }
static id<MTLBuffer> zero_bank(void) { static __strong id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t zero_bank_gpu(void) { static uint64_t gpu; if (!gpu) gpu = zero_bank().gpuAddress; return gpu; }
// Bindless handle (TEXU / SAMPU): the 32-bit word at byte offset off of uniform slot reg = the low 32 bits of the NVN handle the game wrote there (low 20 bits texture ID, high 12 bits sampler ID).
// Read on the CPU side at replay time (the memory at the same moment a real GPU would read it at execution); slot unbound / out of range / unreadable = 0 (empty slot: dummy texture / dummy sampler).
// A location with a GPU-side write queued in this submit (pend_hits) is still read at its current value; the first case is reported red.
// The same read serves storage buffer addresses kept in uniform memory (SBUFU: 8 bytes)
static void ubo_read(int stage, int reg, int off, size_t n, void *out) {
    memset(out, 0, n);
    if (stage < 0 || stage >= 6 || reg < 0 || reg >= 16) return;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint64_t)off + n > u->size) return;
    if (g_nubd && ubd_hits(u->addr + off, u->addr + off + n) && ubd_covers(u->addr + off, u->addr + off + n)) { ubd_read(u->addr + off, n, out); return; }   // written by an inline update deferred in this pass
    { uint64_t g; const uint8_t *c; if (pend_hits(u->addr + off, n) && ubp_get(u->addr + off, u->addr + off + n, &g, &c)) { ubc_wait(g_ubp_ready); memcpy(out, c, n); return; } }   // memory and the inline updates queued over it
    if (pend_hits(u->addr + off, n)) { int kind; if (pend_read(u->addr + off, n, out, &kind)) return;   // written by an inline update queued in this submission: its data
        RED_ONCE(47, "the uniform buffer holding bindless handles (stage %d slot %d +%#x) has GPU-side writes in this submission (%s): the current value is read", stage, reg, off,
                 kind == 2 ? "a buffer copy" : kind == 3 ? "a fill" : kind == 1 ? "an inline update covering part of it" : "more writes than the host tracks"); }
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, n, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return;
    memcpy(out, base + mo, n);
}
static uint32_t ubo_handle(int stage, int reg, int off) { uint32_t h; ubo_read(stage, reg, off, 4, &h); return h; }
// A handle read from a uniform buffer names IDs of the pools bound when the draw runs: as bound handles are moved at recording (hle_nvn.c bind_handles), these take the bases here (0 = none)
static int ubo_tex_id(uint32_t h) { int t = (int)(h & 0xfffff); return t ? t + S.tex_base : 0; }
static int ubo_samp_id(uint32_t h) { int s = (int)(h >> 20); return s ? s + S.samp_base : 0; }
static uint32_t g_dispatch_dim[3];   // the current dispatch's workgroup counts (driver segment c0 + 0x200 for compute)
static uint32_t g_draw_binst;   // the current draw's base instance (draw), for c0 + 0x644
static __unsafe_unretained id<MTLBuffer> g_c0_dim_buf; static size_t g_c0_dim_off;   // where encode_args put them for this dispatch (nil: the shader reads no c0); an indirect dispatch copies its GPU-written counts there
static uint64_t g_geo_bind[5];
static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex);
static int encode_args(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) { AP_B; int r = encode_args_(en, sh, nvn_stage, metal_vertex); AP_E(3); return r; }
static int encode_args_(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) {
    if (!sh->ae) return 1;
    args_sync_dirty();
    if (!passdump() && !g_args_dirty[nvn_stage] && sh->ab_buf && sh->ab_pass == g_pass.serial && nvn_stage != 5 && !(sh->rf.drv_read[0] && sh->ab_binst != g_draw_binst)) {
        bind_args(en, nvn_stage, metal_vertex, sh->ab_buf, sh->ab_gpu, sh->ab_off, sh->rf.abi);
        g_stat_args_reuse++;
        return 1;
    }
    NSUInteger arg_len = sh->arg_len ? sh->arg_len : sh->ae.encodedLength;
    uint64_t slots[NVN_REFL_MAXARG]; memset(slots, 0, arg_len < sizeof slots ? arg_len : sizeof slots);   // only the bytes that are copied out (they used to be zeroed in full, before the reuse check too)
    #define USE(r, u) do { (void)(u); if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)
    // DM_NVN_PASSDUMP: on the first new encode per pass per stage, records the shader key (K) and the actual source of each binding (B: UBO address / length / first 4 floats, texture ID and serial, or "dummy")
    static uint64_t bl_pass[6]; static int bl_all = -1; if (bl_all < 0) bl_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;   // Record on every new encode (otherwise only the first time per pass per stage)
    int bl = passdump() && (bl_all || g_kd_force || bl_pass[nvn_stage] != g_stat_pass);
    if (bl) { bl_pass[nvn_stage] = g_stat_pass; PD("K %llu st%d key=%s nb=%d", g_stat_pass, nvn_stage, sh->key[0] ? sh->key : sh->builtin ? sh->builtin : "?", sh->rf.nb); }
    pthread_mutex_lock(&g_rmu);   // The texture / sampler registration tables may be being grown or rewritten by the game thread
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        int reg = b->reg, aid = b->aid;
        if (b->kind == NVN_RB_GEOMETRY) { slots[aid] = g_geo_bind[reg]; continue; }
        if (b->kind == NVN_RB_CBUF) {
            const p_bind_buf *u = reg < 16 ? &S.ubo[nvn_stage][reg] : NULL;
            void *mb; uint64_t mo, gpu;
            if (!u || !u->addr || !mtl_gpu_addr(u->addr, 1, &mb, &mo, &gpu)) {   // reads 0 (the all-zero bank), as an unbound texture slot does, instead of through a null pointer
                RED_ONCE(16, "stage %d cb%d has no uniform buffer bound", nvn_stage, reg); slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); continue; }
            uint64_t ul = cb_len(sh, reg, u->size);   // what the shader can read of it
            if (g_nubd && ubd_hits(u->addr, u->addr + ul)) {   // inline updates deferred in this pass (see mtl_sub_update); ubd_conflict made sure they cover the binding or the rest can be read from memory
                uint64_t g; const uint8_t *c; if (ubd_covers(u->addr, u->addr + ul)) gpu = ubd_shadow(u->addr, u->addr + ul);
                else if (ubp_get(u->addr, u->addr + ul, &g, &c)) gpu = g;
                else RED_ONCE(92, "a uniform update deferred inside the pass covers only part of the binding (stage %d cb%d): the content before the update is read", nvn_stage, reg); }
            if (bl) {
                PD("B %llu st%d cb reg%d addr=%#llx size=%llu pool=%#llx+%#llx f=%g,%g,%g,%g", g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr, (unsigned long long)u->size,
                   (unsigned long long)(u->addr - mo), (unsigned long long)mo, ((float *)u->addr)[0], ((float *)u->addr)[1], ((float *)u->addr)[2], ((float *)u->addr)[3]);
                // DM_NVN_CBDUMP=directory: also saves the whole uniform block to disk as-is (F<frame>_p<pass>_st<stage>_cb<slot>_<address>.bin), for the ground-truth side to compare byte by byte by guest address
                // DM_NVN_CBDUMP_KEY=shader key: only save for draws using that shader (any stage)
                static const char *cbd, *cbk; static int cbd_init; if (!cbd_init) { cbd = getenv("DM_NVN_CBDUMP"); cbk = getenv("DM_NVN_CBDUMP_KEY"); cbd_init = 1; }
                if (cbd && (!cbk || !strcmp(sh->key, cbk))) {
                    char p[1024]; snprintf(p, sizeof p, "%s/F%llu_p%llu_st%d_cb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr);
                    FILE *f = fopen(p, "wb"); if (f) { fwrite((const void *)u->addr, 1, u->size, f); fclose(f); } else RED_ONCE(41, "CBDUMP cannot write %s", p);
                }
            }
            slots[aid] = gpu;
            USE((__bridge id<MTLBuffer>)mb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SAMPU && b->n) {   // the samplers of an array of handles in a uniform buffer: element k = the sampler ID of the handle at byte 8k (see the textures below)
            for (int k = 0; k < b->n; k++) {   // only registered IDs are looked up: most elements are other data the shader never picks as a handle
                int sid = ubo_samp_id(ubo_handle(nvn_stage, reg, 8 * k)); if (sid >= g_nsreg || !g_sreg_ok[sid]) sid = 0;
                __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
                slots[aid + k] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
                USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_TEXU && b->n) {   // an array of handles in a uniform buffer that the shader indexes at run time (Ryujinx's texture array from a buffer): element k = the handle
            uint64_t real = 0;                         // at byte 8k, the index the shader computes from the handle's offset (an NVN handle is 64-bit)
            for (int k = 0; k < b->n; k++) {
                int tid = ubo_tex_id(ubo_handle(nvn_stage, reg, 8 * k)); if (tid >= g_ntreg || !g_treg[tid].ok) tid = 0;   // as for the samplers: other data, not a handle
                void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); t = v; trid = v ? r2 : 0; }   // a handle the shader does not pick here may be another kind
                if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; } else real |= 1ull << k;
                if (bl && tid) PD("B %llu st%d texu [%d] tid=%d %s", g_stat_pass, nvn_stage, k, tid, tv ? "real" : "dummy");
                slots[aid + k] = trid;
                USE(t, MTLResourceUsageRead);
            }
            if (!(sh->logged & 4)) { sh->logged |= 4; static int nlog; if (nlog++ < 32) MLOG("shader %s (stage %d): an array of %d handles in uniform slot %d, elements %#llx hold textures", sh->key, nvn_stage, b->n, reg, (unsigned long long)real); }
        } else if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT || b->kind == NVN_RB_SAMPU || b->kind == NVN_RB_SAMPI) {   // SAMPT: the sampler part of a combined handle (texture ID | sampler ID << 20); SAMPU: the same, with the handle in a uniform buffer
            int sid = b->kind == NVN_RB_SAMPU ? ubo_samp_id(ubo_handle(nvn_stage, reg, b->off)) :
                      b->kind == NVN_RB_SAMPI ? (int)reg : b->kind == NVN_RB_SAMPT ? (reg < NTEX ? (int)(S.tex[nvn_stage][reg] >> 20) : 0) : reg < 128 ? (int)(S.samp[nvn_stage][reg] >> 20) : 0;   // SAMPI: a sampler ID the shader names as a constant
            __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
            slots[aid] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
            USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            if (bl && sid > 0) { int ok = sid < g_nsreg && g_sreg_ok[sid]; samp_par sp = ok ? g_sreg[sid] : (samp_par){0};   // g_rmu is already held in this section
                if (ok) PD("B %llu st%d samp reg%d sid=%d mag=%d min=%d (0 nearest / 1 linear; 2–5 with mips) wrap=%d,%d,%d border=%g,%g,%g,%g lod=%g..%g bias=%g aniso=%g cmp=%d/%d %s", g_stat_pass, nvn_stage, reg, sid, (int)sp.magf, (int)sp.minf,
                           (int)sp.ws, (int)sp.wt, (int)sp.wr, sp.border[0], sp.border[1], sp.border[2], sp.border[3], sp.lod_min, sp.lod_max, sp.lod_bias, sp.aniso, (int)sp.cmp_mode, (int)sp.cmp_func, ss ? "real" : "failed"); }
        } else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) {
            int tid = b->kind == NVN_RB_TEXU ? ubo_tex_id(ubo_handle(nvn_stage, reg, b->off)) : (int)(tex_slot_h(S.tex[nvn_stage], S.img[nvn_stage], reg) & 0xfffff);
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && g_fb_n && g_fb_seq == g_draw_seq && nvn_stage == 1) for (int q = 0; q < g_fb_n; q++) if (t == g_fb_src[q]) { t = g_fb_use[q]; trid = g_fb_rid[q]; break; }   // same-pass feedback sampling: read the snapshot instead (see fb_prepare)
            if (t && passdump() && tid < g_ntreg) PD("R %llu st%d ser=%llu", g_stat_pass, nvn_stage, g_treg[tid].t.serial);
            if (bl) PD("B %llu st%d tex reg%d tid=%d %s ser=%llu fmt=%lu %lux%lux%lu view_lv=%lu+%lu view_sl=%lu array=%lu target=%lu swz=%d%d%d%d mtl=%p", g_stat_pass, nvn_stage, reg, tid, t ? "real" : "dummy", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
                       (unsigned long)(t.parentTexture ? t.parentRelativeLevel : 0), (unsigned long)t.mipmapLevelCount,
                       (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0), (unsigned long)t.arrayLength, (unsigned long)t.textureType, t ? (int)t.swizzle.red : 0, t ? (int)t.swizzle.green : 0, t ? (int)t.swizzle.blue : 0, t ? (int)t.swizzle.alpha : 0, (__bridge void *)(t.parentTexture ? t.parentTexture : t));
            if (t && b->f1d && t.textureType == MTLTextureTypeTextureBuffer && (MTLTextureType)b->dtt == MTLTextureType2D) { uint64_t r2 = 0; id<MTLTexture> v = tbuf_as_2d(tid, t, &r2); if (v) { t = v; trid = r2; } }   // see tbuf_as_2d
            if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit(tid, t, (MTLTextureType)b->dtt, &r2); if (v) { t = v; trid = r2; }
                // no view fits, and one side is a buffer texture (Octopath Traveler 0: a buffer texture left in a slot a shader reads as a cube): reads of it are undefined, so the slot reads 0
                else if (t.textureType == MTLTextureTypeTextureBuffer || (MTLTextureType)b->dtt == MTLTextureTypeTextureBuffer) { tex_mis_note(sh, nvn_stage, b->kind == NVN_RB_TEX ? reg : -1, tid, t, (MTLTextureType)b->dtt, "a buffer texture: the slot reads 0 (dummy)"); t = nil; }
                else tex_mis_note(sh, nvn_stage, b->kind == NVN_RB_TEX ? reg : -1, tid, t, (MTLTextureType)b->dtt, "bound as is"); }
            if (t && nvn_stage == 1 && afr_want(sh)) { uint64_t r2 = 0; id<MTLTexture> v = tex_alpha_red(tid, t, &r2); if (v) { t = v; trid = r2; } }   // see tex_alpha_red
            if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_IMG) {   // Writable texture: NVN image slot r (BindImages) ↔ D3D u_r
            int tid = reg < 32 ? (int)(S.img[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (passdump()) PD("W %llu st%d reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "real" : "dummy", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            int swz_only = t && !(t.usage & MTLTextureUsageShaderWrite) && (t.parentTexture.usage & MTLTextureUsageShaderWrite);   // a swizzled view of an image texture (img_fit)
            if (t && !(t.usage & MTLTextureUsageShaderWrite) && !swz_only) { RED_ONCE(32, "texture bound to an image slot lacks the shader-write usage (texture ID %d)", tid); t = nil; }
            if (t && (swz_only || (b->dpf != NVN_DPF_FLOAT && mtl_pf_dpf(t.pixelFormat) != b->dpf))) { uint64_t r2 = 0; id<MTLTexture> v = img_fit(tid, t, b->dpf, &r2); if (v) { t = v; trid = r2; } else if (swz_only) t = nil; }   // see img_fit
            if (!t) { t = dummy_img(b->dtt, b->dpf); trid = g_dummy_img_rid[b->dtt & 7][b->dpf & 3]; }   // Unbound: D3D semantics discard the write - write into a dedicated dummy texture so the set that reads 0 is not polluted
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead | MTLResourceUsageWrite);
        } else if (b->kind == NVN_RB_IMGA) {   // image atomics: the storage of the image in slot r. A buffer texture is a view of its memory pool (tex_mtl), so the texture reads and the atomics see the same words
            int tid = reg < 32 ? (int)(S.img[nvn_stage][reg] & 0xfffff) : 0;
            const tex_t *tt = tid && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid].t : NULL;
            void *mb; uint64_t mo, gpu;
            // A block-linear image is a deswizzled Metal copy whose pool memory has another layout (and is re-uploaded from): its atomics go to a scratch buffer instead (logged, atom_scratch)
            int linear = tt && (tt->d.target == 10 || (tt->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)));
            if (tt && !linear) RED_ONCE(93, "image atomics on a block-linear texture (texture ID %d target %d): not followed, the atomics go to a scratch buffer", tid, (int)tt->d.target);
            id<MTLBuffer> sb = nil;
            if (linear && tt->mem && mtl_gpu_addr((uint64_t)tt->mem, tt->L.size ? tt->L.size : 4, &mb, &mo, &gpu)) {
                slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, MTLResourceUsageRead | MTLResourceUsageWrite);
                static int nlog; if (nlog < 8 && !(sh->logged & 2)) { sh->logged |= 2; nlog++; MLOG("shader %s (stage %d): image atomics on image slot %d, texture ID %d (target %d, %llu bytes)", sh->key, nvn_stage, reg, tid, (int)tt->d.target, (unsigned long long)tt->L.size); }
            } else if (tt && (sb = atom_scratch(tt))) { slots[aid] = sb.gpuAddress; USE(sb, MTLResourceUsageRead | MTLResourceUsageWrite); }
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead | MTLResourceUsageWrite); }   // unbound: the image slot has the dummy image, so the writes stay in the 64 KB dummy buffer
            if (bl) PD("B %llu st%d imga reg%d tid=%d %s", g_stat_pass, nvn_stage, reg, tid, tt && tt->mem ? "real" : "dummy");
        } else if (b->kind == NVN_RB_TBUF) {   // Typed buffer: the buffer texture (target BUFFER) in texture slot r
            int tid = (int)(tex_slot_h(S.tex[nvn_stage], S.img[nvn_stage], reg) & 0xfffff);
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && t.textureType != MTLTextureTypeTextureBuffer) { tex_mis_note(sh, nvn_stage, reg, tid, t, MTLTextureTypeTextureBuffer, "a buffer slot: it reads 0 (dummy)"); t = nil; }
            if (bl) PD("B %llu st%d tbuf reg%d tid=%d %s ser=%llu fmt=%#x n=%u mem=%p", g_stat_pass, nvn_stage, reg, tid, t ? "real" : "dummy", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       tid < g_ntreg ? g_treg[tid].t.d.format : 0, tid < g_ntreg ? g_treg[tid].t.d.width : 0, tid < g_ntreg ? (void *)g_treg[tid].t.mem : NULL);
            if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_TEXD) {   // dynamic handle array (profile key tex_dyn_slots): element k = the texture in slot k (combined, else separate), since c2 holds slot indices for this shader (see DRV2 below)
            uint64_t real = 0;   // the elements that got a texture of the declared kind (logged on the shader's first encode: logged bit 0)
            for (int k = 0; k < b->n; k++) {
                uint64_t hk = k < 128 ? (S.tex[nvn_stage][k] ? S.tex[nvn_stage][k] : S.tex[nvn_stage][NVN_SEPTEX_SLOT0 + k]) : 0; int tid = (int)(hk & 0xfffff);
                void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (b->dtt == (uint8_t)MTLTextureTypeTextureBuffer) {
                    if (t && t.textureType != MTLTextureTypeTextureBuffer) t = nil;   // a slot the shader does not pick here may hold another kind of texture
                    if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
                } else {
                    if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); t = v; trid = v ? r2 : 0; }
                    if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; }
                }
                if (bl && tid) PD("B %llu st%d texd [%d] tid=%d %s", g_stat_pass, nvn_stage, k, tid, tv ? "real" : "dummy");
                if (tv && trid && trid != g_dummy_tbuf_rid[b->dpf & 3] && trid != g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]) real |= 1ull << k;
                slots[aid + k] = trid;
                USE(t, MTLResourceUsageRead);
            }
            if (!(sh->logged & 1)) { sh->logged |= 1; static int nlog; if (nlog++ < 32) MLOG("shader %s (stage %d): dynamic texture handles, slots %#llx of its %d-entry array hold %s", sh->key, nvn_stage,
                (unsigned long long)real, b->n, b->dtt == (uint8_t)MTLTextureTypeTextureBuffer ? "buffer textures" : "textures"); }
        } else if (b->kind == NVN_RB_TEXA) {   // an array of the driver table's handles that the shader indexes at run time (nvn_refl.h): element k = the texture of the c2 word at off + 8k
            uint64_t real = 0;
            for (int k = 0; k < b->n; k++) {
                int tid = (int)(c2_handle(nvn_stage, b->off + 8u * k) & 0xfffff);
                void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); t = v; trid = v ? r2 : 0; }   // an element the shader does not pick here may be another kind
                if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; } else real |= 1ull << k;
                if (bl && tid) PD("B %llu st%d texa [%d] tid=%d %s", g_stat_pass, nvn_stage, k, tid, tv ? "real" : "dummy");
                slots[aid + k] = trid;
                USE(t, MTLResourceUsageRead);
            }
            if (!(sh->logged & 8)) { sh->logged |= 8; static int nlog; if (nlog++ < 32) MLOG("shader %s (stage %d): an array of %d driver-table handles from c2 byte %#x, elements %#llx hold textures", sh->key, nvn_stage, b->n, b->off, (unsigned long long)real); }
        } else if (b->kind == NVN_RB_SAMPA) {   // its samplers: element k = the sampler of the combined handle at c2 byte off + 8k
            for (int k = 0; k < b->n; k++) {
                int sid = (int)(c2_handle(nvn_stage, b->off + 8u * k) >> 20);
                __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
                slots[aid + k] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
                USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_SAMPD) {   // its samplers: element k = the separate sampler in slot k, else the sampler of a combined handle in texture slot k
            for (int k = 0; k < b->n; k++) {
                int sid = k < 128 ? (int)(S.samp[nvn_stage][k] >> 20) : 0;
                if (!sid && k < NTEX) sid = (int)(S.tex[nvn_stage][k] >> 20);
                __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
                slots[aid + k] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
                USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) {   // B: storage buffers / compute-stage uniforms are device pointers; the address is taken by NVN slot
            const p_bind_buf *u = reg < 16 ? (b->kind == NVN_RB_UBOP ? &S.ubo[nvn_stage][reg] : &S.ssbo[nvn_stage][reg]) : NULL;
            void *mb; uint64_t mo, gpu;
            if (u && u->addr && mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &gpu)) { slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); if (b->rw && b->kind == NVN_RB_SBUFP && !(reg < 32 && (sh->sb_ro >> reg & 1))) sbw_note(u->addr, u->size, SBW_GPU); }   // cb_extent.tsv: slots the shader never writes
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); }
            if (bl) PD("B %llu st%d sbufp reg%d %s addr=%#llx size=%llu %s", g_stat_pass, nvn_stage, reg, u && u->addr ? "real" : "dummy", u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_SBUFU) {   // B: a storage buffer whose GPU address the game keeps in its own uniform buffer (Monster Hunter Rise's compute shaders: c3 + 0x38); read like a bindless handle
            uint64_t addr; ubo_read(nvn_stage, reg, b->off, 8, &addr);
            void *mb; uint64_t mo, gpu;
            if (addr && mtl_gpu_addr(addr, 1, &mb, &mo, &gpu)) { slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); if (b->rw) sbw_note(addr, 0, SBW_GPU); }   // size not known
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); }
            if (bl) PD("B %llu st%d sbufu reg%d+%#x addr=%#llx %s", g_stat_pass, nvn_stage, reg, b->off, (unsigned long long)addr, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_C1) {      // B: the shader's own constant segment, built when the shader is fetched
            id<MTLBuffer> cb = sh->c1buf ? sh->c1buf : zero_bank();   // c1 has length 0 (600 of them in the whole library) yet the shader still declares it: reads 0
            slots[aid] = (sh->c1buf ? sh->c1_gpu : zero_bank_gpu()) + (sh->c1buf ? sh->c1off : 0);
            USE(cb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SUPPORT) {   // support_buffer: scale 1.0, BGRA swap 0, viewport transform not disabled (viewport fields are not read)
            slots[aid] = support_gpu();
            USE(support_buf(), MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_DRV0 || b->kind == NVN_RB_DRV2) {
            // NVN driver constant segments: those proven unread by static analysis at packaging time are supplied as 0; those read are filled on the spot from the current bindings - c0 = storage buffer table (base + 0x10 r: address 64 bits, size 32 bits),
            // c2 = the handle tables (C2_*), the values being the NVN handles the game gave at bind time; positions read outside the tables are registered at packaging time and reported a failure at load time
            int bank = b->kind == NVN_RB_DRV2 ? 2 : 0;
            if (!sh->rf.drv_read[bank]) { slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); }
            else {
                id<MTLBuffer> db; size_t doff = arena_alloc(DRV_SIZE, &db); uint8_t *d = (uint8_t *)db.contents + doff; memset(d, 0, DRV_SIZE);
                if (bank == 0) {
                    int base = c0_ssbo_base(nvn_stage);
                    if (base < 0) RED_ONCE(45, "stage %d shader reads driver segment c0; the storage buffer table base was never measured", nvn_stage);
                    else for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[nvn_stage][r].addr; uint32_t n = (uint32_t)S.ssbo[nvn_stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
                    // Compute also has the dispatch size at 0x200 (x, y, z) and the uniform buffer table at 0x210 + 16 r (address 64 bits, size 32 bits), ending where the storage buffer table
                    // starts (0x310). Measured on Link's Awakening's light culling (light_culling_compute.bfsha): tile index = workgroup y × c0[0x200] + workgroup x, and c0[0x280] is
                    // the address of uniform buffer 7 (the light parameters, 144 bytes per light); Breath of the Wild's compute shaders read 0x280 / 0x290 / 0x2d0 of the same table
                    // Graphics stages: 0x610, right after the pixel stage's storage buffer table, holds the largest point size, to which GLSLC clamps a shader-written
                    // gl_PointSize (Super Mario Odyssey's agl point sprites, alPointSpriteShader: gl_PointSize = min(size attribute, c0[0x610])). It is not SetPointSize: the game
                    // sets that to 1 and sizes its sprites itself (the fog splats ask for 3-18 px). Left at 0, every sprite had size 0, so the splats that push the Cap Kingdom
                    // fog aside never reached its velocity fields and the fog did not part around Mario. 2047 is Maxwell's point size limit (Metal clamps to its own)
                    if (nvn_stage != 5) { const float max_point = 2047.0f; memcpy(d + 0x610, &max_point, 4); }
                    // Graphics stages: 0x644 holds the draw's base instance. Maxwell's instance ID leaves it out (GL's gl_InstanceID), and GLSLC adds this word where a shader needs the
                    // absolute instance (gl_InstanceID + gl_BaseInstance): Pokémon Legends: Z-A's occlusion boxes (a793f436dcabd350, one box per draw at base instance 61, 62, …) pick their
                    // transform this way, and 1,550 reads in 793 of its vertex shaders. Left at 0, every box was box 0: all six queries came back empty, and the game, taking its characters for
                    // hidden, posed their legs straight back from the hips. 0x640 (read by 58 shaders, added to the vertex ID) stays 0: what the driver puts there is not established
                    if (nvn_stage != 5) memcpy(d + 0x644, &g_draw_binst, 4);
                    if (nvn_stage == 5) {
                        memcpy(d + 0x200, g_dispatch_dim, 12); g_c0_dim_buf = db; g_c0_dim_off = doff + 0x200;
                        for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); }
                    }
                } else {
                    // A shader with dynamic handle arrays (TEXD / SAMPD, profile key tex_dyn_slots) picks a table slot at run time and ORs a texture word with a sampler word into a handle,
                    // whose texture / sampler IDs index the arrays. For it the tables hold slot indices instead of handles (texture slot r → r, and r << 20 when a combined handle carries
                    // a sampler; sampler slot r → r << 20), and the arrays hold the bound slots, so the shader's own arithmetic picks the binding (Octopath Traveler 0: separate texture slots 0-10, samplers 0-4)
                    const uint64_t *tx = S.tex[nvn_stage], *im = S.img[nvn_stage]; int dyn = sh->rf.dyn;
                    for (int r = 0; r < C2_IMG_SLOTS; r++) { uint32_t ih = (uint32_t)im[r]; memcpy(d + C2_IMG + 8 * r, &ih, 4); }
                    for (int r = 0; r < C2_TEX_SLOTS; r++) {   // a combined slot past 31 is written when it is bound and the image slot it shares is not (Hades II binds texture slot 34, no image)
                        if (r >= 32 && (!tx[r] || (r - 32 < C2_IMG_SLOTS && im[r - 32]))) continue;
                        uint32_t v = !dyn ? (uint32_t)tx[r] : !tx[r] ? 0 : (uint32_t)r | (tx[r] >> 20 ? (uint32_t)r << 20 : 0); memcpy(d + C2_TEX + 8 * r, &v, 4); }
                    for (int r = 0; r < 128; r++) { uint64_t h = tx[NVN_SEPTEX_SLOT0 + r]; uint32_t v = !dyn ? (uint32_t)h : h ? (uint32_t)r : 0; memcpy(d + C2_SEPTEX + 8 * r, &v, 4); }
                    for (int r = 0; r < 128; r++) { uint64_t h = S.samp[nvn_stage][r]; uint32_t v = !dyn ? (uint32_t)h : h ? (uint32_t)r << 20 : 0; memcpy(d + C2_SAMP + 8 * r, &v, 4); }
                }
                slots[aid] = g_cur_arena->gpu + doff;
                USE(db, MTLResourceUsageRead);
            }
        } else RED_ONCE(46, "binding kind %d not implemented", b->kind);
    }
    #undef USE
    pthread_mutex_unlock(&g_rmu);
    id<MTLBuffer> ab; size_t off = arena_alloc(arg_len, &ab);
    memcpy((uint8_t *)ab.contents + off, slots, arg_len);
    bind_args(en, nvn_stage, metal_vertex, ab, g_cur_arena->gpu + off, off, sh->rf.abi);
    sh->ab_buf = ab; sh->ab_gpu = g_cur_arena->gpu + off; sh->ab_off = off; sh->ab_pass = g_pass.serial; sh->ab_binst = g_draw_binst; g_args_dirty[nvn_stage] = 0;
    g_stat_args_enc++;
    return 1;
}
static id<MTLBuffer> g_quad_ib; static uint64_t g_quad_gpu;
static id<MTLBuffer> g_fan_ib; static uint64_t g_fan_gpu;   // non-indexed triangle fans: shared indices (0, i, i+1), base vertex = first
#define SHARED_IB_N 65536   // quads / fan triangles the shared index buffers cover
static void quad_ib_make(void) {   // Quads: shared indices 0,1,2,0,2,3
    if (g_quad_ib) return;
    uint32_t n = SHARED_IB_N; uint32_t *ix = malloc(n * 6 * 4);
    for (uint32_t q = 0; q < n; q++) { uint32_t b = q * 4; uint32_t v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + q * 6, v, 24); }
    g_quad_ib = [g_dev newBufferWithBytes:ix length:n * 24 options:MTLResourceStorageModeShared]; g_quad_gpu = g_quad_ib.gpuAddress; res_add(g_quad_ib); free(ix);
}
static void fan_ib_make(void) {
    if (g_fan_ib) return;
    uint32_t n = SHARED_IB_N; uint32_t *ix = malloc(n * 12);
    for (uint32_t i = 0; i < n; i++) { uint32_t v[3] = {0, i + 1, i + 2}; memcpy(ix + i * 3, v, 12); }
    g_fan_ib = [g_dev newBufferWithBytes:ix length:n * 12 options:MTLResourceStorageModeShared]; g_fan_gpu = g_fan_ib.gpuAddress; res_add(g_fan_ib); free(ix);
}
static id<MTL4RenderCommandEncoder> rp_begin(void);
