// Per-shader negative caches for fb_prepare, fb_depth_rule, ubd_conflict_textures, ubd_conflict_sh, and ubd_conflict (vertex-stream clean mask and UBO envelope tracking):
// verify cache population, fast-path state updates, and every invalidation trigger (mtl_reg_tex, mtl_reg_tex_gone, OP_BIND_TEX/UBO/VBUF, OP_SET_TEX_POOL, target change,
// mtl_sub_update, ubd_flush, state_reset, mtl_queue_activate, fb_pool_depth, bindless/pool/dyn/TEXU/TEXP-f1d/tbuf/shadow-hit guards).
#include "../nvn/nvn_mtl.m"
#include <sys/mman.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, label); } } while (0)

static id<MTLTexture> make_tex(tex_t *t, uint64_t serial, int fmt) {
    *t = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = serial};
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = fmt, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    CHECK(mt != nil, "created Metal texture");
    res_add(mt); mtl_tex_init(t); serial_put(serial, mt);
    return mt;
}

static void bind_tex_slot(int stage, int slot, int tid) {
    p_bind_handles b = {.stage = stage, .first = slot, .n = 1};
    b.h[0] = tid >= 0 ? (uint64_t)tid : 0;
    state_op(OP_BIND_TEX, &b);
}

static void bind_ubo_slot(int stage, int idx, uint64_t addr, uint64_t size) {
    p_bind_buf b = {.stage = stage, .index = idx, .addr = addr, .size = size};
    state_op(OP_BIND_UBO, &b);
}

static void bind_vbuf_slot(int slot, uint64_t addr, uint64_t size) {
    p_bind_vbuf b = {.first = slot, .n = 1};
    b.r[0].address = addr; b.r[0].size = size;
    state_op(OP_BIND_VBUF, &b);
}

