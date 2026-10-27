// BC textures decoded on the CPU for a device that cannot sample them (nvn_bcdec.c, nvn_mtl.m tex_desc / pkg_upload_ / copy_b2t). A Mac can sample BC, so the test forces the CPU path (DM_TEST_NO_BC=1,
// as an A15 / A16 gets it from supportsBCTextureCompression) and checks, on the Metal device:
//  - a CPU-written 2D-array BC1 texture of 18x10 texels (partial edge blocks), 2 layers, 3 levels is a RGBA8 texture, and every level and layer reads back as the decoded texels;
//  - a GPU-only BC1 texture filled by a buffer-to-texture copy (the blocks are decoded from the buffer's memory) reads back the same;
//  - with the CPU path off (a device that samples BC) the same texture stays a BC1 texture holding the blocks as written.
// Runs on a Mac with Metal 4 (GitHub's virtual Macs have none, issue #112): make -C dm/gfx/tests GAME=<id> check_t_bc_cpu
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { LAYERS = 2 };
static unsigned W = 18, H = 10, LEVELS = 3;   // part 3 uses a size whose blocks are whole (Metal copies whole blocks into a BC texture)
// A BC1 block of one solid colour (both endpoints the same RGB565 value, all indices 0): its texels are that colour exactly, so the expected texels need no second decoder
static uint16_t block_colour(unsigned layer, unsigned level, unsigned bx, unsigned by) { return (uint16_t)(0x0841u * (1 + layer * 7 + level * 3 + bx * 5 + by * 11) + (bx << 11 | by << 5)); }
static void put_block(uint8_t *b, uint16_t c) { b[0] = b[2] = (uint8_t)c; b[1] = b[3] = (uint8_t)(c >> 8); b[4] = b[5] = b[6] = b[7] = 0; }
static void expect_texel(uint16_t c, uint8_t out[4]) {
    unsigned r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    out[0] = (uint8_t)((r * 255 + 15) / 31); out[1] = (uint8_t)((g * 255 + 31) / 63); out[2] = (uint8_t)((b * 255 + 15) / 31); out[3] = 255;   // the exact rounding of the 5 / 6 bit endpoints (D3D10 rule)
}
static void level_size(unsigned l, unsigned *w, unsigned *h) { *w = MAX(W >> l, 1u); *h = MAX(H >> l, 1u); }
// The blocks of one level and layer, tightly packed
static size_t level_blocks(unsigned layer, unsigned level, uint8_t *out) {
    unsigned w, h; level_size(level, &w, &h); unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (unsigned by = 0; by < bh; by++) for (unsigned bx = 0; bx < bw; bx++) put_block(out + (by * bw + bx) * 8, block_colour(layer, level, bx, by));
    return (size_t)bw * bh * 8;
}
// Reads one level and layer of a private Metal texture into `out` (bytes per row `row`)
static void read_level(id<MTLTexture> t, unsigned layer, unsigned level, uint8_t *out, size_t row, size_t rows) {
    id<MTLBuffer> buf = [g_dev newBufferWithLength:row * rows options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    unsigned w, h; level_size(level, &w, &h);
    if (mtl_fmt_block(t.pixelFormat)) { w = (w + 3) / 4 * 4; h = (h + 3) / 4 * 4; }
    [b copyFromTexture:t sourceSlice:layer sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * rows];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    memcpy(out, buf.contents, row * rows);
}
static size_t g_off;
static void make_texture(NVNtexture *tex, NVNmemoryPool *pool, int has_levels) {   // W x H, LAYERS layers, has_levels levels
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D_ARRAY); h_nvnTextureBuilderSetWidth(&tb, W); h_nvnTextureBuilderSetHeight(&tb, H); h_nvnTextureBuilderSetDepth(&tb, LAYERS);
    h_nvnTextureBuilderSetLevels(&tb, has_levels); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA_DXT1);
    size_t size = (h_nvnTextureBuilderGetStorageSize(&tb) + 65535) & ~(size_t)65535;
    h_nvnTextureBuilderSetStorage(&tb, pool, g_off); g_off += size;
    CHECK(h_nvnTextureInitialize(tex, &tb), "BC1 array texture initialized");
}
static void check_texels(id<MTLTexture> mt, const char *what) {
    int bad = 0;
    for (unsigned layer = 0; layer < LAYERS; layer++) for (unsigned level = 0; level < LEVELS; level++) {
        unsigned w, h; level_size(level, &w, &h);
        uint8_t got[64 * 64 * 4];
        read_level(mt, layer, level, got, w * 4, h);
        for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
            uint8_t want[4]; expect_texel(block_colour(layer, level, x / 4, y / 4), want);
            if (memcmp(got + (y * w + x) * 4, want, 4)) { if (!bad++) fprintf(stderr, "%s: layer %u level %u texel %u,%u got %02x%02x%02x%02x want %02x%02x%02x%02x\n", what, layer, level, x, y,
                                                                              got[(y * w + x) * 4], got[(y * w + x) * 4 + 1], got[(y * w + x) * 4 + 2], got[(y * w + x) * 4 + 3], want[0], want[1], want[2], want[3]); }
        }
    }
    CHECK(!bad, what);
}
static void check_blocks(id<MTLTexture> mt, const char *what) {
    int bad = 0;
    for (unsigned layer = 0; layer < LAYERS; layer++) for (unsigned level = 0; level < LEVELS; level++) {
        unsigned w, h; level_size(level, &w, &h); uint8_t want[64], got[64];
        size_t n = level_blocks(layer, level, want), row = (w + 3) / 4 * 8;
        read_level(mt, layer, level, got, row, (h + 3) / 4);
        bad += memcmp(got, want, n) != 0;
    }
    CHECK(!bad, what);
}
int main(void) { @autoreleasepool {
    setenv("DM_TEST_NO_BC", "1", 1);
    mtl_init();
    CHECK(g_bc_cpu, "DM_TEST_NO_BC=1 turns the CPU decode on");
    const size_t len = 4 << 20;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    const size_t src_off = len - 65536;   // blocks for the copy test: level after level, layer after layer

    // 1. CPU-written (packaged) texture: WriteTexels of every level and layer, then resolved
    NVNtexture tex = {0}; make_texture(&tex, (NVNmemoryPool *)&pool, LEVELS);
    for (unsigned layer = 0; layer < LAYERS; layer++) for (unsigned level = 0; level < LEVELS; level++) {
        unsigned w, h; level_size(level, &w, &h); uint8_t blocks[64];
        level_blocks(layer, level, blocks);
        NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, level, 1); h_nvnTextureViewSetLayers(&v, layer, 1);
        NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, 1};
        h_nvnTextureWriteTexels(&tex, &v, &r, blocks);
    }
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    mtl_sub_begin(); id<MTLTexture> mt = tex_by_id(1);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "upload completed");
    CHECK(mt && mt.pixelFormat == MTLPixelFormatRGBA8Unorm && mt.width == W && mt.height == H && mt.arrayLength == LAYERS && mt.mipmapLevelCount == LEVELS, "a BC1 texture is made RGBA8 with its size, layers and levels");
    if (mt) check_texels(mt, "CPU-written BC1: every level and layer reads back as the decoded texels");

    // 2. GPU-only texture filled by buffer-to-texture copies from the pool
    NVNtexture gtex = {0}; make_texture(&gtex, (NVNmemoryPool *)&pool, LEVELS);
    NVNcommandBuffer cmd; CHECK(h_nvnCommandBufferInitialize(&cmd, NULL), "command buffer initialized");
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, len - 131072, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    // the blocks of each level and layer in a slot of their own in the pool, copied one by one
    size_t slot[LAYERS][LEVELS], so = src_off;
    for (unsigned layer = 0; layer < LAYERS; layer++) for (unsigned level = 0; level < LEVELS; level++) { slot[layer][level] = so; so += (level_blocks(layer, level, mem + so) + 255) & ~(size_t)255; }
    for (unsigned layer = 0; layer < LAYERS; layer++) for (unsigned level = 0; level < LEVELS; level++) {
        unsigned w, h; level_size(level, &w, &h);
        NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, level, 1); h_nvnTextureViewSetLayers(&v, layer, 1);
        NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, 1};
        h_nvnCommandBufferCopyBufferToTexture(&cmd, (uint64_t)(uintptr_t)mem + slot[layer][level], &gtex, &v, &r, 0);
    }
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
    mtl_reg_tex(2, (const tex_t *)&gtex, NULL);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)handle);
    id<MTLTexture> gt = tex_by_id(2);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "copies completed");
    CHECK(gt && gt.pixelFormat == MTLPixelFormatRGBA8Unorm, "the GPU-only BC1 texture is RGBA8 too");
    if (gt) check_texels(gt, "buffer-to-texture copies into BC1: decoded from the buffer's blocks");

    // 3. A device that samples BC keeps the blocks: the same texture with the CPU path off
    g_bc_cpu = 0; W = 16; H = 8; LEVELS = 2;
    NVNtexture btex = {0}; make_texture(&btex, (NVNmemoryPool *)&pool, LEVELS);
    for (unsigned layer = 0; layer < LAYERS; layer++) for (unsigned level = 0; level < LEVELS; level++) {
        unsigned w, h; level_size(level, &w, &h); uint8_t blocks[64]; level_blocks(layer, level, blocks);
        NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, level, 1); h_nvnTextureViewSetLayers(&v, layer, 1);
        NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, 1};
        h_nvnTextureWriteTexels(&btex, &v, &r, blocks);
    }
    mtl_reg_tex(3, (const tex_t *)&btex, NULL);
    mtl_sub_begin(); id<MTLTexture> bt = tex_by_id(3);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "BC upload completed");
    CHECK(bt && bt.pixelFormat == MTLPixelFormatBC1_RGBA, "with the CPU path off a BC1 texture stays BC1");
    if (bt) check_blocks(bt, "BC1 uploaded as blocks: every level and layer reads back as written");
    mtl_reg_tex(1, NULL, NULL); mtl_reg_tex(2, NULL, NULL); mtl_reg_tex(3, NULL, NULL);
    defer_drain(); munmap(mem, len);
    printf("t_bc_cpu: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
