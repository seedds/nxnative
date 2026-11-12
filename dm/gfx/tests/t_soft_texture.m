// Soft textures (nvn_mtl.m tex_soft, DM_NVN_TEX_SOFT=1): a large block-compressed packaged texture is kept without its finest level. Its host texture is half the size with one
// level less, host level n holds the game's level n + 1, and a CPU write uploads it again the same way. A GPU copy that reads or writes the texture makes it whole first
// (soft_harden): a buffer-to-texture copy, a texture-to-texture copy and a copy through an uncompressed alias (bc_alias_copy) then land at the level the game names, reach the
// storage and survive the next upload from it, with no red line. In the half-size host texture they landed one level off, and gsb_store refused the texture. A texture whose
// finest level's storage has gone back (DM_NVN_TEX_RELEASE=1, soft_sweep) cannot be made whole: it stays soft, a copy of level 0 is skipped, and a copy of another level takes
// the level one lower in the host texture
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed, g_err = -1;   // g_err: the test's own stderr, while the host's log lines go to a file (red_lines)
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; dprintf(g_err, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no error", !mtl_last_commit_error()); }
enum { W = 1024, H = 512, LV = 4, RB = 16 * 8, RR = 16 };   // a 1024x512 BC1 texture with 4 levels; a copy region of 64x64 texels: 16 blocks of 8 bytes a row, 16 rows
static size_t lpitch(unsigned l) { return (size_t)(MAX(W >> l, 1u) + 3) / 4 * 8; }
static size_t lrows(unsigned l) { return (MAX(H >> l, 1u) + 3) / 4; }
static uint8_t *ref[LV];   // what every fixture texture holds at first, a level's blocks row by row
static size_t t_pg;
static void pattern(uint8_t *p, size_t n, unsigned seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 131u + seed * 977u + (i >> 8) * 7u) | 1u); }

