// Compressed-alias pairs (nvn_mtl.m, bc_alias_copy and bcp_*): an uncompressed texture that aliases a block-compressed one (the same guest storage, one 16-byte texel per BC3
// block, its size in blocks) and the compressed texture stay equal, as the shared storage keeps them on the console. A texture copy into an alias with no texture of its own lands
// in the compressed texture's blocks (Monster Jam Steel Titans 2's runtime virtual texture); an image store into the alias (Mortal Kombat 11's BC3CompressTexture, PR #2802) or a
// render pass into it goes into the compressed texture when the dispatch or the pass ends; a copy into the compressed texture goes into the alias; a side is filled from the other
// before its first write, so a write of a part of it keeps the rest; a read-only image binding writes nothing; a copy into a texture over other storage leaves both alone
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
enum { BLOCKS = 8, W = BLOCKS * 4, BLOCK = 16 };   // a 32x32 BC3 texture: 8x8 blocks of 16 bytes, aliased by an 8x8 RGBA32UI texture

static id<MTLBuffer> buffer(NSUInteger n) {   // a shared buffer of n bytes; the test stops when Metal makes none
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "FAIL: no %lu-byte buffer\n", (unsigned long)n); exit(1); }
    return b;
}
static tex_t tex2d(NVNformat format, int w, void *mem) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial(), .mem = mem};
    t.off = (int64_t)t.serial << 20;   // no pool: a sharing key of its own
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = w, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static void run(void (^encode)(id<MTLBlitCommandEncoder> b)) {
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    encode(b); [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "blit completes");
}
static void fill(id<MTLTexture> t, uint8_t seed) {   // every byte of the 8x8 RGBA32UI texture its own value
    id<MTLBuffer> src = buffer(BLOCKS * BLOCKS * BLOCK);
    for (NSUInteger i = 0; i < src.length; i++) ((uint8_t *)src.contents)[i] = (uint8_t)(seed + i * 7);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:BLOCKS * BLOCK sourceBytesPerImage:BLOCKS * BLOCKS * BLOCK
                                               sourceSize:MTLSizeMake(BLOCKS, BLOCKS, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)]; });
}
static id<MTLBuffer> blocks_of(id<MTLTexture> bc) {   // the compressed texture's blocks, row by row
    id<MTLBuffer> out = buffer(BLOCKS * BLOCKS * BLOCK);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromTexture:bc sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, W, 1)
                                                  toBuffer:out destinationOffset:0 destinationBytesPerRow:BLOCKS * BLOCK destinationBytesPerImage:BLOCKS * BLOCKS * BLOCK]; });
    return out;
}
static void copy(const tex_t *src, const tex_t *dst) {
    p_copy_t2t c = {.src = src, .dst = dst, .sr = {0, 0, 0, BLOCKS, BLOCKS, 1}, .dr = {0, 0, 0, BLOCKS, BLOCKS, 1}};
    mtl_sub_begin(); copy_t2t(&c);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the copy's submission completes");
}

static id<MTLBuffer> pattern(uint8_t seed) {   // 8x8 blocks of 16 bytes, every byte its own value
    id<MTLBuffer> b = buffer(BLOCKS * BLOCKS * BLOCK);
    for (NSUInteger i = 0; i < b.length; i++) ((uint8_t *)b.contents)[i] = (uint8_t)(seed + i * 7);
    return b;
}
static id<MTLBuffer> texels_of(id<MTLTexture> t) {   // the 8x8 RGBA32UI texture's texels, row by row
    id<MTLBuffer> out = buffer(BLOCKS * BLOCKS * BLOCK);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(BLOCKS, BLOCKS, 1)
                                                  toBuffer:out destinationOffset:0 destinationBytesPerRow:BLOCKS * BLOCK destinationBytesPerImage:BLOCKS * BLOCKS * BLOCK]; });
    return out;
}
// Whether block rows [y0, y1) x columns [x0, x1) of got equal want's
static int blocks_eq(id<MTLBuffer> got, id<MTLBuffer> want, int x0, int y0, int x1, int y1) {
    for (int y = y0; y < y1; y++) if (memcmp((uint8_t *)got.contents + (y * BLOCKS + x0) * BLOCK, (uint8_t *)want.contents + (y * BLOCKS + x0) * BLOCK, (size_t)(x1 - x0) * BLOCK)) return 0;
    return 1;
}
// What a shader's image store does, in command order: blocks [x0, x1) x [y0, y1) of the 8x8 RGBA32UI texture from src's
static void store(id<MTLTexture> t, id<MTLBuffer> src, int x0, int y0, int x1, int y1) {
    enc_b_copy_buffer_texture(blit(), src, (NSUInteger)(y0 * BLOCKS + x0) * BLOCK, BLOCKS * BLOCK, BLOCKS * BLOCKS * BLOCK, MTLSizeMake((NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0), 1), t, 0, 0, MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0));
}

