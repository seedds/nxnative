// The texture view cache's lifetime (vc_get / vc_put / vc_commit): views are kept by the Metal texture they view, for as long as the owner holds that texture. A texture the game has
// finalized while one of its IDs is still registered resolves through the ghost table and keeps reading the view it had, with none made again; the ghost table alone keeps it once the
// sharing table lets go; its pool's finalization drops it; and when more textures go than the owner's list of released objects holds, the commit checks every texture it keeps. A buffer
// texture's 2D rows (tbuf_as_2d) first asked for after the game finalized it are kept with it while the owner holds it, not only in the table by what they view (g_tb2_gone)
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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)

static id<MTLTexture> read_id(int tid) {   // one submission that reads the ID; its commit runs vc_commit
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    return v;
}
static int cached(id<MTLTexture> t) { uint64_t a = vc_addr(t); os_unfair_lock_lock(&g_rmu); int r = nvn_hm_get(&g_vc, &a) != NULL; os_unfair_lock_unlock(&g_rmu); return r; }
static int holds(id<MTLTexture> t) { pthread_mutex_lock(&g_smu); int r = own_holds_locked((__bridge const void *)t); pthread_mutex_unlock(&g_smu); return r; }

int main(void) { @autoreleasepool {
    mtl_init();
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture initialized");
    NVNtextureView view; h_nvnTextureViewSetDefaults(&view); h_nvnTextureViewSetLevels(&view, 0, 1);   // a level range: the ID reads a Metal view, not the texture
    mtl_reg_tex(1, (const tex_t *)&tex, (const view_t *)&view);

    id<MTLTexture> live = read_id(1);
    CHECK(live && live.parentTexture, "a live texture's ID reads a view");
    id<MTLTexture> base = live.parentTexture;
    CHECK(cached(base), "the view is kept by the texture it views");
    alias_key ak = tex_alias_key((const tex_t *)&tex);
    uint64_t made = g_vc_new;
    h_nvnTextureFinalize(&tex);
    CHECK(g_treg[1].retired, "finalizing the texture retires its registered ID");

    // The ID outlives the texture: it resolves through the ghost table, to the same Metal texture, and reads the view it had
    id<MTLTexture> first = read_id(1);
    CHECK(first == live && g_vc_new == made, "the finalized texture's ID reads the view it had: none made");
    CHECK(!g_treg[1].view, "the ID's own view goes at the submission's end");
    id<MTLTexture> again = read_id(1);
    CHECK(again == live && g_vc_new == made, "and so does the next submission");

    // The sharing table lets the texture go (a takeover of its storage): the ghost table still holds it, so its views stay
    pthread_mutex_lock(&g_smu); int left = own_alias_del_locked(&ak); pthread_mutex_unlock(&g_smu);
    CHECK(left, "the texture leaves the sharing table");
    id<MTLTexture> ghosted = read_id(1);
    CHECK(holds(base) && cached(base) && ghosted == live && g_vc_new == made, "the ghost table alone holds it, and its view is still read");

    // The pool's finalization drops the views of its textures
    mtl_reg_tex(1, NULL, NULL); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    CHECK(!cached(base), "the pool's finalization drops its textures' views");

    // A buffer texture read as 2D rows only after the game finalized it: the rows are kept with the texture while the ghost table holds it
    {   uint8_t *bm = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (bm == MAP_FAILED) abort();
        pool_t bpool = {0}; pool_builder_t bpb = {.mem=bm, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
        CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&bpool, (const NVNmemoryPoolBuilder *)&bpb), "buffer pool initialized");
        NVNtextureBuilder bb; NVNtexture btex = {0}; h_nvnTextureBuilderSetDefaults(&bb);
        h_nvnTextureBuilderSetTarget(&bb, NVN_TEXTURE_TARGET_BUFFER); h_nvnTextureBuilderSetWidth(&bb, 1024); h_nvnTextureBuilderSetFormat(&bb, NVN_FORMAT_R32F);
        h_nvnTextureBuilderSetStorage(&bb, (NVNmemoryPool *)&bpool, 0);
        CHECK(h_nvnTextureInitialize(&btex, &bb), "buffer texture initialized");
        mtl_reg_tex(2, (const tex_t *)&btex, NULL);
        id<MTLTexture> bt = read_id(2);
        CHECK(bt && bt.buffer, "the live buffer texture's ID reads a buffer texture");
        h_nvnTextureFinalize(&btex);
        mtl_sub_begin(); os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> bt2 = tex_by_id(2); uint64_t rid = 0, made0 = g_tb2_made;
        id<MTLTexture> rows = bt2 ? tbuf_as_2d(2, bt2, NVN_DPF_FLOAT, &rid) : nil;
        id<MTLTexture> root = bt2; while (root.parentTexture) root = root.parentTexture;
        uint64_t ra = vc_addr(root); vc_set *rs = nvn_hm_get(&g_vc, &ra);
        int kept = rows && rs && rs->rows == (__bridge void *)rows;
        id<MTLTexture> rows2 = bt2 ? tbuf_as_2d(2, bt2, NVN_DPF_FLOAT, &rid) : nil;
        os_unfair_lock_unlock(&g_rmu); mtl_wait(mtl_sub_commit(), 5000000000ull);
        CHECK(bt2 && holds(root), "the finalized buffer texture resolves, held by the owner");
        CHECK(kept && g_tb2_made == made0 + 1, "its rows, first asked for after its finalization, are kept with the texture");
        CHECK(rows2 == rows && g_tb2_made == made0 + 1, "and the next bind reads them again: none made");
        mtl_reg_tex(2, NULL, NULL); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&bpool);
        CHECK(!cached(root), "the pool's finalization drops them"); }

    // More objects released between two commits than the owner lists: the commit then checks every texture it keeps, dropping those no longer held and keeping the rest
    {   MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        id<MTLTexture> x = [g_dev newTextureWithDescriptor:d], y = [g_dev newTextureWithDescriptor:d], noise = [g_dev newTextureWithDescriptor:d];
        uint64_t kx = nvn_new_serial(), ky = nvn_new_serial(), kn = nvn_new_serial();
        serial_put(kx, x); serial_put(ky, y);
        vc_key kr = {.pf = MTLPixelFormatRGBA8Unorm, .tt = MTLTextureType2D, .nl = 1, .ns = 1};
        os_unfair_lock_lock(&g_rmu);
        vc_put(x, NULL, &kr, [x newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]); vc_put(y, NULL, &kr, [y newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]);
        os_unfair_lock_unlock(&g_rmu);
        CHECK(cached(x) && cached(y), "two held textures keep views");
        for (int i = 0; i < 65537; i++) { serial_put(kn, noise); serial_del(kn); }   // each release lists the object again: past the list's size
        serial_del(kx);   // released while the list is full: not listed
        CHECK(!holds(x) && holds(y), "one of them is released");
        mtl_sub_begin(); mtl_wait(mtl_sub_commit(), 5000000000ull);
        CHECK(!cached(x) && cached(y), "the commit checks everything it keeps: the released texture's views go, the held one's stay");
        serial_del(ky);
        mtl_sub_begin(); mtl_wait(mtl_sub_commit(), 5000000000ull);
        CHECK(!cached(y), "and the next release is listed again"); }

    printf("t_vc_lifetime: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
