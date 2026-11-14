// A texture the pool walk leaves out for good (the budget is full, no shader has read its ID) holds a stand-in in the tables until its first read: its own smallest levels, or its
// last level cut down to every n-th block when it has none that small (pool_stand, issue #1969). Below the budget the entry stays the dummy, as the walk goes on and makes the
// texture; so does a texture of a type that has no stand-in and an unwritten render target. A read makes the texture and lets the stand-in go, as does another registration.
// DM_NVN_POOL_STANDIN=0: the dummy everywhere, as before
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static uint64_t entry(id<MTLBuffer> b, int i) { return ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }
static uint64_t dummy(id<MTLBuffer> b) { return entry(b, (int)*(const uint32_t *)b.contents); }
static pool_snap *snap(void) { for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf && !g_psnap[k].samp && g_psnap[k].dtt == MTLTextureType2D && g_psnap[k].dpf == NVN_DPF_FLOAT && !g_psnap[k].base) return &g_psnap[k]; return NULL; }
// The 2D float texture table at base 0, in a submission of its own, as a draw that reads it would ask. room0: with this frame's share spent, so the walk makes nothing
static id<MTLBuffer> table(int room0) {
    mtl_sub_begin();
    if (room0) { g_walk_frame = atomic_load(&g_cur_frame); g_walk_spent = POOL_FRAME_BYTES; g_walk_req = 0; }
    __unsafe_unretained id<MTLBuffer> b = nil;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> held = b;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    CHECK(!mtl_last_commit_error(), "the submission has no GPU error");
    return held;
}
static void read_mark(int i) { pool_seen_gpu(); atomic_fetch_or((_Atomic uint32_t *)g_seen_buf.contents + i / 32, 1u << (i % 32)); }
// A texture's storage: every byte of level l of layer y is v + l + 8 * y; pattern: byte k of block (x, y) of level 0 is 3 * x + 5 * y + k instead (its storage is block linear, so through nvn_swizzle)
static uint8_t pat(uint32_t x, uint32_t y, uint32_t k) { return (uint8_t)(3 * x + 5 * y + k); }
static void fill(const tex_t *t, uint8_t v, int pattern) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    for (unsigned l = 0; l < t->d.levels; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == NVN_TEXTURE_TARGET_3D ? MAX(t->d.depth >> l, 1u) : 1; size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        uint8_t *lin = malloc(pitch * rows * z);
        for (unsigned y = 0; y < (t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? t->d.depth : 1); y++) {
            memset(lin, v + (int)l + 8 * (int)y, pitch * rows * z);
            if (pattern && !l) for (size_t r = 0; r < rows; r++) for (size_t i = 0; i < pitch; i++) lin[r * pitch + i] = pat((uint32_t)(i / bpb), (uint32_t)r, (uint32_t)(i % bpb));
            if (nvn_swizzle(&t->d, l, y, lin, pitch, t->mem)) abort();
        }
        free(lin);
    }
}
// The bytes of a level of a layer of a Metal texture, read back on the GPU (n of them, rows of row bytes) into a buffer; nil when the copy failed
static id<MTLBuffer> level_of(id<MTLTexture> t, unsigned level, unsigned slice, size_t n, size_t row) {
    if (!t || level >= t.mipmapLevelCount || slice >= t.arrayLength) return nil;
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) return nil;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(MAX(t.width >> level, 1u), MAX(t.height >> level, 1u), 1)
               toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:n];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? b : nil;
}
static int layer_is(id<MTLTexture> t, unsigned level, unsigned slice, size_t n, size_t row, uint8_t v) {   // every byte v
    id<MTLBuffer> b = level_of(t, level, slice, n, row); if (!b) return 0;
    for (size_t i = 0; i < n; i++) if (((const uint8_t *)b.contents)[i] != v) return 0;
    return 1;
}
static int level_is(id<MTLTexture> t, unsigned level, size_t n, size_t row, uint8_t v) { return layer_is(t, level, 0, n, row, v); }
// Whether a stand-in's one level holds block (step * i + step / 2, step * j + step / 2) of the pattern at its block (i, j)
static int picked_is(id<MTLTexture> t, uint32_t nx, uint32_t ny, uint32_t bpb, uint32_t step) {
    id<MTLBuffer> b = level_of(t, 0, 0, (size_t)nx * ny * bpb, (size_t)nx * bpb); if (!b || t.mipmapLevelCount != 1) return 0;
    const uint8_t *q = b.contents;
    for (uint32_t j = 0; j < ny; j++) for (uint32_t i = 0; i < nx; i++) for (uint32_t k = 0; k < bpb; k++) if (q[((size_t)j * nx + i) * bpb + k] != pat(step * i + step / 2, step * j + step / 2, k)) return 0;
    return 1;
}
static id<MTLTexture> stand(int i) { return (__bridge id<MTLTexture>)g_treg[i].stand; }
int main(void) { @autoreleasepool {
    mtl_init();
    const char *e = getenv("DM_NVN_POOL_STANDIN"); int on = !(e && !strcmp(e, "0"));
    enum { SLOT = 1 << 17, N = 10, LEN = N * SLOT, MANY = STAND_FRAME + 40 };
    uint8_t *mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    pool_t p = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, LEN);
    // 0, 1: RGBA8 64x64 with all 7 levels (16x16, 1024 bytes, is the first a stand-in takes); 2: one level, 16 KB (cut down to every 2nd texel: 32x32, 4096 bytes); 3: BC1 with
    // 7 levels (its 32x32, 512 bytes, is the first); 4: a 2D array of 2 layers; 5: a render target no pass has written; 6: as 0, registered through an sRGB view with red and blue
    // exchanged; 7: as 0, for the IDs past a frame's share; 8: a 3D texture; 9: BC7 256x256 with one level, 64 KB (cut down to every 4th block: 16x16 blocks)
    tex_t t[N] = {0};
    for (int n = 0; n < N; n++) {
        t[n].magic = MAGIC('T', 'E', 'X', 0); t[n].has_layout = 1; t[n].serial = 990200 + n; t[n].pool = &p; t[n].off = n * SLOT; t[n].mem = mem + t[n].off;
        t[n].d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 64, .height = 64, .depth = 1, .levels = 7, .samples = 1};
        if (n == 2) t[n].d.levels = 1;
        if (n == 3) t[n].d.format = NVN_FORMAT_RGBA_DXT1;
        if (n == 4) { t[n].d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t[n].d.depth = 2; }
        if (n == 5) { t[n].has_layout = 0; t[n].d.flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE; t[n].d.levels = 1; }
        if (n == 8) { t[n].d.target = NVN_TEXTURE_TARGET_3D; t[n].d.depth = 2; t[n].d.levels = 1; }
        if (n == 9) { t[n].d.format = NVN_FORMAT_BPTC_UNORM; t[n].d.width = t[n].d.height = 256; t[n].d.levels = 1; }
        nvn_tex_layout(&t[n].d, &t[n].L); if (t[n].L.size > SLOT) abort();
        fill(&t[n], (uint8_t)(0x10 * (n + 1)), n == 2 || n == 9); mtl_tex_init(&t[n]);
    }
    view_t srgb = {.set_format = 1, .format = NVN_FORMAT_RGBA8_SRGB, .set_swizzle = 1, .swz = {MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha}};
    view_t layer1 = {.set_target = 1, .target = NVN_TEXTURE_TARGET_2D, .set_layers = 1, .min_layer = 1, .nlayers = 1};

    // Below the budget a texture this frame has no room for is made by a later walk: the dummy until then, and no stand-in
    mtl_reg_tex(10, &t[0], NULL);
    id<MTLBuffer> b0 = table(1);
    CHECK(entry(b0, 10) == dummy(b0) && !g_treg[10].stand && !g_treg[10].view, "below the budget a texture left out holds the dummy");
    CHECK(snap() && snap()->left > 0 && !snap()->over, "and its table goes on walking");

    // Past the budget no walk makes an unread texture. The table walks once more, and what it left out gets its stand-in
    id<MTLBuffer> hog = [g_dev newBufferWithLength:pool_budget() + (1u << 20) options:MTLResourceStorageModePrivate];
    CHECK(hog && pool_budget_full(), "over the budget");
    mtl_reg_tex(11, &t[2], NULL); mtl_reg_tex(12, &t[3], NULL); mtl_reg_tex(13, &t[4], NULL); mtl_reg_tex(14, &t[5], NULL); mtl_reg_tex(15, &t[6], &srgb);
    mtl_reg_tex(16, &t[4], &layer1); mtl_reg_tex(17, &t[8], NULL); mtl_reg_tex(18, &t[9], NULL);
    uint64_t builds = g_stat_pool_builds; atomic_fetch_add(&g_cur_frame, 1);   // a later frame
    id<MTLBuffer> b1 = table(0);
    if (on) { CHECK(g_stat_pool_builds == builds + 1 && snap()->over, "the budget filled: the table is walked once more"); builds++; }
    else CHECK(g_stat_pool_builds == builds, "DM_NVN_POOL_STANDIN=0: the registrations patch the table, with no walk");
    CHECK(!g_treg[10].view && !tex_get(t[0].serial), "the walk makes no unread texture");
    if (on) {
        id<MTLTexture> s = stand(10);
        CHECK(s && entry(b1, 10) == g_treg[10].stand_rid && entry(b1, 10) != dummy(b1), "the texture left out holds its stand-in");
        CHECK(s.width == 16 && s.height == 16 && s.mipmapLevelCount == 5 && s.pixelFormat == MTLPixelFormatRGBA8Unorm && s.textureType == MTLTextureType2D, "the stand-in is the texture from its 16x16 level on");
        CHECK(level_is(s, 0, 16 * 16 * 4, 64, 0x12) && level_is(s, 1, 8 * 8 * 4, 32, 0x13) && level_is(s, 4, 4, 4, 0x16), "and holds those levels' bytes");
        id<MTLTexture> c = stand(12);
        CHECK(c && entry(b1, 12) == g_treg[12].stand_rid, "a compressed texture has one");
        CHECK(c.width == 32 && c.height == 32 && c.mipmapLevelCount == 6 && c.pixelFormat == MTLPixelFormatBC1_RGBA, "from its 32x32 level on");
        CHECK(level_is(c, 0, 8 * 8 * 8, 64, 0x41) && level_is(c, 5, 8, 8, 0x46), "with those levels' blocks");
        id<MTLTexture> v = stand(15); MTLTextureSwizzleChannels sw = v.swizzle;
        CHECK(v && entry(b1, 15) == g_treg[15].stand_rid && v.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "a registration through an sRGB view has one of the view's format");
        CHECK(sw.red == MTLTextureSwizzleBlue && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleRed && sw.alpha == MTLTextureSwizzleAlpha, "and the view's swizzle");
        id<MTLTexture> k = stand(11);
        CHECK(k && entry(b1, 11) == g_treg[11].stand_rid && k.width == 32 && k.height == 32 && k.mipmapLevelCount == 1, "a texture with no level small enough has its last level cut down: 32x32 of 64x64");
        CHECK(picked_is(k, 32, 32, 4, 2), "every 2nd texel of every 2nd row, from the middle of each cell");
        id<MTLTexture> k7 = stand(18);
        CHECK(k7 && entry(b1, 18) == g_treg[18].stand_rid && k7.width == 64 && k7.height == 64 && k7.pixelFormat == MTLPixelFormatBC7_RGBAUnorm, "a compressed one keeps its format: 16x16 blocks of 64x64");
        CHECK(picked_is(k7, 16, 16, 16, 4), "every 4th block of every 4th block row");
        id<MTLTexture> a = stand(13), a1 = stand(16);
        CHECK(a && a.textureType == MTLTextureType2DArray && a.arrayLength == 2 && layer_is(a, 0, 1, 16 * 16 * 4, 64, 0x5a), "a 2D array has a stand-in of its layers");
        CHECK(entry(b1, 13) == dummy(b1), "which a table of 2D textures does not take");
        CHECK(a1 && a1.textureType == MTLTextureType2D && entry(b1, 16) == g_treg[16].stand_rid && level_is(a1, 0, 16 * 16 * 4, 64, 0x5a), "a layer of it registered as a 2D texture has that layer's");
    } else {
        int none = 1; for (int i = 10; i <= 18; i++) none &= entry(b1, i) == dummy(b1) && !g_treg[i].stand;
        CHECK(none && !g_stand_made, "DM_NVN_POOL_STANDIN=0: the dummy, and no stand-in is made");
    }
    CHECK(entry(b1, 17) == dummy(b1) && !g_treg[17].stand, "a 3D texture holds the dummy");
    CHECK(entry(b1, 14) == dummy(b1) && !g_treg[14].stand, "a render target no pass has written holds the dummy");
    atomic_fetch_add(&g_cur_frame, 1);
    id<MTLBuffer> b2 = table(0);
    CHECK(b2 == b1 && g_stat_pool_builds == builds, "nothing changed: the table stands, in a later frame too");

    // Another texture into the unread ID: the stand-in is the new texture's
    uint64_t rid0 = g_treg[10].stand_rid;
    mtl_reg_tex(10, &t[1], NULL);
    id<MTLBuffer> b3 = table(0);
    if (on) {
        CHECK(stand(10) && entry(b3, 10) == g_treg[10].stand_rid && level_is(stand(10), 0, 16 * 16 * 4, 64, 0x22), "another texture registered into the ID: its stand-in holds the new texture's levels");
        CHECK(entry(b1, 10) == rid0, "the earlier table keeps its entry");
    } else CHECK(entry(b3, 10) == dummy(b3), "DM_NVN_POOL_STANDIN=0: the dummy");
    CHECK(!pool_seen(10) && !g_treg[10].view, "the ID is still unread, and its texture not made");

    // The first read: the scan of the read bitmap makes the texture and writes it into the table in place, and the stand-in goes
    uint64_t first = g_first_stand, firstd = g_first_dummy;
    read_mark(10);
    id<MTLBuffer> b4 = table(0);
    CHECK(g_treg[10].view && entry(b4, 10) == g_treg[10].view_rid && entry(b4, 10) != dummy(b4) && !g_treg[10].stand, "a read makes the texture, and the stand-in goes");
    CHECK(level_is((__bridge id<MTLTexture>)g_treg[10].view, 0, 64 * 64 * 4, 256, 0x20), "the table names the texture itself");
    CHECK(on ? g_first_stand == first + 1 && g_first_dummy == firstd : g_first_stand == first && g_first_dummy == firstd + 1, "the first read is counted: it found a stand-in, or the dummy");

    // An emptied ID and an ID whose pool is finalized hold the dummy, and their stand-ins go
    mtl_reg_tex(12, NULL, NULL); mtl_reg_tex_gone(15);
    id<MTLBuffer> b5 = table(0);
    CHECK(entry(b5, 12) == dummy(b5) && !g_treg[12].stand && entry(b5, 15) == dummy(b5) && !g_treg[15].stand, "an emptied ID and one whose storage went hold the dummy");

    // More registrations than a frame's share of stand-ins: the rest hold the dummy until the next submission, which writes theirs into the table in place
    for (int i = 0; i < MANY; i++) mtl_reg_tex(100 + i, &t[7], NULL);
    uint64_t made = g_stand_made; atomic_fetch_add(&g_cur_frame, 1);
    id<MTLBuffer> b6 = table(0);
    int with = 0; for (int i = 0; i < MANY; i++) with += entry(b6, 100 + i) != dummy(b6);
    if (on) {
        CHECK(g_stand_made == made + STAND_FRAME && g_stand_todo_n == MANY - STAND_FRAME, "a frame makes its share of stand-ins, and the rest wait");
        // (b6's entries are written in place below, so the count is taken before)
        CHECK(with == STAND_FRAME, "the rest hold the dummy for now");
        atomic_fetch_add(&g_cur_frame, 1);
        mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); pool_seen_scan(); os_unfair_lock_unlock(&g_rmu);   // as the next submission's first pool draw does, before it asks for the table
        int all = 1; for (int i = 0; i < MANY; i++) all &= g_treg[100 + i].stand && entry(b6, 100 + i) == g_treg[100 + i].stand_rid;
        CHECK(all && !g_stand_todo_n && g_stand_made == made + MANY, "the next frame makes the rest");
        CHECK(snap()->buf == b6, "written into the table in place: the same buffer");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
        CHECK(level_is(stand(100 + MANY - 1), 0, 16 * 16 * 4, 64, 0x82), "and they hold the texture's levels");
    } else CHECK(!with && !g_stand_made, "DM_NVN_POOL_STANDIN=0: the dummy");

    for (int i = 0; i < MANY; i++) mtl_reg_tex(100 + i, NULL, NULL);
    for (int i = 10; i <= 18; i++) mtl_reg_tex(i, NULL, NULL);
    CHECK(!g_stand_live, "no stand-in is left when every ID is emptied");
    hog = nil;
    for (int n = 0; n < N; n++) mtl_tex_release(&t[n]);
    mtl_pool_del(mem); mtl_drain(); munmap(mem, LEN);
    fprintf(stderr, "t_pool_standin: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
} }