static void test_fb_caches(void) {
    state_reset();
    tex_t ctex, ctex2, dtex, otex;
    make_tex(&ctex, 92001, NVN_FORMAT_RGBA8);
    make_tex(&ctex2, 92002, NVN_FORMAT_RGBA8);
    make_tex(&dtex, 92003, NVN_FORMAT_DEPTH32F);
    make_tex(&otex, 92004, NVN_FORMAT_RGBA8);
    const int tid_other = 700, tid_color = 701, tid_depth = 702;
    mtl_reg_tex(tid_other, &otex, NULL);
    mtl_reg_tex(tid_color, &ctex, NULL);
    mtl_reg_tex(tid_depth, &dtex, NULL);

    NvShader *fs1 = [NvShader new]; fs1->stage = 1; fs1->rf.nb = 1;
    fs1->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = MTLTextureType2D};
    fs1->rf.color_known = 1; fs1->rf.color_mask = 1;
    NvShader *fs2 = [NvShader new]; fs2->stage = 1; fs2->rf.nb = 1;
    fs2->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = MTLTextureType2D};
    fs2->rf.color_known = 1; fs2->rf.color_mask = 1;
    NvProg *pr1 = [NvProg new]; pr1->sh[1] = fs1;
    NvProg *pr2 = [NvProg new]; pr2->sh[1] = fs2;

    memset(&R, 0, sizeof R);
    R.rt.n = 1; R.rt.color[0] = &ctex; R.rt.depth = &dtex; R.has_rt = 1; g_setrt_n++;
    bind_tex_slot(1, 0, tid_other);
    S.chmask.mask = 0; S.ds.dt = 0; S.ds.dw = 0; S.ds.st = 0;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
    CHECK(rp_begin() != nil, "fb cache pass opened");

    // 1. Cold scan populates per-shader color and depth negative caches.
    fb_prepare(pr1);
    CHECK(g_fb_n == 0 && g_fb_dread == 0, "no feedback on unrelated texture");
    CHECK(fs1->fb_c_pass == g_pass.serial && fs1->fb_c_setrt == g_setrt_n, "fs1 inline fast-path cache populated");
    CHECK(fs1->fbd_dser == dtex.serial && fs1->fbd_agen == g_args_gen[1], "fs1 depth-rule negative cache populated");
    CHECK(g_fb_wmask == 0 && g_fb_dwrote == 0, "wmask/dwrote start zero when writes masked off");

    // 2. Inline fast path still updates g_fb_wmask and g_fb_dwrote when write masks enable.
    uint64_t seq0 = g_draw_seq;
    S.chmask.mask = 0xf; S.ds.dt = 1; S.ds.dw = 1;
    fb_prepare(pr1);
    CHECK(g_draw_seq == seq0 + 1 && g_fb_n == 0, "inline fast path advances g_draw_seq");
    CHECK((g_fb_wmask & 1u) != 0 && (g_fb_dwrote & FB_DS_DEPTH) != 0, "inline fast path updates g_fb_wmask and g_fb_dwrote");

    // 3. Alternating fragment shaders retain independent per-shader caches in the same pass.
    fb_prepare(pr2);
    CHECK(fs2->fb_c_pass == g_pass.serial && fs2->fbd_dser == dtex.serial, "fs2 populates its own per-shader cache");
    CHECK(fs1->fb_c_pass == g_pass.serial && fs1->fbd_dser == dtex.serial, "fs1 cache survives alternating shader");
    fb_prepare(pr1);
    CHECK(g_fb_n == 0 && fs1->fb_c_pass == g_pass.serial, "returning to fs1 stays cached");

    // 4. mtl_reg_tex re-registering bound tid_other -> ctex bumps g_fbc_gen and detects feedback.
    uint64_t fg0 = fbc_gen_load();
    mtl_reg_tex(tid_other, &ctex, NULL);
    CHECK(fbc_gen_load() > fg0, "mtl_reg_tex bumps g_fbc_gen");
    fb_prepare(pr1);
    CHECK(g_fb_n == 1 && fs1->fb_c_pass == 0, "mtl_reg_tex invalidates fb_prepare cache and triggers color snapshot");
    mtl_reg_tex(tid_other, &otex, NULL);
    if (!g_enc) { R.clear_c[0] = 1; CHECK(rp_begin() != nil, "reopen pass after color snapshot"); }
    fb_prepare(pr1);   // clears g_fb_rebind, repopulates cache
    CHECK(g_fb_n == 0 && fs1->fb_c_pass == g_pass.serial, "repopulated fb_prepare cache after restoring otex");

    // 5. mtl_reg_tex re-registering bound tid_other -> dtex invalidates fb_depth_rule cache.
    mtl_reg_tex(tid_other, &dtex, NULL);
    fb_prepare(pr1);
    CHECK(fs1->fbd_dser == 0 && (g_fb_n == 1 || g_fb_dread != 0), "mtl_reg_tex invalidates fb_depth_rule cache and detects depth feedback");
    mtl_reg_tex(tid_other, &otex, NULL);
    if (!g_enc) { R.clear_c[0] = 1; CHECK(rp_begin() != nil, "reopen pass after depth snapshot"); }
    fb_prepare(pr1);
    CHECK(g_fb_n == 0 && fs1->fb_c_pass == g_pass.serial && fs1->fbd_dser == dtex.serial, "cache restored after depth test");

    // 6. mtl_reg_tex_gone + identical re-registration bumps g_fbc_gen both times.
    uint64_t fg1 = fbc_gen_load();
    mtl_reg_tex_gone(tid_color);
    CHECK(fbc_gen_load() > fg1, "mtl_reg_tex_gone bumps g_fbc_gen");
    uint64_t fg2 = fbc_gen_load();
    mtl_reg_tex(tid_color, &ctex, NULL);
    CHECK(fbc_gen_load() > fg2, "mtl_reg_tex revival of gone ID bumps g_fbc_gen");

    // 7. OP_BIND_TEX to tid_color invalidates via g_args_gen[1] and detects feedback.
    bind_tex_slot(1, 0, tid_color);
    fb_prepare(pr1);
    CHECK(g_fb_n == 1 && fs1->fb_c_pass == 0, "OP_BIND_TEX invalidates fb_prepare cache and detects feedback");
    bind_tex_slot(1, 0, tid_other);
    if (!g_enc) { R.clear_c[0] = 1; CHECK(rp_begin() != nil, "reopen pass after bind-tex snapshot"); }
    fb_prepare(pr1);
    CHECK(g_fb_n == 0 && fs1->fb_c_pass == g_pass.serial, "cache repopulated after rebinding tid_other");

    // 8. Target swap (g_setrt_n++) with R.rt.color[0] == &otex invalidates cache.
    R.rt.color[0] = &otex; g_setrt_n++;
    fb_prepare(pr1);
    CHECK(g_fb_n == 1 && fs1->fb_c_pass == 0, "target change (g_setrt_n++) invalidates cache and detects feedback");
    R.rt.color[0] = &ctex; g_setrt_n++;
    if (!g_enc) { R.clear_c[0] = 1; CHECK(rp_begin() != nil, "reopen pass after target-change snapshot"); }
    fb_prepare(pr1);
    CHECK(g_fb_n == 0 && fs1->fb_c_pass == g_pass.serial, "cache repopulated on ctex");

    // 9. g_fb_pool_seq == g_draw_seq MUST bypass inline fast path and call args_dirty(1).
    uint64_t agen_before = g_args_gen[1];
    g_fb_pool_seq = g_draw_seq;
    fb_prepare(pr1);
    CHECK(g_args_gen[1] > agen_before, "pool_was_active bypasses inline fast path and dirties stage 1 args");
    CHECK(fs1->fb_c_pass == 0, "pool_was_active draw does not stamp fb_c_pass");

    // 10. Shaders with rf.pool, rf.dyn, rf.bindless, or NVN_RB_TEXU never stamp incompatible caches.
    NvShader *fs_pool = [NvShader new]; fs_pool->stage = 1; fs_pool->rf.pool = 1;
    NvProg *pr_pool = [NvProg new]; pr_pool->sh[1] = fs_pool;
    fb_prepare(pr_pool);
    CHECK(fs_pool->fb_c_pass == 0, "rf.pool shader never stamps fb_c_pass");

    NvShader *fs_bl = [NvShader new]; fs_bl->stage = 1; fs_bl->rf.bindless = 1; fs_bl->rf.nb = 1;
    fs_bl->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXU, .reg = 0, .off = 0, .dtt = MTLTextureType2D};
    NvProg *pr_bl = [NvProg new]; pr_bl->sh[1] = fs_bl;
    fb_prepare(pr_bl);
    CHECK(fs_bl->fb_c_pass == 0 && fs_bl->fbd_dser == 0, "bindless/TEXU shader never stamps fb_c_pass or fbd_dser");

    // 11. state_reset and mtl_queue_activate bump g_fbc_gen and clear g_vbuf_ubd_clean.
    rp_end();
    g_vbuf_ubd_clean = 0xffffu;
    uint64_t fg3 = fbc_gen_load();
    state_reset();
    CHECK(fbc_gen_load() > fg3 && g_vbuf_ubd_clean == 0, "state_reset bumps g_fbc_gen and clears g_vbuf_ubd_clean");

    mtl_queue_activate(100);
    g_vbuf_ubd_clean = 0x55u;
    uint64_t fg4 = fbc_gen_load();
    mtl_queue_activate(200);
    CHECK(fbc_gen_load() > fg4 && g_vbuf_ubd_clean == 0, "mtl_queue_activate bumps g_fbc_gen and clears g_vbuf_ubd_clean");
    mtl_queue_forget(200);
    mtl_queue_forget(100);
    (void)ctex2;
}