int main(void) { @autoreleasepool {
    mtl_init();
    static uint8_t storage[W * W], other[BLOCKS * BLOCKS * BLOCK];   // the compressed texture's guest storage (BC3: one byte per texel), and storage nothing else shares
    tex_t bc = tex2d(NVN_FORMAT_RGBA_DXT5, W, storage), alias = tex2d(NVN_FORMAT_RGBA32UI, BLOCKS, storage);
    tex_t src = tex2d(NVN_FORMAT_RGBA32UI, BLOCKS, NULL), apart = tex2d(NVN_FORMAT_RGBA32UI, BLOCKS, other);
    mtl_tex_init(&bc); mtl_tex_init(&alias); mtl_tex_init(&src); mtl_tex_init(&apart);
    mtl_reg_tex(1, &bc, NULL); mtl_reg_tex(2, &alias, NULL);
    id<MTLTexture> mbc = tex_mtl(&bc), msrc = tex_mtl(&src);
    while (mbc.parentTexture) mbc = mbc.parentTexture;
    CHECK(mbc && bc_block_bytes(mbc.pixelFormat) == BLOCK, "the compressed texture is BC3 in Metal");
    CHECK(msrc && msrc.pixelFormat == MTLPixelFormatRGBA32Uint, "the source is RGBA32UI");

    fill(msrc, 3);
    copy(&src, &alias);
    id<MTLBuffer> got = blocks_of(mbc), want = buffer(BLOCKS * BLOCKS * BLOCK);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromTexture:msrc sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(BLOCKS, BLOCKS, 1)
                                                  toBuffer:want destinationOffset:0 destinationBytesPerRow:BLOCKS * BLOCK destinationBytesPerImage:BLOCKS * BLOCKS * BLOCK]; });
    CHECK(!memcmp(got.contents, want.contents, got.length), "the copy into the alias lands in the compressed texture's blocks");
    CHECK(g_bc_alias_n == 1, "and is counted as an alias copy");
    CHECK(!tex_get(alias.serial), "the alias gets no Metal texture of its own");

    fill(msrc, 101);
    copy(&src, &apart);
    id<MTLBuffer> again = blocks_of(mbc);
    CHECK(!memcmp(again.contents, got.contents, got.length), "a copy into a texture over other storage leaves the compressed texture as it was");
    CHECK(g_bc_alias_n == 1, "and is not an alias copy");

    copy(&src, &alias);   // a second alias copy reuses the staging buffer
    id<MTLBuffer> third = blocks_of(mbc), want2 = buffer(BLOCKS * BLOCKS * BLOCK);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromTexture:msrc sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(BLOCKS, BLOCKS, 1)
                                                  toBuffer:want2 destinationOffset:0 destinationBytesPerRow:BLOCKS * BLOCK destinationBytesPerImage:BLOCKS * BLOCKS * BLOCK]; });
    CHECK(!memcmp(third.contents, want2.contents, third.length), "a second copy into the alias lands too");
    CHECK(g_bc_alias_n == 2, "and is counted");

    // An image store into the alias, as image_prepare sees it before a dispatch: the alias, made now, is filled from the compressed texture first; the store goes into the
    // compressed texture when the dispatch ends (Mortal Kombat 11's BC3CompressTexture writes its RGBA32UI image this way)
    NvShader *cs = [NvShader new]; NvProg *pr = [NvProg new]; pr->sh[5] = cs;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .reg = 0, .rw = 1};
    S.img[5][0] = 2;
    uint64_t fills = g_bcp_fills, copies = g_bcp_copies;
    id<MTLBuffer> p1 = pattern(37);
    mtl_sub_begin(); image_prepare(pr, 0x20);
    id<MTLTexture> malias = bcp_have(&alias);
    CHECK(malias && malias.pixelFormat == MTLPixelFormatRGBA32Uint, "the image store makes the alias's texture");
    CHECK(g_nbcp == 1, "and notes the alias as written");
    store(malias, p1, 0, 0, BLOCKS, BLOCKS); bcp_flush();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the image store's submission completes");
    CHECK(g_bcp_fills == fills + 1, "the alias was filled from the compressed texture before its first write");
    CHECK(g_bcp_copies == copies + 1, "the store was copied into the compressed texture");
    CHECK(!memcmp(blocks_of(mbc).contents, p1.contents, p1.length), "the image store reaches the compressed texture's blocks byte for byte");

    // A copy into the compressed texture (blocks 0-3 x 0-3, 16x16 pixels) goes into the alias's texels there; the rest of the alias stays
    static uint8_t bsrc_storage[W * W];
    tex_t bsrc = tex2d(NVN_FORMAT_RGBA_DXT5, W, bsrc_storage); mtl_tex_init(&bsrc);
    id<MTLTexture> mbsrc = tex_mtl(&bsrc); id<MTLBuffer> p2 = pattern(91);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromBuffer:p2 sourceOffset:0 sourceBytesPerRow:BLOCKS * BLOCK sourceBytesPerImage:BLOCKS * BLOCKS * BLOCK
                                               sourceSize:MTLSizeMake(W, W, 1) toTexture:mbsrc destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)]; });
    { tex_t pair; CHECK(bcp_find(&bc, &pair) && pair.serial == alias.serial, "the compressed texture's pair is the alias"); }
    p_copy_t2t cc = {.src = &bsrc, .dst = &bc, .sr = {0, 0, 0, 16, 16, 1}, .dr = {0, 0, 0, 16, 16, 1}};
    mtl_sub_begin(); copy_t2t(&cc);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the compressed copy's submission completes");
    // p2's first 4x4 blocks, as the source texture's blocks 0-3 x 0-3 (its rows are 8 blocks long, as the alias's are)
    id<MTLBuffer> a2 = texels_of(malias), want_a2 = buffer(BLOCKS * BLOCKS * BLOCK);
    memcpy(want_a2.contents, p1.contents, p1.length);
    for (int y = 0; y < 4; y++) memcpy((uint8_t *)want_a2.contents + y * BLOCKS * BLOCK, (uint8_t *)p2.contents + y * BLOCKS * BLOCK, 4 * BLOCK);
    CHECK(!memcmp(a2.contents, want_a2.contents, a2.length), "a copy into the compressed texture reaches the alias's texels there, and only there");

    // A store into another part of the alias, then the whole alias into the compressed texture: the compressed copy's blocks stay, as the alias carried them
    id<MTLBuffer> p3 = pattern(201);
    mtl_sub_begin(); image_prepare(pr, 0x20);
    CHECK(g_nbcp == 1, "the second store is noted");
    store(malias, p3, 4, 4, BLOCKS, BLOCKS); bcp_flush();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the second store's submission completes");
    CHECK(g_bcp_fills == fills + 1, "an alias equal to the compressed texture is not filled again");
    id<MTLBuffer> b3 = blocks_of(mbc);
    CHECK(blocks_eq(b3, want_a2, 0, 0, 4, 4), "the compressed copy's blocks survive the alias's later store");
    CHECK(blocks_eq(b3, p3, 4, 4, BLOCKS, BLOCKS), "the second store's blocks are in the compressed texture");
    CHECK(blocks_eq(b3, p1, 4, 0, BLOCKS, 4) && blocks_eq(b3, p1, 0, 4, 4, BLOCKS), "and the blocks neither wrote are the first store's");

    // A read-only image binding writes nothing: no note, no copy
    cs->rf.b[0].rw = 0; copies = g_bcp_copies;
    mtl_sub_begin(); image_prepare(pr, 0x20); bcp_flush();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the read-only submission completes");
    CHECK(g_bcp_copies == copies, "a read-only image binding copies nothing");

    // Image handles are relative to the execution pool's base (bound_img): raw slot 1 with base 1 is registration 2, the alias
    cs->rf.b[0].rw = 1; S.tex_base = 1; S.img[5][0] = 1;
    mtl_sub_begin(); image_prepare(pr, 0x20);
    CHECK(g_nbcp == 1, "a handle relative to the pool's base finds the alias");
    bcp_flush(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the pool-base submission completes");
    S.tex_base = 0; S.img[5][0] = 2;

    // A render pass into the alias (a clear to 5, 6, 7, 8 in every texel) goes into the compressed texture when the pass ends
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &alias; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(5, 6, 7, 8);
    mtl_sub_begin(); CHECK(rp_begin() != nil, "the pass into the alias opens");
    CHECK(g_nbcp == 1, "the attachment is noted as written");
    rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the pass's submission completes");
    id<MTLBuffer> b4 = blocks_of(mbc); int cleared = 1;
    for (int i = 0; i < BLOCKS * BLOCKS; i++) { const uint32_t *v = (const uint32_t *)((const uint8_t *)b4.contents + i * BLOCK); cleared &= v[0] == 5 && v[1] == 6 && v[2] == 7 && v[3] == 8; }
    CHECK(cleared, "the pass's texels are the compressed texture's blocks");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.clear_c[0] = 0;

    // The ID registers another texture: the pair is gone, and a copy into the compressed texture no longer reaches the old alias
    mtl_reg_tex(2, &apart, NULL);
    { tex_t pair; CHECK(!bcp_find(&bc, &pair), "a registration that changes texture leaves the pair"); }

    // A second alias over the same storage, made after the compressed texture was written, takes its blocks at once (bcp_up_fill, in the upload batch): a shader may read the
    // alias before writing it, and a texture binding resolved inside a render pass cannot copy in the replay's order
    tex_t alias2 = tex2d(NVN_FORMAT_RGBA32UI, BLOCKS, storage); mtl_tex_init(&alias2); mtl_reg_tex(3, &alias2, NULL);
    id<MTLBuffer> bc_now = blocks_of(mbc);
    mtl_sub_begin(); id<MTLTexture> ma2 = tex_mtl(&alias2);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the second alias's submission completes");
    CHECK(ma2 && !memcmp(texels_of(ma2).contents, bc_now.contents, bc_now.length), "an alias made after the compressed texture's write holds its blocks");

    // The CPU's bytes uploaded into the compressed texture (a packaged texture taken from its storage) reach the alias too (bcp_up_follow)
    id<MTLBuffer> p4 = pattern(17);
    run(^(id<MTLBlitCommandEncoder> b) { [b copyFromBuffer:p4 sourceOffset:0 sourceBytesPerRow:BLOCKS * BLOCK sourceBytesPerImage:BLOCKS * BLOCKS * BLOCK
                                               sourceSize:MTLSizeMake(W, W, 1) toTexture:mbc destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)]; });
    mtl_sub_begin(); bcp_up_follow(&bc, mbc);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the upload's submission completes");
    CHECK(!memcmp(texels_of(ma2).contents, p4.contents, p4.length), "an upload into the compressed texture reaches the alias");

    // A texture clear of the alias (clear_tex, unsigned 9, 10, 11, 12 over blocks 2-5 x 1-3) goes into the compressed texture's blocks there, and only there
    p_clear_tex ct = {.tex = &alias2, .is_uint = 1, .r = {2, 1, 0, 4, 3, 1}, .mask = 0xf, .c.u = {9, 10, 11, 12}};
    mtl_sub_begin(); clear_tex(&ct);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the texture clear's submission completes");
    id<MTLBuffer> b5 = blocks_of(mbc); int in = 1, out = 1;
    for (int y = 0; y < BLOCKS; y++) for (int x = 0; x < BLOCKS; x++) { const uint32_t *v = (const uint32_t *)((const uint8_t *)b5.contents + (y * BLOCKS + x) * BLOCK);
        if (x >= 2 && x < 6 && y >= 1 && y < 4) in &= v[0] == 9 && v[1] == 10 && v[2] == 11 && v[3] == 12;
        else out &= !memcmp(v, (const uint8_t *)p4.contents + (y * BLOCKS + x) * BLOCK, BLOCK); }
    CHECK(in, "the cleared texels are the compressed texture's blocks");
    CHECK(out, "and the blocks outside the clear stay");

    // A scissored zero clear of the alias as a render target (clear_rect_zero: blocks 0-2 x 5-7) goes into the compressed texture too
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &alias2; R.has_sc = 1; R.sc = (p_scissor){0, 5, 3, 3};
    mtl_sub_begin(); CHECK(clear_rect_zero(0), "the scissored clear is taken");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the scissored clear's submission completes");
    id<MTLBuffer> b6 = blocks_of(mbc); int zero = 1;
    for (int y = 5; y < 8; y++) for (int i = 0; i < 3 * BLOCK; i++) zero &= ((const uint8_t *)b6.contents)[(y * BLOCKS) * BLOCK + i] == 0;
    CHECK(zero, "the scissored zero clear reaches the compressed texture's blocks");
    CHECK(blocks_eq(b6, b5, 3, 5, BLOCKS, BLOCKS) && blocks_eq(b6, b5, 0, 0, BLOCKS, 5), "and the rest stays");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.has_sc = 0;

    printf("t_bc_alias: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
