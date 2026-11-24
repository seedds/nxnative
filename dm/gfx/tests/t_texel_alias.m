// Uncompressed-alias pairs (nvn_mtl.m, bcp_*): uncompressed textures of other formats over the same guest storage with the same rows of bytes stay equal, as the shared storage keeps
// them on the console. Ocarina of Time 1.0.0's upscaler stores through an R8 image four times as wide as the RGBA8 texture its next pass reads, then stores through an R32UI image
// that the last pass reads as RGB9E5, over storage its scene also took as R11G11B10F: an image store goes into every other format there, the newest registration of each; a
// render pass goes into none; and textures of one format, or of another height, are no pair
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
enum { W = 8, H = 8, ROW = W * 4, SIZE = ROW * H };   // 8x8 texels of 4 bytes: rows of 32 bytes

static id<MTLBuffer> buffer(NSUInteger n) {   // a shared buffer of n bytes; the test stops when Metal makes none
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
static id<MTLBuffer> pattern(uint8_t seed) {   // SIZE bytes, every byte its own value
    id<MTLBuffer> b = buffer(SIZE);
    for (NSUInteger i = 0; i < b.length; i++) ((uint8_t *)b.contents)[i] = (uint8_t)(seed + i * 7);
    return b;
}
static id<MTLBuffer> bytes_of(id<MTLTexture> t) {   // the texture's rows of bytes, whatever its texel size
    id<MTLBuffer> out = buffer(SIZE);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
              toBuffer:out destinationOffset:0 destinationBytesPerRow:ROW destinationBytesPerImage:SIZE];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "the read-back completes");
    return out;
}
static id<MTLTexture> base_of(const tex_t *t) { id<MTLTexture> m = tex_mtl(t); while (m.parentTexture) m = m.parentTexture; return m; }
// A shader's image store into registration slot, in command order: the whole texture from src, then the dispatch's end (bcp_flush)
static void store(NvProg *pr, int slot, const tex_t *t, id<MTLBuffer> src) {
    S.img[5][0] = slot;
    mtl_sub_begin(); image_prepare(pr, 0x20);
    id<MTLTexture> m = bcp_have(t);
    CHECK(m != nil, "the image store makes the image's texture");
    enc_b_copy_buffer_texture(blit(), src, 0, ROW, SIZE, MTLSizeMake(m.width, m.height, 1), m, 0, 0, MTLOriginMake(0, 0, 0)); bcp_flush();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the image store's submission completes");
}

int main(void) { @autoreleasepool {
    mtl_init();
    NvShader *cs = [NvShader new]; NvProg *pr = [NvProg new]; pr->sh[5] = cs;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .reg = 0, .rw = 1};

    // An R8 image four times as wide as an RGBA8 texture over its storage: a store into it reaches the RGBA8 texture byte for byte
    static uint8_t s1[SIZE];
    tex_t rgba = tex2d(NVN_FORMAT_RGBA8, W, H, s1), r8 = tex2d(NVN_FORMAT_R8, W * 4, H, s1);
    mtl_tex_init(&rgba); mtl_tex_init(&r8); mtl_reg_tex(1, &rgba, NULL); mtl_reg_tex(2, &r8, NULL);
    { tex_t o; CHECK(bcp_find(&r8, &o) && o.serial == rgba.serial, "the R8 image's pair is the RGBA8 texture"); }
    id<MTLTexture> mrgba = base_of(&rgba); id<MTLBuffer> p1 = pattern(37);
    store(pr, 2, &r8, p1);
    CHECK(!memcmp(bytes_of(mrgba).contents, p1.contents, SIZE), "the R8 store reaches the RGBA8 texture");

    // An R32UI image over storage taken as RGB9E5 and R11G11B10F too: a store into it reaches both
    static uint8_t s2[SIZE];
    tex_t u = tex2d(NVN_FORMAT_R32UI, W, H, s2), e = tex2d(NVN_FORMAT_RGB9E5F, W, H, s2), f = tex2d(NVN_FORMAT_R11G11B10F, W, H, s2);
    mtl_tex_init(&u); mtl_tex_init(&e); mtl_tex_init(&f); mtl_reg_tex(3, &u, NULL); mtl_reg_tex(4, &e, NULL); mtl_reg_tex(5, &f, NULL);
    id<MTLTexture> me = base_of(&e), mf = base_of(&f); id<MTLBuffer> p2 = pattern(91);
    { tex_t o[BCP_SIDES]; CHECK(bcp_find_all(&u, o, BCP_SIDES) == 2, "the R32UI image has two other sides"); }
    store(pr, 3, &u, p2);
    CHECK(!memcmp(bytes_of(me).contents, p2.contents, SIZE), "the R32UI store reaches the RGB9E5 texture");
    CHECK(!memcmp(bytes_of(mf).contents, p2.contents, SIZE), "and the R11G11B10F texture");

    // An RGB9E5 texture registered again over the same storage (as the game does each frame): the store goes into the newest one, and the sides stay one for each format
    tex_t e2 = tex2d(NVN_FORMAT_RGB9E5F, W, H, s2); mtl_tex_init(&e2); mtl_reg_tex(6, &e2, NULL);
    id<MTLTexture> me2 = base_of(&e2); id<MTLBuffer> p3 = pattern(201);
    { tex_t o[BCP_SIDES]; int n = bcp_find_all(&u, o, BCP_SIDES); int newest = 0; for (int i = 0; i < n; i++) newest |= o[i].serial == e2.serial;
      CHECK(n == 2 && newest, "a format registered again is one side, its newest registration"); }
    store(pr, 3, &u, p3);
    CHECK(!memcmp(bytes_of(me2).contents, p3.contents, SIZE), "the store reaches the newest RGB9E5 texture");

    // A render pass into the R11G11B10F texture (OoT 1.1.1's scene target, over storage it also takes as RGB9E5) copies into no other side: two uncompressed sides follow
    // image stores only
    uint64_t copies = g_bcp_copies;
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &f; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(1, 0, 0, 1);
    mtl_sub_begin(); CHECK(rp_begin() != nil, "the pass into the R11G11B10F texture opens");
    CHECK(g_nbcp == 0, "and notes no side as written");
    rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the pass's submission completes");
    CHECK(g_bcp_copies == copies, "a render pass copies into no other uncompressed side");
    CHECK(!memcmp(bytes_of(me2).contents, p3.contents, SIZE), "and the RGB9E5 texture keeps the store");
    R.has_rt = 0; R.rt.n = 0; R.rt.color[0] = NULL; R.clear_c[0] = 0;

    // Textures of one format over the same storage, or of another height, are no pair
    static uint8_t s3[SIZE];
    tex_t a = tex2d(NVN_FORMAT_R32UI, W, H, s3), b = tex2d(NVN_FORMAT_R32UI, W, H, s3), c = tex2d(NVN_FORMAT_R8, W * 4, H / 2, s3);
    mtl_tex_init(&a); mtl_tex_init(&b); mtl_tex_init(&c); mtl_reg_tex(7, &a, NULL); mtl_reg_tex(8, &b, NULL); mtl_reg_tex(9, &c, NULL);
    { tex_t o; CHECK(!bcp_find(&a, &o), "textures of one format and another height are no pair"); }

    printf("t_texel_alias: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
