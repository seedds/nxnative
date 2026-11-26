// Demand-driven uncompressed storage aliases (nvn_mtl.m, ua_*): uncompressed 2D textures of different formats over the same guest storage with matching row byte-widths and heights
// sync lazily on read instead of eagerly on every write. Covers Ocarina of Time 1.0.0's R8 -> RGBA8 and R32UI -> RGB9E5 upscaler links, avoids wasted copies on unread aliases
// (OoT 1.1.1's R11G11B10F scene target over RGB9E5/R32UI storage), handles render-pass writes read through another format, preserves compressed-partner selection when newer
// uncompressed aliases share storage, and drops stale writer state on pool finalization.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
enum { W = 8, H = 8, ROW = W * 4, SIZE = ROW * H };   // 8x8 texels of 4 bytes: rows of 32 bytes

static id<MTLBuffer> buffer(NSUInteger n) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "FAIL: no %lu-byte buffer\n", (unsigned long)n); exit(1); }
    return b;
}
static tex_t tex2d(NVNformat format, int w, int h, void *mem) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial(), .mem = mem};
    t.off = (int64_t)t.serial << 20;   // no pool: a sharing key of its own
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static id<MTLBuffer> pattern(uint8_t seed) {
    id<MTLBuffer> b = buffer(SIZE);
    for (NSUInteger i = 0; i < b.length; i++) ((uint8_t *)b.contents)[i] = (uint8_t)(seed + i * 7);
    return b;
}
static id<MTLBuffer> bytes_of_sz(id<MTLTexture> t, NSUInteger row, NSUInteger sz) {
    id<MTLBuffer> out = buffer(sz);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
              toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:sz];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "read-back completes");
    return out;
}
static id<MTLBuffer> bytes_of(id<MTLTexture> t) { return bytes_of_sz(t, ROW, SIZE); }
static id<MTLTexture> base_of(const tex_t *t) { id<MTLTexture> m = tex_mtl(t); while (m.parentTexture) m = m.parentTexture; return m; }

// Simulate a compute image store (write-only or read-write) into slot, then flush.
static void store_img(NvProg *pr, int slot, const tex_t *t, id<MTLBuffer> src, int partial_rows) {
    S.img[5][0] = slot;
    mtl_sub_begin(); image_prepare(pr, 0x20);
    id<MTLTexture> m = bcp_have(t);
    CHECK(m != nil, "image store makes the image's texture");
    NSUInteger h = partial_rows > 0 ? (NSUInteger)partial_rows : m.height;
    enc_b_copy_buffer_texture(blit(), src, 0, ROW, ROW * h, MTLSizeMake(m.width, h, 1), m, 0, 0, MTLOriginMake(0, 0, 0));
    bcp_flush(); ua_flush(0);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "image store submission completes");
}
// Simulate a compute shader reading a 2D texture from slot 0 (triggers ua_prepare_shader).
static void sample_tex(NvShader *reader, int slot) {
    S.tex[5][0] = slot;
    mtl_sub_begin(); ua_prepare_shader(reader, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "sampled read preparation completes");
}