// The host's log goes to a file the test reads: its lines with [RED] so far that hold `what` (NULL: any). The file is written out after the test's own lines, also when Metal's
// validation stops the test
static FILE *g_log; static int g_log_fd = -1;
static int red_lines(const char *what) {
    if (fseek(g_log, 0, SEEK_SET)) return -1;
    char line[2048]; int n = 0;
    while (fgets(line, sizeof line, g_log)) n += strstr(line, "[RED]") && (!what || strstr(line, what));
    return n;
}
static void log_out(void) { char b[4096]; ssize_t n; lseek(g_log_fd, 0, SEEK_SET); while ((n = read(g_log_fd, b, sizeof b)) > 0) if (write(g_err, b, (size_t)n) < 0) break; }
static void on_abort(int sig) { log_out(); _exit(128 + sig); }
static int log_begin(void) {
    char path[] = "/tmp/t_soft_texture.XXXXXX"; int w = mkstemp(path);
    if (w < 0 || fcntl(w, F_SETFL, O_APPEND) < 0 || !(g_log = fopen(path, "r")) || (g_log_fd = open(path, O_RDONLY)) < 0 || (g_err = dup(2)) < 0 || dup2(w, 2) < 0) return -1;
    unlink(path); close(w); signal(SIGABRT, on_abort);
    return 0;
}
// A soft texture in a pool of its own, its storage holding ref. big: a pool in chunks (BIG_MIN), which has no zero-copy buffer until a lookup reaches it, as a game's large pools:
// only there does a finest level's storage go back (mtl_pool_wrapped)
static tex_t soft_tex(pool_t *pool, uint64_t serial, int big) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = serial};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA_DXT1, .width = W, .height = H, .depth = 1, .levels = LV, .samples = 1}; nvn_tex_layout(&t.d, &t.L);
    size_t size = big ? BIG_MIN : (t.L.size + t_pg - 1) & ~(t_pg - 1);
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (mem == MAP_FAILED) { perror("pool"); abort(); }
    *pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = size, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    t.pool = pool; t.mem = mem;
    mtl_pool_add(mem, size); nvn_dirty_add_range(mem, size);
    for (unsigned l = 0; l < LV; l++) CHECK("fixture swizzle succeeds", !nvn_swizzle(&t.d, l, 0, ref[l], lpitch(l), t.mem));
    return t;
}
static id<MTLTexture> resolve(const tex_t *t) { mtl_sub_begin(); id<MTLTexture> mt = tex_mtl(t); finish(); return mt; }
static int half_size(id<MTLTexture> mt) { return mt && mt.width == W / 2 && mt.height == H / 2 && mt.mipmapLevelCount == LV - 1; }
static int whole(id<MTLTexture> mt) { return mt && mt.width == W && mt.height == H && mt.mipmapLevelCount == LV; }
static id<MTLBuffer> buffer(size_t n) {   // a shared buffer of n bytes; the test stops when Metal makes none
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!b) { dprintf(g_err, "FAIL: no %zu-byte buffer\n", n); exit(1); }
    return b;
}
// The host texture's level hl, read back whole: its blocks row by row
static uint8_t *host_level(id<MTLTexture> mt, unsigned hl, size_t *pitch) {
    NSUInteger w = MAX(mt.width >> hl, 1u), h = MAX(mt.height >> hl, 1u); size_t n = (*pitch = (w + 3) / 4 * 8) * ((h + 3) / 4);
    id<MTLBuffer> b = buffer(n);
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:mt sourceSlice:0 sourceLevel:hl sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:*pitch destinationBytesPerImage:n];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK("host texture readback completes", cb.status == MTLCommandBufferStatusCompleted);
    uint8_t *o = malloc(n); memcpy(o, b.contents, n); return o;
}
// Whether RR rows of RB bytes from byte x0 of a level's rows (pitch apart) equal src
static int region_eq(const uint8_t *lin, size_t pitch, size_t x0, const uint8_t *src) {
    for (size_t y = 0; y < RR; y++) if (memcmp(lin + y * pitch + x0, src + y * RB, RB)) return 0;
    return 1;
}
static int host_region(id<MTLTexture> mt, unsigned hl, size_t x0, const uint8_t *src) { size_t p; uint8_t *got = host_level(mt, hl, &p); int r = region_eq(got, p, x0, src); free(got); return r; }
static int host_is(id<MTLTexture> mt, unsigned hl, const uint8_t *level, size_t n) { size_t p; uint8_t *got = host_level(mt, hl, &p); int r = !memcmp(got, level, n); free(got); return r; }
static int storage_region(const tex_t *t, unsigned level, size_t x0, const uint8_t *src) {   // the same of the storage's level
    uint8_t *lin = malloc(lpitch(level) * lrows(level)); int r = !nvn_deswizzle(&t->d, level, 0, t->mem, lin, lpitch(level)) && region_eq(lin, lpitch(level), x0, src);
    free(lin); return r;
}
// The copies the game makes: 64x64 texels at (x, 0) of a level, from or to guest memory, or from the source texture's level 0
static void b2t(const tex_t *t, unsigned level, int x, const uint8_t *src) {
    p_copy_b2t c = {.src = (uint64_t)(uintptr_t)src, .tex = t, .has_view = 1, .r = {x, 0, 0, 64, 64, 1}}; c.view.set_levels = 1; c.view.base_level = (uint8_t)level;
    copy_b2t(&c, sizeof c);
}
static void t2b(const tex_t *t, unsigned level, int w, int h, uint8_t *dst) {
    p_copy_b2t c = {.src = (uint64_t)(uintptr_t)dst, .tex = t, .has_view = 1, .r = {0, 0, 0, w, h, 1}}; c.view.set_levels = 1; c.view.base_level = (uint8_t)level;
    copy_t2b(&c, sizeof c);
}
static void t2t(const tex_t *s, const tex_t *d, unsigned level, int x) {
    p_copy_t2t c = {.src = s, .dst = d, .has_dv = 1, .sr = {0, 0, 0, 64, 64, 1}, .dr = {x, 0, 0, 64, 64, 1}}; c.dv.set_levels = 1; c.dv.base_level = (uint8_t)level;
    copy_t2t(&c);
}
// The CPU rewrites the texture's last level: its next use uploads it again from its storage
static void cpu_write(const tex_t *t) {
    uint8_t small[lpitch(LV - 1) * lrows(LV - 1)]; pattern(small, sizeof small, 90);
    dm_guest_write_prepare(t->mem, t->L.size); CHECK("swizzle of the rewritten last level", !nvn_swizzle(&t->d, LV - 1, 0, small, lpitch(LV - 1), t->mem));
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TEX_SOFT", "1", 1); setenv("DM_NVN_TEX_RELEASE", "1", 1);
    if (log_begin()) { perror("log file"); return 1; }
    mtl_init();
    t_pg = (size_t)getpagesize();
    for (unsigned l = 0; l < LV; l++) { size_t n = lpitch(l) * lrows(l); ref[l] = malloc(n); pattern(ref[l], n, l + 1); }
    static pool_t pools[8], side;
    // Beside the textures: a copy's source bytes, the storage of a copy's source texture, and where a texture-to-buffer copy writes
    size_t n0 = lpitch(0) * lrows(0), n1 = lpitch(1) * lrows(1), side_size = 4 * t_pg + ((n0 + t_pg - 1) & ~(t_pg - 1)); void *smem = NULL; if (posix_memalign(&smem, t_pg, side_size)) abort(); memset(smem, 0, side_size);
    side = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .mem = smem, .size = side_size, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(smem, side_size); nvn_dirty_add_range(smem, side_size);
    uint8_t *B = smem, *out = (uint8_t *)smem + 4 * t_pg; for (size_t i = 0; i < RB * RR; i++) B[i] = (uint8_t)(0xB0 | (i & 15));
    tex_t s = {.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = 880100, .pool = &side, .mem = (uint8_t *)smem + 2 * t_pg};
    s.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA_DXT1, .width = 64, .height = 64, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&s.d, &s.L);
    uint8_t Sb[RB * RR]; for (size_t i = 0; i < sizeof Sb; i++) Sb[i] = (uint8_t)(0x50 | (i & 15));
    CHECK("swizzle of the copy source", !nvn_swizzle(&s.d, 0, 0, Sb, RB, s.mem));
    CHECK("a small texture is not soft", !tex_soft(&s));

    // The upload: half the size, each level one down; a CPU write uploads it again the same way
    {
        tex_t t = soft_tex(&pools[0], 880001, 0);
        CHECK("a large block-compressed packaged texture is soft", tex_soft(&t));
        id<MTLTexture> img = resolve(&t);
        CHECK("its host texture is half the size with one level less", half_size(img));
        for (unsigned hl = 0; hl + 1 < LV; hl++) CHECK("each host level holds the game's next level", host_is(img, hl, ref[hl + 1], lpitch(hl + 1) * lrows(hl + 1)));
        uint64_t re = g_pkg_reuploads;
        CHECK("unchanged storage keeps its host texture", resolve(&t) == img && g_pkg_reuploads == re);
        uint8_t l2[lpitch(2) * lrows(2)]; pattern(l2, sizeof l2, 40);
        { uint64_t lo = nvn_tex_level_offset(&t.d, 2); dm_guest_write_prepare((uint8_t *)t.mem + lo, nvn_tex_level_offset(&t.d, 3) - lo); }   // the level's own pages, as a game writes a piece of a texture
        CHECK("swizzle of the rewritten level 2", !nvn_swizzle(&t.d, 2, 0, l2, lpitch(2), t.mem));
        img = resolve(&t);
        CHECK("a CPU write uploads it again", g_pkg_reuploads == re + 1);
        CHECK("still half the size", half_size(img) && tex_soft(&t));
        CHECK("host level 1 holds the rewritten level 2", host_is(img, 1, l2, sizeof l2));
        CHECK("host level 0 still holds level 1", host_is(img, 0, ref[1], n1));
        // The partial re-upload (pkg_written, DM_NVN_PARTIAL_UPLOAD) asks about the byte range of the game's level, host level + 1: only the rewritten one is read, the others are copied from the replaced texture
        __unsafe_unretained id o = tex_get(t.serial); NvPkg *pk = [o isKindOfClass:[NvPkg class]] ? o : nil;
        CHECK("only the rewritten level was read from the storage", pk && pk->part);
        for (unsigned hl = 2; hl + 1 < LV; hl++) CHECK("the levels below it are as before", host_is(img, hl, ref[hl + 1], lpitch(hl + 1) * lrows(hl + 1)));
    }
    // A buffer-to-texture copy into the game's level 1
    {
        tex_t t = soft_tex(&pools[1], 880002, 0);
        CHECK("soft before the copy", half_size(resolve(&t)));
        mtl_sub_begin(); b2t(&t, 1, 0, B); finish();
        id<MTLTexture> img = resolve(&t);
        CHECK("a buffer copy into it makes it whole", whole(img) && !tex_soft(&t));
        CHECK("the buffer copy is at the game's level 1", host_region(img, 1, 0, B));
        CHECK("and not one level off", host_is(img, 2, ref[2], lpitch(2) * lrows(2)));
        CHECK("the finest level came with the whole upload", host_is(img, 0, ref[0], n0));
        CHECK("the storage's level 1 holds the buffer copy", storage_region(&t, 1, 0, B));
        uint64_t re = g_pkg_reuploads; cpu_write(&t); img = resolve(&t);
        CHECK("the buffer copy survives the next upload from the storage", g_pkg_reuploads == re + 1 && whole(img) && host_region(img, 1, 0, B));
    }
    // A texture-to-texture copy into the game's level 1
    {
        tex_t t = soft_tex(&pools[2], 880003, 0);
        CHECK("soft before the copy", half_size(resolve(&t)));
        mtl_sub_begin(); t2t(&s, &t, 1, 64); finish();
        id<MTLTexture> img = resolve(&t);
        CHECK("a texture copy into it makes it whole", whole(img) && !tex_soft(&t));
        CHECK("the texture copy is at the game's level 1", host_region(img, 1, RB, Sb));
        CHECK("and not one level off", host_is(img, 2, ref[2], lpitch(2) * lrows(2)));
        CHECK("the storage's level 1 holds the texture copy", storage_region(&t, 1, RB, Sb));
        uint64_t re = g_pkg_reuploads; cpu_write(&t); img = resolve(&t);
        CHECK("the texture copy survives the next upload from the storage", g_pkg_reuploads == re + 1 && whole(img) && host_region(img, 1, RB, Sb));
    }
    // A copy from it: the level the host texture did not have
    {
        tex_t t = soft_tex(&pools[3], 880004, 0);
        CHECK("soft before the copy", half_size(resolve(&t)));
        memset(out, 0xcd, n0);
        mtl_sub_begin(); t2b(&t, 0, W, H, out); finish();
        CHECK("a copy from it makes it whole", whole(resolve(&t)) && !tex_soft(&t));
        CHECK("the copy holds the game's level 0", !memcmp(out, ref[0], n0));
    }
    // A buffer copy into it and a copy from it in one command buffer: the second sees the first, and so does the storage (soft_harden's upload has no GPU store to wait for)
    {
        tex_t t = soft_tex(&pools[4], 880005, 0);
        CHECK("soft before the copies", half_size(resolve(&t)));
        memset(out, 0xcd, n1);
        mtl_sub_begin(); b2t(&t, 1, 0, B); t2b(&t, 1, W / 2, H / 2, out); finish();
        CHECK("the copy out holds the copy in", region_eq(out, lpitch(1), 0, B));
        CHECK("and the level's other rows", !memcmp(out + RR * lpitch(1), ref[1] + RR * lpitch(1), n1 - RR * lpitch(1)));
        CHECK("the storage's level 1 holds the copy in", storage_region(&t, 1, 0, B));
        cpu_write(&t); id<MTLTexture> img = resolve(&t);
        CHECK("which survives the next upload from the storage", whole(img) && host_region(img, 1, 0, B));
    }
    // A copy through an uncompressed alias of it (bc_alias_copy): 16x16 RGBA16UI texels, one a block, into the game's level 1
    {
        tex_t t = soft_tex(&pools[5], 880006, 0);
        tex_t alias = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 880007, .pool = t.pool, .mem = t.mem}, src = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 880008};
        alias.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA16UI, .width = W / 4, .height = H / 4, .depth = 1, .levels = LV, .samples = 1}; nvn_tex_layout(&alias.d, &alias.L);
        src.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA16UI, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&src.d, &src.L);
        mtl_tex_init(&t); mtl_tex_init(&alias); mtl_tex_init(&src); mtl_reg_tex(1, &t, NULL); mtl_reg_tex(2, &alias, NULL);
        CHECK("soft before the copy", half_size(resolve(&t)));
        id<MTLTexture> msrc = resolve(&src);
        CHECK("the alias copy's source is RGBA16UI", msrc && msrc.pixelFormat == MTLPixelFormatRGBA16Uint);
        id<MTLBuffer> fb = buffer(RB * RR); uint8_t *A = fb.contents; for (size_t i = 0; i < RB * RR; i++) A[i] = (uint8_t)(0xA0 | (i & 15));
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
        [ce copyFromBuffer:fb sourceOffset:0 sourceBytesPerRow:RB sourceBytesPerImage:RB * RR sourceSize:MTLSizeMake(16, 16, 1) toTexture:msrc destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        p_copy_t2t c = {.src = &src, .dst = &alias, .has_dv = 1, .sr = {0, 0, 0, 16, 16, 1}, .dr = {0, 0, 0, 16, 16, 1}}; c.dv.set_levels = 1; c.dv.base_level = 1;
        uint64_t na = g_bc_alias_n; mtl_sub_begin(); copy_t2t(&c); finish();
        id<MTLTexture> img = resolve(&t);
        CHECK("the copy went through the alias", g_bc_alias_n == na + 1);
        CHECK("a copy through an alias makes it whole", whole(img) && !tex_soft(&t));
        CHECK("the alias copy is at the game's level 1", host_region(img, 1, 0, A));
        CHECK("and not one level off", host_is(img, 2, ref[2], lpitch(2) * lrows(2)));
    }
    CHECK("no red line so far", !red_lines(NULL));

    // The finest level's storage has gone back (DM_NVN_TEX_RELEASE=1, 300 frames after the upload): the texture cannot be made whole and stays soft
    {
        tex_t t = soft_tex(&pools[6], 880009, 1);
        id<MTLTexture> img = resolve(&t);
        CHECK("soft before the release", half_size(img));
        for (unsigned hl = 0; hl + 1 < LV; hl++) CHECK("uploaded on the CPU, each host level holds the game's next level", host_is(img, hl, ref[hl + 1], lpitch(hl + 1) * lrows(hl + 1)));
        uint64_t given = nvn_dirty_released();
        atomic_fetch_add(&g_cur_frame, SOFT_WAIT + 30); mtl_sub_begin(); finish();
        __unsafe_unretained id o = tex_get(t.serial); NvPkg *pk = [o isKindOfClass:[NvPkg class]] ? o : nil;
        CHECK("its finest level's storage went back", pk && pk->released && nvn_dirty_released() > given);
        mtl_sub_begin(); b2t(&t, 0, 0, B); finish();
        img = resolve(&t);
        CHECK("a copy into it leaves it soft", half_size(img) && tex_soft(&t));
        CHECK("and says so once", red_lines("finest level has gone back") == 1);
        CHECK("a buffer copy into its level 0 is skipped", host_is(img, 0, ref[1], n1));
        mtl_sub_begin(); b2t(&t, 1, 0, B); finish();
        img = resolve(&t);
        CHECK("a buffer copy into its level 1 is at host level 0", half_size(img) && host_region(img, 0, 0, B));
        CHECK("and in the storage's level 1", storage_region(&t, 1, 0, B));
        mtl_sub_begin(); t2t(&s, &t, 1, 64); finish();
        img = resolve(&t);
        CHECK("a texture copy into its level 1 is at host level 0", half_size(img) && host_region(img, 0, RB, Sb));
        CHECK("its storage does not get that one, which is reported", !storage_region(&t, 1, RB, Sb) && red_lines("not written into its storage") == 1);
        memset(out, 0xcd, n1);
        mtl_sub_begin(); t2b(&t, 1, W / 2, H / 2, out); finish();
        CHECK("a copy from its level 1 reads host level 0", region_eq(out, lpitch(1), 0, B) && region_eq(out, lpitch(1), RB, Sb) && !memcmp(out + RR * lpitch(1), ref[1] + RR * lpitch(1), n1 - RR * lpitch(1)));
        memset(out, 0xcd, n0);
        mtl_sub_begin(); t2b(&t, 0, W, H, out); finish();
        int untouched = 1; for (size_t i = 0; i < n0 && untouched; i++) untouched = out[i] == 0xcd;
        CHECK("a copy from its level 0 is skipped", untouched);
        CHECK("it is still soft", half_size(resolve(&t)) && tex_soft(&t));
    }
    log_out();
    dprintf(g_err, "t_soft_texture: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
