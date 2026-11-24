// A texture viewed on a sparse pool's run buffer follows the run. A mapping beside a run, or an unmap inside it, replaces the run's buffer (vrun_set) even over pages it does not
// change, and the old buffer leaves the residency set at its deferred release while a linear or buffer texture still names it (the shape of the fault in GitHub issue #3229, not
// confirmed to be this). Moving the view costs one lookup, and only views on a replaced buffer are noted.
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
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
enum { LIN_ID = 6, BUF_ID = 9, RUN = 1 << 20, PIECE = 0x10000, ROW = 256, ROWS = 16 };
static pool_t g_phys, g_sparse;
static NVNtexture g_lin, g_buf, g_tw_lin, g_tw_srgb, g_clip, g_same, g_far, g_late_srgb, g_tall, g_lrt, g_pin_lin, g_pin_srgb, g_many[80];
static uint8_t *g_phys_mem;

static void map_at(uint64_t voff, uint64_t size, int mapped) {
    map_req r = {.phys = mapped ? (NVNmemoryPool *)&g_phys : NULL, .poff = mapped ? (int64_t)voff : 0, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
// One submission that walks the texture tables of both kinds of view: the table entry of each ID
static void walk(uint64_t *lin, uint64_t *buf) {
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    __unsafe_unretained id<MTLBuffer> b = nil; pool_gpu(0, (int)MTLTextureType2D, NVN_DPF_FLOAT, 0, &b);
    const uint8_t *c = b.contents; uint32_t n = *(const uint32_t *)c; const uint64_t *e = (const uint64_t *)(c + DMSH_POOL_HEAD);
    *lin = LIN_ID < n ? e[LIN_ID] : 0;
    b = nil; pool_gpu(0, (int)MTLTextureTypeTextureBuffer, NVN_DPF_UINT, 0, &b);
    c = b.contents; n = *(const uint32_t *)c; e = (const uint64_t *)(c + DMSH_POOL_HEAD);
    *buf = BUF_ID < n ? e[BUF_ID] : 0;
    os_unfair_lock_unlock(&g_rmu);
    CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
}
static id<MTLBuffer> run_buffer(const tex_t *t, uint64_t size) {
    void *mb = NULL; uint64_t mo = 0;
    CHECK("the texture's storage is in a run", mtl_addr((uint64_t)(uintptr_t)t->mem, size, &mb, &mo));
    return (__bridge id<MTLBuffer>)mb;
}
// The texels the GPU holds: an uploaded texture is private storage
static void read_texture(id<MTLTexture> t, uint8_t *out, unsigned rows) {
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLBuffer> b = [g_dev newBufferWithLength:ROW * rows options:MTLResourceStorageModeShared];
    if (!b) { CHECK("a readback buffer", 0); memset(out, 0, ROW * rows); return; }
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, rows, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:ROW destinationBytesPerImage:ROW * rows];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; memcpy(out, b.contents, ROW * rows);
}
static id<MTLBuffer> view_buffer(int id_) {
    id<MTLTexture> v = (__bridge id<MTLTexture>)g_treg[id_].view;
    while (v.parentTexture) v = v.parentTexture;
    return v.buffer;
}
static void check_views(const char *when, const tex_t *L, const tex_t *B, uint64_t lin, uint64_t buf) {
    char label[160];
    snprintf(label, sizeof label, "%s: the linear texture views the run's current buffer", when);
    CHECK(label, view_buffer(LIN_ID) == run_buffer(L, ROW * ROWS));
    // and at its own place in it: the view reads the bytes of the texture's storage
    id<MTLTexture> v = (__bridge id<MTLTexture>)g_treg[LIN_ID].view; while (v.parentTexture) v = v.parentTexture;
    uint8_t got[ROW * ROWS]; memset(got, 0, sizeof got);
    if (v) [v getBytes:got bytesPerRow:ROW fromRegion:MTLRegionMake2D(0, 0, 64, ROWS) mipmapLevel:0];
    snprintf(label, sizeof label, "%s: the linear view reads its storage", when);
    CHECK(label, v && !memcmp(got, g_phys_mem + RUN, sizeof got));
    snprintf(label, sizeof label, "%s: its table entry is that view", when);
    CHECK(label, lin == g_treg[LIN_ID].view_rid);
    snprintf(label, sizeof label, "%s: the buffer texture views the run's current buffer", when);
    CHECK(label, view_buffer(BUF_ID) == run_buffer(B, 1024));
    snprintf(label, sizeof label, "%s: its table entry is that view", when);
    CHECK(label, buf == g_treg[BUF_ID].view_rid);
}

int main(void) { @autoreleasepool {
    mtl_init();
    size_t plen = 16u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen; i++) phys[i] = (uint8_t)(i * 7 + (i >> 12) + 1);
    g_phys_mem = phys;
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    map_at(RUN, PIECE, 1);

    NVNtextureBuilder tb;
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, ROWS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, RUN);
    CHECK("linear texture", h_nvnTextureInitialize(&g_lin, &tb));
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_BUFFER); h_nvnTextureBuilderSetSize1D(&tb, 256);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32UI); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, RUN + 0x4000);
    CHECK("buffer texture", h_nvnTextureInitialize(&g_buf, &tb));
    const tex_t *L = (const tex_t *)&g_lin, *B = (const tex_t *)&g_buf;
    mtl_reg_tex(LIN_ID, L, NULL); mtl_reg_tex(BUF_ID, B, NULL);
    atomic_store(&g_cur_frame, 100);

    uint64_t lin = 0, buf = 0;
    walk(&lin, &buf);
    check_views("first use", L, B, lin, buf);
    id<MTLBuffer> first = run_buffer(L, ROW * ROWS);

    // 1. a mapping beside the run joins it: a new buffer, with no request over either texture's pages
    map_at(RUN + PIECE, PIECE, 1);
    CHECK("the join replaced the run's buffer", run_buffer(L, ROW * ROWS) != first);
    walk(&lin, &buf);
    check_views("after a mapping beside the run", L, B, lin, buf);

    // 2. an unmap of the run's far end splits it: again a new buffer, again over no page of theirs
    id<MTLBuffer> joined = run_buffer(L, ROW * ROWS);
    map_at(RUN + PIECE + PIECE / 2, PIECE / 2, 0);
    CHECK("the unmap replaced the run's buffer", run_buffer(L, ROW * ROWS) != joined);
    walk(&lin, &buf);
    check_views("after an unmap at the run's end", L, B, lin, buf);

    // 3. a mapping before the run joins it from below: the textures' offsets in the run's buffer move as well
    id<MTLBuffer> split = run_buffer(L, ROW * ROWS);
    map_at(RUN - PIECE, PIECE, 1);
    CHECK("the join from below replaced the run's buffer", run_buffer(L, ROW * ROWS) != split);
    walk(&lin, &buf);
    check_views("after a mapping before the run", L, B, lin, buf);
    { id<MTLTexture> v = (__bridge id<MTLTexture>)g_treg[LIN_ID].view; while (v.parentTexture) v = v.parentTexture;
      CHECK("the linear view starts a mapping's length into the joined run", v.bufferOffset == PIECE); }

    // 4. a linear texture and its sRGB twin over one storage: the twin is a view of the first one resolved, and when the twin is the first to be resolved after a run change, the
    //    texture under it moves to the run's buffer and the twin's view is made again on it
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, ROWS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, RUN + 0x8000);
    CHECK("twin: linear texture", h_nvnTextureInitialize(&g_tw_lin, &tb));
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    CHECK("twin: sRGB texture", h_nvnTextureInitialize(&g_tw_srgb, &tb));
    const tex_t *TL = (const tex_t *)&g_tw_lin, *TS = (const tex_t *)&g_tw_srgb;
    mtl_sub_begin();
    id<MTLTexture> b0 = tex_mtl(TL), v0 = tex_mtl(TS);
    CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("twin: the sRGB texture is a view of the linear one, on the run's buffer", b0 && v0.parentTexture == b0 && b0.buffer == run_buffer(TL, ROW * ROWS));
    map_at(RUN + PIECE + PIECE / 2, PIECE / 2, 1);   // the far end mapped again: the run's buffer is replaced once more
    CHECK("twin: the run's buffer was replaced", run_buffer(TL, ROW * ROWS) != b0.buffer);
    mtl_sub_begin();
    id<MTLTexture> v1 = tex_mtl(TS), b1 = tex_mtl(TL);
    CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("twin: the linear texture is on the run's current buffer", b1 && b1 != b0 && b1.buffer == run_buffer(TL, ROW * ROWS));
    CHECK("twin: the sRGB view was made again on it", v1 && v1 != v0 && v1.parentTexture == b1 && v1.pixelFormat == v0.pixelFormat);
    CHECK("twin: the new texture keeps the old one's usage", b1.usage == b0.usage && b1.bufferBytesPerRow == b0.bufferBytesPerRow);

    // 4b. a twin first resolved after a run change takes no parent on the buffer the run has left
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, ROWS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, RUN + 0x8000);
    CHECK("late twin: texture", h_nvnTextureInitialize(&g_late_srgb, &tb));
    map_at(RUN + PIECE + PIECE / 2, PIECE / 2, 0);   // the run replaced again, the linear texture of step 4 not resolved since
    mtl_sub_begin();
    id<MTLTexture> late = tex_mtl((const tex_t *)&g_late_srgb); { id<MTLTexture> r = late; while (r.parentTexture) r = r.parentTexture;
    CHECK("late twin: on the run's current buffer", r && r.buffer == run_buffer(TL, ROW * ROWS)); }
    CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));

    // 4c. a buffer texture declared past the run's end is clipped to it, follows the run when it grows, and is not lost when only its tail is unmapped
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_BUFFER); h_nvnTextureBuilderSetSize1D(&tb, 3 * PIECE / 4);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32UI); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, RUN);
    CHECK("clipped: buffer texture", h_nvnTextureInitialize(&g_clip, &tb));
    const tex_t *C = (const tex_t *)&g_clip;
    mtl_sub_begin(); id<MTLTexture> c0 = tex_mtl(C); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("clipped: viewed up to the run's end", c0 && (uint64_t)c0.width * 4 < 3 * PIECE);
    map_at(RUN + PIECE + PIECE / 2, PIECE / 2, 1);   // the run grows
    mtl_sub_begin(); id<MTLTexture> c1 = tex_mtl(C); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("clipped: still resolves after the run is replaced, on its new buffer", c1 && c1 != c0 && c1.buffer == run_buffer(C, 1) && c1.width > c0.width);

    // 4d. a second serial over the same storage takes the view the first one moved, and the old view does not stay resident for it
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, ROWS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, RUN);
    CHECK("same storage: second texture", h_nvnTextureInitialize(&g_same, &tb));
    const tex_t *Q = (const tex_t *)&g_same;
    mtl_sub_begin(); id<MTLTexture> l0 = tex_mtl(L), q0 = tex_mtl(Q); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("same storage: one shared view", l0 && l0 == q0);
    map_at(RUN + PIECE + PIECE / 2, PIECE / 2, 0);
    mtl_sub_begin(); id<MTLTexture> l1 = tex_mtl(L), q1 = tex_mtl(Q); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("same storage: both serials take the one moved view", l1 && l1 != l0 && l1 == q1 && l1.buffer == run_buffer(L, ROW * ROWS));
    l0 = q0 = nil; c0 = nil;

    // 4e. the view's own pages mapped to other memory: it reads the new pages
    uint64_t lin3 = 0, buf3 = 0; walk(&lin3, &buf3);
    map_req over = {.phys = (NVNmemoryPool *)&g_phys, .poff = (int64_t)(RUN + 4 * PIECE), .voff = RUN, .size = PIECE};
    CHECK("mapped over the texture's own pages", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&over));
    walk(&lin3, &buf3);
    { id<MTLTexture> v = (__bridge id<MTLTexture>)g_treg[LIN_ID].view; while (v.parentTexture) v = v.parentTexture;
      uint8_t got[ROW * ROWS]; memset(got, 0, sizeof got); if (v) [v getBytes:got bytesPerRow:ROW fromRegion:MTLRegionMake2D(0, 0, 64, ROWS) mipmapLevel:0];
      CHECK("remapped: the linear view reads the new pages", v && !memcmp(got, g_phys_mem + RUN + 4 * PIECE, sizeof got)); }
    map_req back = {.phys = (NVNmemoryPool *)&g_phys, .poff = RUN, .voff = RUN, .size = PIECE};
    CHECK("mapped back", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&back));
    walk(&lin3, &buf3);

    // 4f. a read-only linear texture with a page unmapped: on NVN only that page reads 0. It is taken from its storage from then on, and the page's content comes back with its mapping
    enum { TALL = 512, HOLE_RUN = 4 * RUN };
    map_at(HOLE_RUN, 2 * PIECE, 1);
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, TALL);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, HOLE_RUN);
    CHECK("hole: texture", h_nvnTextureInitialize(&g_tall, &tb));
    const tex_t *H = (const tex_t *)&g_tall;
    mtl_sub_begin(); id<MTLTexture> h0 = tex_mtl(H); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("hole: first a view on its run", h0 && h0.buffer);
    map_at(HOLE_RUN + PIECE, PIECE, 0);
    mtl_sub_begin(); id<MTLTexture> h1 = tex_mtl(H); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("hole: a texture of its own, not nothing", h1 && !h1.buffer && h1.width == 64 && h1.height == TALL);
    uint8_t *hb = malloc(ROW * TALL), zero[PIECE]; memset(zero, 0, sizeof zero);
    read_texture(h1, hb, TALL);
    CHECK("hole: the mapped rows keep their content", !memcmp(hb, g_phys_mem + HOLE_RUN, PIECE));
    CHECK("hole: the unmapped page reads 0", !memcmp(hb + PIECE, zero, PIECE));
    map_at(HOLE_RUN + PIECE, PIECE, 1);
    mtl_sub_begin(); id<MTLTexture> h2 = tex_mtl(H); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    read_texture(h2, hb, TALL);
    CHECK("hole: the page's content is back with its mapping", h2 && !memcmp(hb, g_phys_mem + HOLE_RUN, 2 * PIECE));
    free(hb); h0 = h1 = h2 = nil;

    // 4f2. one the GPU may write has no copy to read through: nothing until the mapping is back, then its view
    enum { LRT_RUN = 5 * RUN };
    map_at(LRT_RUN, PIECE, 1);
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, ROWS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, LRT_RUN);
    CHECK("render target: texture", h_nvnTextureInitialize(&g_lrt, &tb));
    const tex_t *R = (const tex_t *)&g_lrt;
    mtl_sub_begin(); id<MTLTexture> r0 = tex_mtl(R); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("render target: a view on its run", r0 && r0.buffer);
    map_at(LRT_RUN, PIECE, 0);
    mtl_sub_begin(); id<MTLTexture> r1 = tex_mtl(R); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("render target: unmapped, no texture", r1 == nil);
    map_at(LRT_RUN, PIECE, 1);
    mtl_sub_begin(); id<MTLTexture> r2 = tex_mtl(R); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("render target: mapped again, on the run's current buffer", r2 && r2.buffer == run_buffer(R, ROW * ROWS));
    r0 = r2 = nil;

    // 4f3. a pinned sRGB twin whose linear serial moved first: its new parent goes with its last entry, it does not stay resident
    enum { PIN_RUN = 6 * RUN };
    map_at(PIN_RUN, PIECE, 1);
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize2D(&tb, 64, ROWS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, ROW);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, PIN_RUN);
    CHECK("pinned twin: linear", h_nvnTextureInitialize(&g_pin_lin, &tb));
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    CHECK("pinned twin: sRGB", h_nvnTextureInitialize(&g_pin_srgb, &tb));
    const tex_t *PL = (const tex_t *)&g_pin_lin, *PS = (const tex_t *)&g_pin_srgb;
    mtl_sub_begin(); id<MTLTexture> pb0 = tex_mtl(PL), pv0 = tex_mtl(PS); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("pinned twin: a view of the linear texture", pb0 && pv0.parentTexture == pb0);
    mtl_tex_pin(PS->serial);
    map_at(PIN_RUN + PIECE, PIECE, 1);
    mtl_sub_begin(); id<MTLTexture> pb1 = tex_mtl(PL), pv1 = tex_mtl(PS); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    id<MTLTexture> parent = pv1.parentTexture;
    CHECK("pinned twin: both on the run's new buffer", pb1 && pb1.buffer == run_buffer(PL, ROW * ROWS) && parent && parent.buffer == pb1.buffer);
    int two = parent != pb1;   // the case under test: a second parent, in no sharing entry
    CHECK("pinned twin: its view went to a second parent", two);
    mtl_tex_unpin(PS->serial); h_nvnTextureFinalize(&g_pin_srgb);
    pv0 = pv1 = nil; pb0 = nil;
    for (int i = 0; i < 3; i++) { uint64_t a = 0, b2 = 0; walk(&a, &b2); mtl_finish(); }
    if (two) { mtl_sub_begin(); CHECK("the submission completed", mtl_wait(mtl_sub_commit(), 5000000000ull)); mtl_finish();
        pthread_mutex_lock(&g_res_mu); int still = [g_res_objects containsObject:parent]; pthread_mutex_unlock(&g_res_mu);
        CHECK("pinned twin: its parent leaves the residency set with its last entry", !still); }
    parent = nil; pb1 = nil;

    // 4f4. more views in replaced runs than the notes hold: no new epoch, which would send every texture ID down the slow path
    enum { MANY = 80, MANY_RUN = 7 * RUN, MANY_ID = 100 };
    map_at(MANY_RUN, PIECE, 1);
    for (int i = 0; i < MANY; i++) {
        h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_BUFFER); h_nvnTextureBuilderSetSize1D(&tb, 16);
        h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32UI); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, MANY_RUN + (uint64_t)i * 256);
        CHECK("many: buffer texture", h_nvnTextureInitialize(&g_many[i], &tb));
        mtl_reg_tex(MANY_ID + i, (const tex_t *)&g_many[i], NULL);
    }
    { uint64_t a = 0, b2 = 0; walk(&a, &b2); walk(&a, &b2); }
    uint64_t ep0 = g_treg_ep, nn0 = g_run_notes;
    map_at(MANY_RUN + PIECE, PIECE, 1);
    { uint64_t a = 0, b2 = 0; walk(&a, &b2); }
    CHECK("many: all the views on the replaced buffer are noted", g_run_notes - nn0 >= MANY);
    CHECK("many: no new epoch", g_treg_ep == ep0);
    if (g_treg_ep != ep0) fprintf(stderr, "  epochs %llu -> %llu\n", (unsigned long long)ep0, (unsigned long long)g_treg_ep);
    for (int i = 0; i < MANY; i++) mtl_reg_tex(MANY_ID + i, NULL, NULL);

    // 4g. a replacement of another run notes no view of this one
    h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_BUFFER); h_nvnTextureBuilderSetSize1D(&tb, 256);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32UI); h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&g_sparse, 8 * RUN);
    CHECK("far run: buffer texture", h_nvnTextureInitialize(&g_far, &tb));
    map_at(8 * RUN, PIECE, 1); walk(&lin3, &buf3);
    uint64_t notes0 = g_run_notes; id<MTLBuffer> near = run_buffer(L, ROW * ROWS);
    map_at(8 * RUN + PIECE, PIECE, 1); walk(&lin3, &buf3);
    CHECK("a replaced run elsewhere leaves this run's views alone", g_run_notes == notes0 && run_buffer(L, ROW * ROWS) == near && view_buffer(LIN_ID) == near);

    // 4h. many replacements: the residency set does not grow, and no replaced buffer stays alive for a view
    NSUInteger res0 = 0; __weak id<MTLBuffer> oldest = nil;
    for (int i = 0; i < 48; i++) {
        map_at(RUN + PIECE + PIECE / 2, PIECE / 2, i & 1);
        if (i == 8) { oldest = run_buffer(L, ROW * ROWS); }
        walk(&lin, &buf);
        if (i == 15) { mtl_finish(); walk(&lin, &buf); res0 = g_res.allocationCount; }
    }
    mtl_finish(); walk(&lin, &buf); mtl_finish(); walk(&lin, &buf);
    NSUInteger res1 = g_res.allocationCount;
    CHECK("many replacements: the residency set does not grow", res1 <= res0 + 1);
    if (res1 > res0 + 1) fprintf(stderr, "  residency set %lu -> %lu allocations\n", (unsigned long)res0, (unsigned long)res1);
    CHECK("many replacements: a replaced run's buffer is released", oldest == nil);

    // 5. no run change: the views are left alone
    walk(&lin, &buf);   // step 4's change, seen by the registered views
    check_views("after the twin's run change", L, B, lin, buf);
    id<MTLTexture> kept = (__bridge id<MTLTexture>)g_treg[LIN_ID].view;
    uint64_t lin2 = 0, buf2 = 0;
    walk(&lin2, &buf2);
    CHECK("no change of run: the linear view stays", (__bridge id<MTLTexture>)g_treg[LIN_ID].view == kept && lin2 == lin && buf2 == buf);

    mtl_reg_tex(LIN_ID, NULL, NULL); mtl_reg_tex(BUF_ID, NULL, NULL);
    printf("t_sparse_run_view: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