int main(void) { @autoreleasepool {
    mtl_init();
    NvShader *cs_w = [NvShader new]; NvProg *pr_w = [NvProg new]; pr_w->sh[5] = cs_w;
    cs_w->rf.nb = 1; cs_w->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .reg = 0, .rw = 1};
    NvShader *cs_rw = [NvShader new]; NvProg *pr_rw = [NvProg new]; pr_rw->sh[5] = cs_rw;
    cs_rw->rf.nb = 1; cs_rw->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .reg = 0, .rw = 1, .img_rd = 1};
    NvShader *cs_r = [NvShader new];
    cs_r->rf.nb = 1; cs_r->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .reg = 0};

    // 1. OoT 1.0.0 link 1: R8 (4W x H) image store -> RGBA8 (W x H) sampled read.
    //    Write incurs 0 copies; first read copies once; second read copies 0 times.
    enum { STOR = 4096 };
    static uint8_t s1[STOR];
    tex_t rgba = tex2d(NVN_FORMAT_RGBA8, W, H, s1), r8 = tex2d(NVN_FORMAT_R8, W * 4, H, s1);
    mtl_tex_init(&rgba); mtl_tex_init(&r8); mtl_reg_tex(1, &rgba, NULL); mtl_reg_tex(2, &r8, NULL);
    id<MTLTexture> mrgba = base_of(&rgba); id<MTLBuffer> p1 = pattern(37);
    uint64_t c0 = g_ua_copies;
    store_img(pr_w, 2, &r8, p1, 0);
    CHECK(g_ua_copies == c0, "R8 image store copies nothing at write time");
    sample_tex(cs_r, 1);
    CHECK(g_ua_copies == c0 + 1, "reading RGBA8 triggers one lazy alias copy");
    CHECK(!memcmp(bytes_of(mrgba).contents, p1.contents, SIZE), "R8 store reaches RGBA8 byte for byte");
    sample_tex(cs_r, 1);
    CHECK(g_ua_copies == c0 + 1, "second read of RGBA8 does not copy again");

    // 2. OoT 1.0.0 link 2: R32UI store over storage with both RGB9E5 and R11G11B10F.
    //    Reading only RGB9E5 copies only to RGB9E5 (1 copy, not 2); R11G11B10F stays untouched.
    //    Then reading R11G11B10F copies to it, and re-reading RGB9E5 does NOT thrash or recopy.
    static uint8_t s2[STOR];
    tex_t u = tex2d(NVN_FORMAT_R32UI, W, H, s2), e = tex2d(NVN_FORMAT_RGB9E5F, W, H, s2), f = tex2d(NVN_FORMAT_R11G11B10F, W, H, s2);
    mtl_tex_init(&u); mtl_tex_init(&e); mtl_tex_init(&f); mtl_reg_tex(3, &u, NULL); mtl_reg_tex(4, &e, NULL); mtl_reg_tex(5, &f, NULL);
    id<MTLTexture> me = base_of(&e), mf = base_of(&f); id<MTLBuffer> p2 = pattern(91);
    c0 = g_ua_copies;
    store_img(pr_w, 3, &u, p2, 0);
    CHECK(g_ua_copies == c0, "R32UI store copies nothing at write time");
    sample_tex(cs_r, 4);
    CHECK(g_ua_copies == c0 + 1, "reading RGB9E5 triggers exactly one copy");
    CHECK(!memcmp(bytes_of(me).contents, p2.contents, SIZE), "R32UI store reaches RGB9E5");
    CHECK(memcmp(bytes_of(mf).contents, p2.contents, SIZE) != 0, "unread R11G11B10F is not copied to");
    sample_tex(cs_r, 5);
    CHECK(g_ua_copies == c0 + 2, "reading R11G11B10F copies to it on demand");
    CHECK(!memcmp(bytes_of(mf).contents, p2.contents, SIZE), "R32UI store reaches R11G11B10F on read");
    sample_tex(cs_r, 4);
    CHECK(g_ua_copies == c0 + 2, "re-reading RGB9E5 after R11G11B10F does not thrash");

    // 3. Re-registered texture over the same storage (as OoT 1.0.0 does each frame): reading the new registration gets the store.
    tex_t e2 = tex2d(NVN_FORMAT_RGB9E5F, W, H, s2); mtl_tex_init(&e2); mtl_reg_tex(6, &e2, NULL);
    id<MTLTexture> me2 = base_of(&e2); id<MTLBuffer> p3 = pattern(201);
    store_img(pr_w, 3, &u, p3, 0);
    sample_tex(cs_r, 6);
    CHECK(!memcmp(bytes_of(me2).contents, p3.contents, SIZE), "store reaches newly registered RGB9E5 texture on read");

    // 4. OoT 1.1.1 transient scene-target reuse pattern: render pass (clear + draw) into R11G11B10F,
    //    then write-only R32UI image store over the same storage, then next frame clears R11G11B10F: 0 copies!
    c0 = g_ua_copies;
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &f; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(1, 0, 0, 1);
    mtl_sub_begin(); CHECK(rp_begin() != nil, "R11G11B10F render pass opens");
    g_stat_draw++;   // simulate a draw in the pass so it is not clear-only
    rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "R11G11B10F pass completes");
    store_img(pr_w, 3, &u, p3, 0);   // write-only image store over drawn R11G11B10F
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 1, 0, 1);
    mtl_sub_begin(); CHECK(rp_begin() != nil, "next-frame clear pass into R11G11B10F opens");
    g_stat_draw++;
    rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "next-frame pass completes");
    CHECK(g_ua_copies == c0, "OoT 1.1.1 transient R11G11B10F/R32UI reuse triggers zero copies");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.clear_c[0] = 0;

    // 5. Render pass written in one uncompressed format (RGBA8) and read through another (R32UI): works lazily!
    static uint8_t s3[STOR];
    tex_t rt_rgba = tex2d(NVN_FORMAT_RGBA8, W, H, s3), rd_u32 = tex2d(NVN_FORMAT_R32UI, W, H, s3);
    mtl_tex_init(&rt_rgba); mtl_tex_init(&rd_u32); mtl_reg_tex(7, &rt_rgba, NULL); mtl_reg_tex(8, &rd_u32, NULL);
    id<MTLTexture> mrd_u32 = base_of(&rd_u32);
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &rt_rgba; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
    c0 = g_ua_copies;
    mtl_sub_begin(); CHECK(rp_begin() != nil, "RGBA8 render pass opens");
    g_stat_draw++;
    rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "RGBA8 render pass completes");
    CHECK(g_ua_copies == c0, "render pass copies nothing at write time");
    sample_tex(cs_r, 8);
    CHECK(g_ua_copies == c0 + 1, "reading R32UI after RGBA8 render pass triggers one copy");
    id<MTLBuffer> got_rp = bytes_of(mrd_u32);
    const uint32_t *px_rp = (const uint32_t *)got_rp.contents;
    CHECK(px_rp[0] == 0xffff00ffu && px_rp[W * H - 1] == 0xffff00ffu, "render-pass RGBA8 pixels reach R32UI reader");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.clear_c[0] = 0;

    // 6. Whole-level zero clear of RGBA8 followed by partial write-only R8 image store (top half), then read RGBA8:
    //    top half has R8 bytes, bottom half has the cleared zeros (not stale bytes from earlier!).
    static uint8_t s4[STOR];
    tex_t c_rgba = tex2d(NVN_FORMAT_RGBA8, W, H, s4), c_r8 = tex2d(NVN_FORMAT_R8, W * 4, H, s4);
    mtl_tex_init(&c_rgba); mtl_tex_init(&c_r8); mtl_reg_tex(9, &c_rgba, NULL); mtl_reg_tex(10, &c_r8, NULL);
    id<MTLTexture> mc_rgba = base_of(&c_rgba);
    store_img(pr_w, 10, &c_r8, p1, 0);   // seed c_r8 with non-zero bytes
    p_clear_tex ct0 = {.tex = &c_rgba, .r = {0, 0, 0, W, H, 1}, .mask = 0xf, .c.f = {0, 0, 0, 0}};
    mtl_sub_begin(); clear_tex(&ct0); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "zero clear completes");
    store_img(pr_w, 10, &c_r8, p2, H / 2);   // write only top half of c_r8
    sample_tex(cs_r, 9);
    id<MTLBuffer> got_part = bytes_of(mc_rgba);
    CHECK(!memcmp(got_part.contents, p2.contents, SIZE / 2), "top half has partial R8 image store");
    int bottom_zero = 1; for (size_t i = SIZE / 2; i < SIZE; i++) bottom_zero &= ((const uint8_t *)got_part.contents)[i] == 0;
    CHECK(bottom_zero, "bottom half preserves RGBA8 zero clear through write-only R8 store");

    // 7. Read-write image binding (img_rd=1) pre-syncs from the prior writer before dispatch.
    static uint8_t s5[STOR];
    tex_t rw_a = tex2d(NVN_FORMAT_RGBA8, W, H, s5), rw_b = tex2d(NVN_FORMAT_R32UI, W, H, s5);
    mtl_tex_init(&rw_a); mtl_tex_init(&rw_b); mtl_reg_tex(11, &rw_a, NULL); mtl_reg_tex(12, &rw_b, NULL);
    id<MTLTexture> mrw_b = base_of(&rw_b);
    store_img(pr_w, 11, &rw_a, p1, 0);
    S.img[5][0] = 12;
    mtl_sub_begin(); image_prepare(pr_rw, 0x20); bcp_flush(); ua_flush(0);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "read-write image prepare completes");
    CHECK(!memcmp(bytes_of(mrw_b).contents, p1.contents, SIZE), "read-write image binding pre-syncs prior uncompressed format");

    // 8. Compressed-partner selection is preserved even when a newer uncompressed alias shares storage!
    enum { BLOCKS = 8, BW = BLOCKS * 4, BLK = 16, BSIZE = BLOCKS * BLOCKS * BLK };
    static uint8_t s_bc[STOR];
    tex_t bc = tex2d(NVN_FORMAT_RGBA_DXT5, BW, BW, s_bc);
    tex_t bc_u32 = tex2d(NVN_FORMAT_RGBA32UI, BLOCKS, BLOCKS, s_bc);
    tex_t bc_f32 = tex2d(NVN_FORMAT_RGBA32F, BLOCKS, BLOCKS, s_bc);   // newer uncompressed alias at same storage
    tex_t bc_src = tex2d(NVN_FORMAT_RGBA32UI, BLOCKS, BLOCKS, NULL);
    mtl_tex_init(&bc); mtl_tex_init(&bc_u32); mtl_tex_init(&bc_f32); mtl_tex_init(&bc_src);
    mtl_reg_tex(13, &bc, NULL); mtl_reg_tex(14, &bc_u32, NULL); mtl_reg_tex(15, &bc_f32, NULL);
    id<MTLTexture> mbc = base_of(&bc), mbc_src = base_of(&bc_src);
    { tex_t pair; CHECK(bcp_find(&bc_u32, &pair) && pair.serial == bc.serial, "bcp_find preserves compressed partner over newer uncompressed alias"); }
    id<MTLBuffer> pbc = buffer(BSIZE);
    for (NSUInteger i = 0; i < BSIZE; i++) ((uint8_t *)pbc.contents)[i] = (uint8_t)(19 + i * 11);
    { id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
      [be copyFromBuffer:pbc sourceOffset:0 sourceBytesPerRow:BLOCKS * BLK sourceBytesPerImage:BSIZE sourceSize:MTLSizeMake(BLOCKS, BLOCKS, 1)
               toTexture:mbc_src destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      [be endEncoding]; [cb commit]; [cb waitUntilCompleted]; }
    uint64_t bca0 = g_bc_alias_n;
    p_copy_t2t cp_bc = {.src = &bc_src, .dst = &bc_u32, .sr = {0, 0, 0, BLOCKS, BLOCKS, 1}, .dr = {0, 0, 0, BLOCKS, BLOCKS, 1}};
    mtl_sub_begin(); copy_t2t(&cp_bc); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "bc_alias_copy submission completes");
    CHECK(g_bc_alias_n == bca0 + 1, "bc_alias_copy still fires with newer uncompressed alias present");
    CHECK(!tex_get(bc_u32.serial), "uncompressed BC alias still allocates no Metal texture");
    CHECK(!memcmp(bytes_of_sz(mbc, BLOCKS * BLK, BSIZE).contents, pbc.contents, BSIZE), "bc_alias_copy lands in BC3 blocks");

    // 9. Same format, mismatched height, and depth textures never trigger uncompressed alias copies.
    static uint8_t s6[STOR];
    tex_t same_a = tex2d(NVN_FORMAT_R32UI, W, H, s6), same_b = tex2d(NVN_FORMAT_R32UI, W, H, s6);
    tex_t bad_h = tex2d(NVN_FORMAT_R8, W * 4, H / 2, s6), dep = tex2d(NVN_FORMAT_DEPTH32F, W, H, s6);
    mtl_tex_init(&same_a); mtl_tex_init(&same_b); mtl_tex_init(&bad_h); mtl_tex_init(&dep);
    mtl_reg_tex(16, &same_a, NULL); mtl_reg_tex(17, &same_b, NULL); mtl_reg_tex(18, &bad_h, NULL); mtl_reg_tex(19, &dep, NULL);
    c0 = g_ua_copies;
    store_img(pr_w, 16, &same_a, p1, 0);
    sample_tex(cs_r, 17); sample_tex(cs_r, 18); sample_tex(cs_r, 19);
    CHECK(g_ua_copies == c0, "same format, mismatched height and depth trigger no alias copies");

    // 10. Pool finalization drops writer state for that pool so reused addresses never see stale writes.
    static uint8_t s7[STOR];
    pool_t fake_pool = {.magic = MAGIC('P','O','O','L'), .mem = s7, .size = STOR};
    tex_t pa = tex2d(NVN_FORMAT_R32UI, W, H, s7), pb = tex2d(NVN_FORMAT_RGBA8, W, H, s7);
    pa.pool = &fake_pool; pa.off = 0; pb.pool = &fake_pool; pb.off = 0;
    mtl_tex_init(&pa); mtl_tex_init(&pb); mtl_reg_tex(20, &pa, NULL); mtl_reg_tex(21, &pb, NULL);
    id<MTLTexture> mpb = base_of(&pb);
    // 10a. Bindless texture-pool shader (NVN_RB_TEXP) flushes dirty writers to live registered pool aliases of matching class.
    NvShader *cs_bindless = [NvShader new];
    cs_bindless->rf.nb = 1;
    cs_bindless->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXP, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    NvShader *cs_bindless_u = [NvShader new];
    cs_bindless_u->rf.nb = 1;
    cs_bindless_u->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXP, .dtt = MTLTextureType2D, .dpf = NVN_DPF_UINT};
    // Drain earlier tests' bindless dirty list so counts below isolate s7 alone.
    mtl_sub_begin(); ua_prepare_shader(cs_bindless, 5); ua_prepare_shader(cs_bindless_u, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "bindless baseline drain completes");
    c0 = g_ua_copies;
    store_img(pr_w, 20, &pa, p1, 0);
    CHECK(g_ua_copies == c0, "pool-backed R32UI store copies nothing at write time");
    mtl_sub_begin(); ua_prepare_shader(cs_bindless_u, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "mismatched-class bindless prepare completes");
    CHECK(g_ua_copies == c0, "UINT bindless NVN_RB_TEXP skips FLOAT RGBA8 alias");
    mtl_sub_begin(); ua_prepare_shader(cs_bindless, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "bindless NVN_RB_TEXP prepare completes");
    CHECK(g_ua_copies == c0 + 1, "bindless NVN_RB_TEXP syncs compatible pool alias once");
    CHECK(!memcmp(bytes_of(mpb).contents, p1.contents, SIZE), "bindless NVN_RB_TEXP lands R32UI bytes in RGBA8 alias");
    mtl_sub_begin(); ua_prepare_shader(cs_bindless, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "repeat bindless prepare completes");
    CHECK(g_ua_copies == c0 + 1, "repeat bindless NVN_RB_TEXP does not copy again");
    // 10b. Finalized (retired) textures kept in g_alias are NOT synced by bindless shaders.
    mtl_tex_release(&pb);
    c0 = g_ua_copies;
    store_img(pr_w, 20, &pa, p2, 0);
    mtl_sub_begin(); ua_prepare_shader(cs_bindless, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "bindless prepare after finalize completes");
    CHECK(g_ua_copies == c0, "finalized transient alias in g_alias triggers zero bindless copies");
    // 10c. Finalizing the pool drops its uncompressed alias writer state.
    mtl_alias_drop_pool(&fake_pool);
    tex_t pb2 = tex2d(NVN_FORMAT_RGBA8, W, H, s7); mtl_tex_init(&pb2); mtl_reg_tex(22, &pb2, NULL);
    c0 = g_ua_copies;
    sample_tex(cs_r, 22);
    CHECK(g_ua_copies == c0, "finalized pool drops its uncompressed alias writer state");

    // 11. Registration changes update the hot IDs one by one (ua_hot_refresh), without a walk of every writer entry.
    //     Mario Kart World registers textures over written storage about 46 times a frame, with IDs past 65,536.
    static uint8_t s8[STOR], s9[STOR];
    tex_t wu = tex2d(NVN_FORMAT_R32UI, W, H, s8); mtl_tex_init(&wu); mtl_reg_tex(30, &wu, NULL);
    id<MTLBuffer> p11 = pattern(113);
    store_img(pr_w, 30, &wu, p11, 0);
    // 11a. An alias registered after the store, with nothing written in between, is hot and gets the store on its first read.
    tex_t la = tex2d(NVN_FORMAT_RGB9E5F, W, H, s8); mtl_tex_init(&la); mtl_reg_tex(31, &la, NULL);
    c0 = g_ua_copies;
    sample_tex(cs_r, 31);
    CHECK(ua_hot_test(31), "an alias registered after the store is hot");
    CHECK(g_ua_copies == c0 + 1, "an alias registered after the store is copied to on its first read");
    CHECK(!memcmp(bytes_of(base_of(&la)).contents, p11.contents, SIZE), "the store reaches the alias registered after it");
    // 11b. An ID past 65,536 has a bit of its own: its neighbour over unwritten storage is not hot.
    tex_t hi = tex2d(NVN_FORMAT_R11G11B10F, W, H, s8), hn = tex2d(NVN_FORMAT_R11G11B10F, W, H, s9);
    mtl_tex_init(&hi); mtl_tex_init(&hn); mtl_reg_tex(70000, &hi, NULL); mtl_reg_tex(70001, &hn, NULL);
    c0 = g_ua_copies;
    sample_tex(cs_r, 31);   // any prepared read brings the hot IDs up to date
    CHECK(ua_hot_test(70000), "an alias with an ID past 65,536 is hot");
    CHECK(!ua_hot_test(70001), "an ID past 65,536 over unwritten storage is not hot");
    CHECK(g_ua_copies == c0, "a repeat read copies nothing while other IDs are registered");
    // 11c. A registration that goes away is no longer hot; the others at its storage stay.
    mtl_reg_tex(70000, NULL, NULL);
    sample_tex(cs_r, 31);
    CHECK(!ua_hot_test(70000), "an unregistered alias is no longer hot");
    CHECK(ua_hot_test(31), "the other alias at its storage stays hot");
    // 11d. More registration changes between two reads than the change list holds: the whole walk still finds every alias.
    enum { MANY = UA_REG_EV + 8 };
    static tex_t many[MANY];
    for (int i = 0; i < MANY; i++) { many[i] = tex2d(NVN_FORMAT_RGB9E5F, W, H, s8); mtl_tex_init(&many[i]); mtl_reg_tex(1000 + i, &many[i], NULL); }
    c0 = g_ua_copies;
    sample_tex(cs_r, 1000 + MANY - 1);
    int hot_many = 0; for (int i = 0; i < MANY; i++) hot_many += ua_hot_test(1000 + i);
    CHECK(hot_many == MANY, "every alias is hot after more registrations than the change list holds");
    CHECK(g_ua_copies == c0 + 1, "the last of them is copied to on its read");
    // 11e. Storage written again through one of its aliases: that alias's bit clears (it is the writer now) and the former writer's sets.
    NvShader *cs_none = [NvShader new];
    S.tex[5][0] = 31;
    mtl_sub_begin(); ua_note_write(&la, bcp_have(&la) ?: tex_mtl(&la), 0, 0); ua_prepare_shader(cs_none, 5);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "write through the alias completes");
    CHECK(!ua_hot_test(31), "the texture that wrote last is not hot");
    CHECK(ua_hot_test(30), "the former writer is hot once an alias of another format wrote its storage");

    // 11f. An ID that leaves written storage and is registered again over storage written afterwards: the pending change for the old address must not clear the bit the
    //      write at the new address set (ua_note_write applies pending changes first).
    static uint8_t s10[STOR];
    tex_t mv = tex2d(NVN_FORMAT_R11G11B10F, W, H, s8); mtl_tex_init(&mv); mtl_reg_tex(40, &mv, NULL);
    sample_tex(cs_r, 40);
    CHECK(ua_hot_test(40), "an alias of written storage is hot before it moves");
    mtl_reg_tex(40, NULL, NULL);
    tex_t mv2 = tex2d(NVN_FORMAT_R11G11B10F, W, H, s10), wv = tex2d(NVN_FORMAT_R32UI, W, H, s10);
    mtl_tex_init(&mv2); mtl_tex_init(&wv); mtl_reg_tex(40, &mv2, NULL); mtl_reg_tex(41, &wv, NULL);
    id<MTLBuffer> p13 = pattern(171);
    store_img(pr_w, 41, &wv, p13, 0);
    c0 = g_ua_copies;
    sample_tex(cs_r, 40);
    CHECK(ua_hot_test(40), "an ID registered again over storage written afterwards is hot");
    CHECK(g_ua_copies == c0 + 1, "it is copied to on its read");
    CHECK(!memcmp(bytes_of(base_of(&mv2)).contents, p13.contents, SIZE), "the store reaches the ID registered again at other storage");

    // 12a. Pool drop drained during blit() -> rp_end() -> ua_flush() inside ua_sync_tex() must not UAF w.
    static uint8_t s11[STOR], s12[STOR];
    pool_t drop_pool = {.magic = MAGIC('P','O','O','L'), .mem = s11, .size = STOR};
    tex_t dp_w = tex2d(NVN_FORMAT_R32UI, W, H, s11), dp_r = tex2d(NVN_FORMAT_RGBA8, W, H, s11);
    tex_t dp_rt = tex2d(NVN_FORMAT_RGBA8, W, H, s12);
    dp_w.pool = &drop_pool; dp_w.off = 0; dp_r.pool = &drop_pool; dp_r.off = 0;
    mtl_tex_init(&dp_w); mtl_tex_init(&dp_r); mtl_tex_init(&dp_rt);
    mtl_reg_tex(23, &dp_w, NULL); mtl_reg_tex(24, &dp_r, NULL);
    store_img(pr_w, 23, &dp_w, p1, 0);
    id<MTLTexture> mdp_r = base_of(&dp_r);
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &dp_rt; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 1, 0, 1);
    mtl_sub_begin(); CHECK(rp_begin() != nil, "pending RT pass opens for 12a");
    g_stat_draw++;
    ua_drop_pool(&drop_pool);   // deferred until ua_drain_drops inside rp_end -> ua_flush -> ua_note_write
    c0 = g_ua_copies;
    ua_sync_tex(&dp_r, mdp_r, nvn_hm_get(&g_ua_wr, &dp_w.mem), UA_RSN_DIRECT);
    CHECK(g_ua_copies == c0 + 1, "ua_sync_tex completes safely when rp_end drains writer pool drop");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "12a submission completes");
    CHECK(!memcmp(bytes_of(mdp_r).contents, p1.contents, SIZE), "12a copied writer bytes before drop");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.clear_c[0] = 0;

    // 12b. When rp_end() inside ua_before_read's blit updates g_ua_wr[mem] in place to a newer RT write,
    //      the read texture must be stamped with the earlier writer's gen, not the newer RT's gen.
    static uint8_t s13[STOR];
    tex_t mut_w0 = tex2d(NVN_FORMAT_R32UI, W, H, s13), mut_rt = tex2d(NVN_FORMAT_RGBA8, W, H, s13), mut_r = tex2d(NVN_FORMAT_R8, W * 4, H, s13);
    mtl_tex_init(&mut_w0); mtl_tex_init(&mut_rt); mtl_tex_init(&mut_r);
    mtl_reg_tex(25, &mut_w0, NULL); mtl_reg_tex(26, &mut_rt, NULL); mtl_reg_tex(27, &mut_r, NULL);
    id<MTLTexture> mmut_r = base_of(&mut_r);
    store_img(pr_w, 25, &mut_w0, p1, 0);
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &mut_rt; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 1, 0, 1);
    mtl_sub_begin(); CHECK(rp_begin() != nil, "mutating RT pass opens for 12b");
    g_stat_draw++;
    ua_before_read(&mut_r, mmut_r);   // copies p1 from mut_w0 while rp_end updates g_ua_wr[s13] to green mut_rt
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "12b first submission completes");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.clear_c[0] = 0;
    c0 = g_ua_copies;
    sample_tex(cs_r, 27);   // must observe mut_rt's newer gen and sync green RT pixels into mut_r
    CHECK(g_ua_copies == c0 + 1, "subsequent read sees newer RT gen after in-place g_ua_wr update");
    {
        id<MTLBuffer> got_mut = bytes_of(mmut_r);
        const uint32_t *px_mut = (const uint32_t *)got_mut.contents;
        CHECK(px_mut[0] == 0xff00ff00u && px_mut[W * H - 1] == 0xff00ff00u, "R8 alias receives green render-pass pixels");
    }

    // 13. ua_prepare_bindless calls pool_seen_scan() with g_pool_walk = 1 so scanned IDs are not stamped as slot-bind uses.
    (void)pool_seen_gpu();
    g_pool_walk = 0;
    os_unfair_lock_lock(&g_rmu);
    g_seen_sub = g_sub_serial - 1;
    g_treg[27].used = 0;
    g_seen_todo[27 >> 5] |= 1u << (27 & 31);
    os_unfair_lock_unlock(&g_rmu);
    store_img(pr_w, 25, &mut_w0, p2, 0);
    mtl_sub_begin();
    g_seen_sub = g_sub_serial - 1;
    g_treg[27].used = 0;
    g_seen_todo[27 >> 5] |= 1u << (27 & 31);
    ua_prepare_shader(cs_bindless, 5);
    CHECK(g_pool_walk == 0, "g_pool_walk is restored after ua_prepare_bindless");
    CHECK(g_treg[27].used == 0, "ua_prepare_bindless does not stamp scanned IDs as slot-bind uses");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "13 submission completes");

    printf("t_texel_alias: %d checks, %d failed (ua_copies=%llu, zero_fills=%llu)\n", checks, failed, (unsigned long long)g_ua_copies, (unsigned long long)g_ua_zero_fills);
    return failed != 0;
} }
