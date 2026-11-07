// A GPU block encoder's output copied into an RGBA32Uint texture in a CPU_NO_ACCESS pool reaches that storage, so a BC3 texture made over the same storage
// (4x the size, one 16-byte block per texel) reads the blocks at its first use (Cricket 24's kits). An RGBA8 copy into such a pool is no block encoder's and is not read back.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no error", !mtl_last_commit_error()); }

enum { W = 8, POOL = 0x10000 };
typedef struct { void *mem; pool_t pool; } gpool;
static void gpool_new(gpool *g) {
    if (posix_memalign(&g->mem, getpagesize(), POOL)) abort(); memset(g->mem, 0, POOL);
    g->pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = g->mem, .size = POOL, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(g->mem, POOL);
}
static tex_t tex_on(gpool *g, uint64_t serial, int format, unsigned w) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .pool = &g->pool, .mem = g->mem};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = w, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static void copy_run(const tex_t *s, const tex_t *d) {   // one recorded texture copy, run through mtl_exec as a submission would
    void *cmd = NULL; if (posix_memalign(&cmd, getpagesize(), 4096)) abort(); memset(cmd, 0, 4096); mtl_pool_add(cmd, 4096);
    p_copy_t2t x = {.src = s, .dst = d}; x.sr = (NVNcopyRegion){0, 0, 0, (int)s->d.width, (int)s->d.height, 1}; x.dr = x.sr;
    size_t z = (8 + sizeof x + 7) & ~7ull; op_hdr h = {OP_COPY_T2T, (uint32_t)z}; memcpy(cmd, &h, 8); memcpy((uint8_t *)cmd + 8, &x, sizeof x);
    op_hdr end = {OP_END, 8}; memcpy((uint8_t *)cmd + z, &end, 8);
    state_reset(); memset(&R, 0, sizeof R);
    mtl_sub_begin(); uint64_t hh = (uint64_t)cmd; mtl_sub_index(&hh, 1); mtl_exec(cmd); finish();
    mtl_pool_del(cmd); free(cmd);
}
static void level0(id<MTLTexture> t, size_t row, size_t rows, uint8_t *out) {   // a level's bytes: texels, or blocks for a compressed format
    id<MTLBuffer> b = [g_dev newBufferWithLength:row * rows options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "FAIL: no readback buffer\n"); abort(); }
    mtl_sub_begin();
    [blit() copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * rows];
    finish(); memcpy(out, b.contents, row * rows);
}

static void block_encoder(void) {
    gpool src, kit; gpool_new(&src); gpool_new(&kit);
    // The encoder's output: one 16-byte BC3 block per texel, a distinct pattern per block, filled from its storage at first use
    uint8_t blocks[W * W * 16]; for (size_t i = 0; i < sizeof blocks; i++) blocks[i] = (uint8_t)(i * 7 + 3);
    tex_t s = tex_on(&src, 9900, NVN_FORMAT_RGBA32UI, W);
    CHECK("encoder output swizzles", !nvn_swizzle(&s.d, 0, 0, blocks, W * 16, src.mem));
    mtl_tex_init(&s); mtl_sub_begin(); CHECK("encoder output resolves", tex_mtl(&s) != nil); finish();
    tex_t a = tex_on(&kit, 9901, NVN_FORMAT_RGBA32UI, W);   // the 8x8 integer texture at offset 0 of the kit's pool
    mtl_tex_init(&a);
    copy_run(&s, &a);
    CHECK("the copy reached the integer texture's storage", !memcmp(kit.mem, src.mem, a.L.size));
    tex_t b = tex_on(&kit, 9902, NVN_FORMAT_RGBA_DXT5, W * 4);   // the 32x32 BC3 texture the game samples, over the same storage
    CHECK("both describe the same bytes", b.L.size == a.L.size);
    mtl_tex_init(&b); mtl_sub_begin(); id<MTLTexture> bc = tex_mtl(&b); finish();
    CHECK("BC3 texture resolves", bc != nil);
    if (bc) { uint8_t got[sizeof blocks]; level0(bc, W * 16, W, got); CHECK("the BC3 texture holds the encoder's blocks at its first use", !memcmp(got, blocks, sizeof blocks)); }
    mtl_tex_release(&b); mtl_tex_release(&a); mtl_tex_release(&s);
    mtl_pool_del(kit.mem); mtl_pool_del(src.mem); mtl_drain(); free(kit.mem); free(src.mem);
}

static void rgba8_not_read_back(void) {
    gpool src, dst; gpool_new(&src); gpool_new(&dst);
    uint8_t px[W * W * 4]; for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i | 1);
    tex_t s = tex_on(&src, 9910, NVN_FORMAT_RGBA8, W);
    CHECK("RGBA8 source swizzles", !nvn_swizzle(&s.d, 0, 0, px, W * 4, src.mem));
    mtl_tex_init(&s); mtl_sub_begin(); tex_mtl(&s); finish();
    tex_t d = tex_on(&dst, 9911, NVN_FORMAT_RGBA8, W); mtl_tex_init(&d);
    copy_run(&s, &d);
    CHECK("an RGBA8 copy into a CPU_NO_ACCESS pool leaves its storage alone", !mem_any(dst.mem, d.L.size));
    mtl_tex_release(&d); mtl_tex_release(&s);
    mtl_pool_del(dst.mem); mtl_pool_del(src.mem); mtl_drain(); free(dst.mem); free(src.mem);
}

static void switch_off(void) {   // DM_NVN_BLOCK_ALIAS=0: the copy into the encoder's destination is not read back, as before the fix
    setenv("DM_NVN_BLOCK_ALIAS", "0", 1); g_block_alias_on = -1;
    gpool src, kit; gpool_new(&src); gpool_new(&kit);
    uint8_t blocks[W * W * 16]; for (size_t i = 0; i < sizeof blocks; i++) blocks[i] = (uint8_t)(i * 5 + 1);
    tex_t s = tex_on(&src, 9920, NVN_FORMAT_RGBA32UI, W);
    CHECK("off: encoder output swizzles", !nvn_swizzle(&s.d, 0, 0, blocks, W * 16, src.mem));
    mtl_tex_init(&s); mtl_sub_begin(); tex_mtl(&s); finish();
    tex_t a = tex_on(&kit, 9921, NVN_FORMAT_RGBA32UI, W); mtl_tex_init(&a);
    copy_run(&s, &a);
    CHECK("off: the copy is not read back into the storage", !mem_any(kit.mem, a.L.size));
    mtl_tex_release(&a); mtl_tex_release(&s);
    mtl_pool_del(kit.mem); mtl_pool_del(src.mem); mtl_drain(); free(kit.mem); free(src.mem);
    unsetenv("DM_NVN_BLOCK_ALIAS"); g_block_alias_on = -1;
}

int main(void) { @autoreleasepool { mtl_init();
    block_encoder(); rgba8_not_read_back(); switch_off();
    printf("t_block_alias: %d checks, %d failed\n", checks, failed); return failed != 0; } }
