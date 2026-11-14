// ReadTexels on a thread other than the submit thread (GitHub issue #1814): the upload batch is the submit thread's (up_owner_check aborts on any other), so a reader never uploads,
// syncs or makes a host texture; it takes the texture's storage whenever that holds the content. Tears of the Kingdom's bake::ModuleMgr reads textures on its own thread: the reader
// found a texture not taken from its storage, the submit thread marked it CPU-written meanwhile, and tex_mtl_ took the packaged path and uploaded on the reader's thread (abort).
// The race is made here by its outcome: each case gives tex_mtl_ the classification the submit thread left, on a thread that is not the submit thread.
//   a: CPU-written, never resolved (the first upload in the report's crash)
//   b: resolved as a GPU texture, then CPU-written (a rewrite the poll found): rebuilt as packaged on the submit thread only
//   c: never resolved, never classified: read from its storage, left unresolved, so its first use on the submit thread still takes it from its storage
// An abort in any case fails the test (the process exits on SIGABRT).
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
#define CHECK(c, label) do { checks++; int ok_ = !!(c); printf("%-80s %s\n", label, ok_ ? "PASS" : "FAIL"); failed += !ok_; } while (0)

enum { W = 64, H = 64, LEN = 1 << 20 };
static uint8_t *g_mem; static pool_t g_pool; static NVNtextureBuilder g_tb;
static uint8_t g_want[W * H * 4];

// The submit thread is a parked helper; the test's own thread plays the game's reader, and becomes the submit thread for a submission (mtl_sub_begin takes the calling thread)
static pthread_t g_helper; static dispatch_semaphore_t g_park;
static void *helper_main(void *a) { (void)a; dispatch_semaphore_wait(g_park, DISPATCH_TIME_FOREVER); return NULL; }
static void as_reader(void) { g_sub_thread = g_helper; atomic_store(&g_sub_thread_set, 1); }
static void submit(const tex_t *t) { mtl_sub_begin(); tex_mtl(t); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "  the submit thread's submission completes"); as_reader(); }

// Initializes a texture at off whose storage holds the pattern, written before the texture exists (a scratch object over the same storage gives the layout), so no tracking sees a write
static void make(NVNtexture *tex, size_t off, int fill) {
    h_nvnTextureBuilderSetStorage(&g_tb, (NVNmemoryPool *)&g_pool, (ptrdiff_t)off);
    if (fill) {
        NVNtexture scratch = {0}; CHECK(h_nvnTextureInitialize(&scratch, &g_tb), "  scratch texture initialized");
        tex_t *s = (tex_t *)&scratch;
        dm_guest_write_prepare(s->mem, s->L.size);
        CHECK(!nvn_swizzle(&s->d, 0, 0, g_want, W * 4, s->mem), "  pattern swizzled into the storage");
        h_nvnTextureFinalize(&scratch);
    }
    CHECK(h_nvnTextureInitialize(tex, &g_tb), "  texture initialized");
}
static int reads_pattern(const tex_t *t) {
    uint8_t got[W * H * 4]; memset(got, 0xEE, sizeof got);
    NVNcopyRegion r = {0, 0, 0, W, H, 1};
    mtl_read_texels(t, 0, 0, &r, got, W * 4, sizeof got);
    return !memcmp(got, g_want, sizeof got);
}
static id resolve_off(const tex_t *t, int *in_storage) { *in_storage = 0; return tex_mtl_(t, in_storage); }

static void case_a(void) {
    printf("a: CPU-written, never resolved\n");
    NVNtexture tex = {0}; make(&tex, 0x10000, 1); tex_t *t = (tex_t *)&tex;
    mtl_tex_cpu_written(t);   // what the submit thread did between the reader's look and its resolve
    int st; id got = resolve_off(t, &st);
    CHECK(!got && st && !tex_get(t->serial), "  off the submit thread: in storage, nothing uploaded");
    CHECK(!tex_mtl(t), "  a present-thread resolve gets no texture rather than uploading");
    CHECK(reads_pattern(t), "  ReadTexels gives the storage");
    submit(t);
    __unsafe_unretained id have = tex_get(t->serial);
    CHECK([have isKindOfClass:[NvPkg class]], "  the submit thread uploads it");
    id<MTLTexture> up = ((NvPkg *)have)->tex;
    CHECK(tex_mtl(t) == up, "  a present-thread resolve then uses the upload as it is");
    got = resolve_off(t, &st);
    CHECK(!got && st && reads_pattern(t), "  a reader still takes the storage");
    h_nvnTextureFinalize(&tex);
}
static void case_b(void) {
    printf("b: GPU texture, then CPU-written\n");
    NVNtexture tex = {0}; make(&tex, 0x30000, 0); tex_t *t = (tex_t *)&tex;
    submit(t);   // zeros at first use: not the CPU's content, a GPU texture
    __unsafe_unretained id have = tex_get(t->serial);
    CHECK(have && ![have isKindOfClass:[NvPkg class]] && !tex_cpu_written(t->serial), "  resolved as a GPU texture");
    dm_guest_write_prepare(t->mem, t->L.size);
    CHECK(!nvn_swizzle(&t->d, 0, 0, g_want, W * 4, t->mem), "  the CPU rewrites the storage");
    mtl_tex_cpu_written(t);   // as the poll marks a rewrite (mtl_tex_cpu_rewritten)
    int st; id got = resolve_off(t, &st);
    CHECK(!got && st, "  off the submit thread: in storage, not rebuilt");
    CHECK(![tex_get(t->serial) isKindOfClass:[NvPkg class]], "  the GPU texture is kept until the submit thread rebuilds it");
    CHECK(reads_pattern(t), "  ReadTexels gives the storage");
    submit(t);
    CHECK([tex_get(t->serial) isKindOfClass:[NvPkg class]], "  the submit thread rebuilds it as packaged");
    h_nvnTextureFinalize(&tex);
}
static void case_c(void) {
    printf("c: never resolved, never classified\n");
    NVNtexture tex = {0}; make(&tex, 0x50000, 1); tex_t *t = (tex_t *)&tex;
    CHECK(!tex_from_storage(t), "  not taken from its storage yet");
    CHECK(reads_pattern(t), "  ReadTexels off the submit thread gives the storage");
    CHECK(!tex_get(t->serial), "  and leaves it unresolved");
    submit(t);
    CHECK(tex_cpu_written(t->serial) && [tex_get(t->serial) isKindOfClass:[NvPkg class]], "  its first use on the submit thread takes it from its storage");
    h_nvnTextureFinalize(&tex);
}

int main(void) { @autoreleasepool {
    mtl_init();
    for (int i = 0; i < W * H * 4; i++) g_want[i] = (uint8_t)(i * 7 + 3);
    g_mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (g_mem == MAP_FAILED) abort();
    pool_builder_t pb = {.mem = g_mem, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_pool, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    h_nvnTextureBuilderSetDefaults(&g_tb); h_nvnTextureBuilderSetWidth(&g_tb, W); h_nvnTextureBuilderSetHeight(&g_tb, H); h_nvnTextureBuilderSetFormat(&g_tb, NVN_FORMAT_RGBA8);
    g_park = dispatch_semaphore_create(0); pthread_create(&g_helper, NULL, helper_main, NULL);
    as_reader();
    case_a(); case_b(); case_c();
    dispatch_semaphore_signal(g_park); pthread_join(g_helper, NULL);
    printf("t_read_owner: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