static void test_ubd_caches(void) {
    state_reset();
    size_t pool_sz = 65536;
    void *mem = mmap(NULL, pool_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(mem != MAP_FAILED, "mapped guest pool memory");
    memset(mem, 0x11, pool_sz);
    mtl_pool_add(mem, pool_sz);
    uint64_t base = (uint64_t)(uintptr_t)mem;
    uint64_t upd_lo = base + 4096;        // deferred update [4096, 4352)
    uint64_t other_lo = base + 16384;     // non-overlapping range 1
    uint64_t other2_lo = base + 24576;    // non-overlapping range 2
    uint64_t tbuf2_lo = base + 32768;     // buffer texture range outside first update

    uint8_t payload[256]; memset(payload, 0x5a, sizeof payload);
    mtl_sub_update(payload, upd_lo, sizeof payload);
    CHECK(g_nubd > 0, "deferred inline update queued");

    // 1. ubd_conflict_textures: negative cache + mtl_reg_tex -> buffer texture invalidation + is_tbuf/TEXP-f1d/TEXU non-caching.
    tex_t t2d; make_tex(&t2d, 93001, NVN_FORMAT_RGBA8);
    tex_t tbuf_hit = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 93002, .mem = (void *)(uintptr_t)upd_lo};
    tbuf_hit.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32F, .width = 64, .height = 1, .depth = 1, .levels = 1, .samples = 1};
    tex_t tbuf_miss = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 93003, .mem = (void *)(uintptr_t)tbuf2_lo};
    tbuf_miss.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32F, .width = 64, .height = 1, .depth = 1, .levels = 1, .samples = 1};

    const int tid_t = 710;
    mtl_reg_tex(tid_t, &t2d, NULL);
    bind_tex_slot(1, 0, tid_t);

    NvShader *fs_tex = [NvShader new]; fs_tex->stage = 1; fs_tex->rf.nb = 1;
    fs_tex->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = MTLTextureType2D};
    CHECK(ubd_conflict_textures(fs_tex, 1) == 0, "2D texture has no ubd texture conflict");
    CHECK(fs_tex->ubdt_agen == g_args_gen[1] && fs_tex->ubdt_fgen == fbc_gen_load(), "ubd_conflict_textures caches negative 2D result");

    // Re-register tid_t as a buffer texture overlapping upd_lo -> must invalidate via g_fbc_gen and return 1.
    mtl_reg_tex(tid_t, &tbuf_hit, NULL);
    CHECK(ubd_conflict_textures(fs_tex, 1) == 1 && fs_tex->ubdt_agen == 0, "mtl_reg_tex to overlapping buffer texture invalidates ubdt cache");

    // Re-register tid_t as a non-overlapping buffer texture -> returns 0, MUST NOT cache (is_tbuf == 1).
    mtl_reg_tex(tid_t, &tbuf_miss, NULL);
    CHECK(ubd_conflict_textures(fs_tex, 1) == 0 && fs_tex->ubdt_agen == 0, "non-overlapping buffer texture is not cached as buffer-texture-free");
    // Deferring an update over tbuf2_lo must now immediately conflict without changing g_args_gen or g_fbc_gen.
    mtl_sub_update(payload, tbuf2_lo, sizeof payload);
    CHECK(ubd_conflict_textures(fs_tex, 1) == 1, "subsequent update over uncached buffer texture conflicts");
    ubd_flush();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_sub_update(payload, upd_lo, sizeof payload);
    CHECK(g_nubd > 0, "re-queued deferred inline update after flush");
    mtl_reg_tex(tid_t, &t2d, NULL);

    // NVN_RB_TEXP with f1d=1 must conflict even when dtt==2D and must not cache.
    NvShader *fs_f1d = [NvShader new]; fs_f1d->stage = 1; fs_f1d->rf.nb = 1;
    fs_f1d->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXP, .dtt = MTLTextureType2D, .f1d = 1};
    CHECK(ubd_conflict_textures(fs_f1d, 1) == 1 && fs_f1d->ubdt_agen == 0, "NVN_RB_TEXP with f1d=1 conflicts and never caches");

    // NVN_RB_TEXU must never cache in ubdt.
    bind_ubo_slot(1, 0, other_lo, 256);
    *(uint32_t *)(uintptr_t)other_lo = (uint32_t)tid_t;
    NvShader *fs_texu = [NvShader new]; fs_texu->stage = 1; fs_texu->rf.nb = 1;
    fs_texu->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXU, .reg = 0, .off = 0, .dtt = MTLTextureType2D};
    CHECK(ubd_conflict_textures(fs_texu, 1) == 0 && fs_texu->ubdt_agen == 0, "NVN_RB_TEXU never caches in ubdt");

    // 2. ubd_conflict + ubd_conflict_sh + ubd_conflict_ubo + g_vbuf_ubd_clean + g_ubd_hitn.
    NvShader *vs = [NvShader new]; vs->stage = 0; vs->rf.nb = 2; vs->rf.nv = 1; vs->rf.v[0].nvn = 0;
    vs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0};
    vs->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 1};
    NvShader *fs = [NvShader new]; fs->stage = 1; fs->rf.nb = 2;
    fs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = MTLTextureType2D};
    fs->rf.b[1] = (nvn_rbind){.kind = NVN_RB_UBOP, .reg = 0};
    NvProg *pr = [NvProg new]; pr->sh[0] = vs; pr->sh[1] = fs;

    S.nattr = 1; S.attr[0] = (va_st){.fmt = NVN_FORMAT_RGB32F, .stream = 0};
    bind_vbuf_slot(0, other_lo, 1024);
    bind_ubo_slot(0, 0, other_lo, 256);
    bind_ubo_slot(0, 1, other2_lo, 256);
    bind_ubo_slot(1, 0, other_lo, 256);

    p_draw dw = {.prim = 4, .first = 0, .count = 3};
    CHECK(ubd_conflict(pr, &dw, NULL) == 0, "clean draw has no ubd conflict");
    CHECK(g_vbuf_ubd_ver == g_ubd_ver && (g_vbuf_ubd_clean & 1u) != 0, "vertex stream 0 marked clean in g_vbuf_ubd_clean");
    CHECK(vs->ubsc_ver == g_ubd_ver && vs->ubsc_use_env == 1, "cbuf-only vs caches with ubsc_use_env=1");
    CHECK(fs->ubsc_ver == g_ubd_ver && fs->ubsc_use_env == 0, "UBOP fs caches with ubsc_use_env=0");

    // Rebinding VS UBO slot 0 to another non-overlapping address leaves g_args_ubd_ugen[0] unchanged (envelope skip).
    uint64_t ubd_ug0 = g_args_ubd_ugen[0], ug0 = g_args_ugen[0];
    bind_ubo_slot(0, 0, other2_lo, 256);
    CHECK(g_args_ugen[0] > ug0 && g_args_ubd_ugen[0] == ubd_ug0, "non-overlapping OP_BIND_UBO skips g_args_ubd_ugen bump");
    CHECK(ubd_conflict(pr, &dw, NULL) == 0 && vs->ubsc_ver == g_ubd_ver, "non-overlapping UBO rebind hits fast path");

    // Rebinding FS UBOP slot 0 to overlapping range triggers ubd_conflict_ubo and returns 1.
    bind_ubo_slot(1, 0, upd_lo, 256);
    CHECK(ubd_conflict(pr, &dw, NULL) == 1 && fs->ubsc_ver == 0, "overlapping UBOP rebind detected via ubd_conflict_ubo");
    bind_ubo_slot(1, 0, other_lo, 256);
    CHECK(ubd_conflict(pr, &dw, NULL) == 0 && fs->ubsc_ver == g_ubd_ver, "FS cache restored after rebinding outside envelope");

    // Fully covered CBUF hit returns 0 (shadow covers it) but increments g_ubd_hitn so vs->ubsc_ver MUST NOT cache clean.
    uint64_t hitn0 = g_ubd_hitn;
    bind_ubo_slot(0, 0, upd_lo, 256);
    CHECK(g_args_ubd_ugen[0] > ubd_ug0, "overlapping OP_BIND_UBO bumps g_args_ubd_ugen");
    CHECK(ubd_conflict(pr, &dw, NULL) == 0, "fully covered CBUF binding succeeds via shadow");
    CHECK(g_ubd_hitn > hitn0 && vs->ubsc_ver == 0, "shadow-satisfied CBUF hit increments g_ubd_hitn and clears vs->ubsc_ver");
    bind_ubo_slot(0, 0, other_lo, 256);
    CHECK(ubd_conflict(pr, &dw, NULL) == 0 && vs->ubsc_ver == g_ubd_ver, "VS cache restored after moving CBUF out of envelope");

    // Incremental OP_BIND_VBUF clean mask: non-overlapping stays clean, overlapping clears bit and detects conflict.
    bind_vbuf_slot(0, other2_lo, 512);
    CHECK((g_vbuf_ubd_clean & 1u) != 0 && ubd_conflict(pr, &dw, NULL) == 0, "non-overlapping OP_BIND_VBUF keeps stream 0 clean");
    bind_vbuf_slot(0, upd_lo, 256);
    CHECK((g_vbuf_ubd_clean & 1u) == 0 && ubd_conflict(pr, &dw, NULL) == 1, "overlapping OP_BIND_VBUF clears clean bit and detects conflict");
    bind_vbuf_slot(0, other_lo, 1024);
    CHECK((g_vbuf_ubd_clean & 1u) != 0 && ubd_conflict(pr, &dw, NULL) == 0, "re-binding clean stream 0 restores clean bit");

    // Index buffer overlapping deferred update must conflict even when streams and shaders are cached clean.
    p_draw_idx di_hit = {.prim = 4, .itype = 2, .count = 16, .ibuf = upd_lo};
    p_draw_idx di_ok = {.prim = 4, .itype = 2, .count = 16, .ibuf = other_lo};
    CHECK(ubd_conflict(pr, NULL, &di_ok) == 0, "non-overlapping index buffer hits fast path");
    CHECK(ubd_conflict(pr, NULL, &di_hit) == 1, "overlapping index buffer bypasses fast path and conflicts");

    // ubd_flush bumps g_ubd_ver, invalidating g_vbuf_ubd_ver and ubsc_ver.
    uint64_t ver0 = g_ubd_ver;
    ubd_flush();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    CHECK(g_ubd_ver > ver0 && g_vbuf_ubd_ver != g_ubd_ver && vs->ubsc_ver != g_ubd_ver, "ubd_flush invalidates ubd version caches");

    mtl_pool_del(mem);
    munmap(mem, pool_sz);
}

int main(void) { @autoreleasepool {
    mtl_init();
    if (!g_dev) { fprintf(stderr, "no Metal device\n"); return 1; }
    mtl_sub_begin();
    test_fb_caches();
    test_ubd_caches();
    // Run again with mtl_pc_claim() active (replay-thread identity).
    mtl_pc_claim();
    test_fb_caches();
    test_ubd_caches();
    mtl_sub_commit();
    mtl_finish();
    printf("t_scan_cache: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
