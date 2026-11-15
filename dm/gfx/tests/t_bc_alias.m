// bc_alias_copy (nvn_mtl.m): a texture copy into an uncompressed texture that aliases a block-compressed one (the same guest storage, one 16-byte texel per BC3 block, its size in
// blocks) lands in the compressed texture's blocks, as the shared storage does on the console; a copy into a texture over other storage leaves the compressed one alone.
// Unreal's runtime virtual texture compresses its tiles to RGBA32UI blocks and copies them into such an alias of its BC3 tile cache (Monster Jam Steel Titans 2's black arena floor)
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

    // A compressor writes the integer alias directly, without CopyTextureToTexture.
    // Its GPU result must become the sampled BC blocks, even if that BC backing
    // already existed before the dispatch (MK11's character-selection images).
    id<MTLTexture> malias = tex_mtl(&alias);
    fill(malias, 37);
    NvShader *cs = [NvShader new];
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .reg = 0, .rw = 1};
    S.img[5][0] = 2;
    mtl_sub_begin(); bc_alias_images(cs);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "image alias propagation completes");
    id<MTLBuffer> from_image = blocks_of(mbc), expected_image = buffer(BLOCKS * BLOCKS * BLOCK);
    for (NSUInteger i = 0; i < expected_image.length; i++) ((uint8_t *)expected_image.contents)[i] = (uint8_t)(37 + i * 7);
    CHECK(!memcmp(from_image.contents, expected_image.contents, from_image.length), "direct image writes reach the compressed alias bit for bit");
    fill(malias, 93); cs->rf.b[0].rw = 0;
    mtl_sub_begin(); bc_alias_images(cs);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "read-only image submission completes");
    id<MTLBuffer> read_only = blocks_of(mbc);
    CHECK(!memcmp(read_only.contents, from_image.contents, read_only.length), "a read-only image binding does not overwrite compressed storage");

    // Image handles are relative to the execution pool, as encode_args reads
    // them. Raw slot 1 with pool base 1 must still resolve registered alias 2.
    cs->rf.b[0].rw = 1; S.tex_base = 1; S.img[5][0] = 1;
    mtl_sub_begin(); bc_alias_images(cs);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "image alias with a pool offset completes");
    id<MTLBuffer> shifted = blocks_of(mbc);
    for (NSUInteger i = 0; i < expected_image.length; i++) ((uint8_t *)expected_image.contents)[i] = (uint8_t)(93 + i * 7);
    CHECK(!memcmp(shifted.contents, expected_image.contents, shifted.length), "image alias uses the execution pool base");
    S.tex_base = 0; S.img[5][0] = 2;

    printf("t_bc_alias: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
