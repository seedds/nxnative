// The view cache (vc_get / vc_put) for a texture the game has finalized while its ID is still registered, which resolves through the ghost table: its views are kept across
// submissions (DM_NVN_VC_KEEP, default on; --off runs it with DM_NVN_VC_KEEP=0, where each submission makes the view again), a texture buffer's rows stay out of its set
// (tbuf_as_2d keeps a finalized texture's rows by what they view, g_tb2_gone), and the sweep at the first commit of a frame drops the sets of serials the ghost table no longer holds
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

static id<MTLTexture> read_id(int tid) {   // one submission that reads the ID
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    return v;
}
static vc_set *fake_ghost_set(uint64_t ser) {   // a finalized texture's set whose serial the ghost table does not hold
    vc_set *s = calloc(1, sizeof *s); s->ghost = 1; nvn_hm_put(&g_vc, &ser, s);
    if (g_vcg_n == g_vcg_cap) { g_vcg_cap = g_vcg_cap ? 2 * g_vcg_cap : 256; g_vcg = realloc_or_die(g_vcg, g_vcg_cap * sizeof *g_vcg, "test"); }
    g_vcg[g_vcg_n++] = ser;
    return s;
}

int main(int argc, char **argv) { @autoreleasepool {
    int off = argc > 1 && !strcmp(argv[1], "--off");
    if (off) setenv("DM_NVN_VC_KEEP", "0", 1);
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
    uint64_t ser = ((tex_t *)&tex)->serial;
    NVNtextureView view; h_nvnTextureViewSetDefaults(&view); h_nvnTextureViewSetLevels(&view, 0, 1);   // a level range: the ID reads a Metal view, not the texture
    mtl_reg_tex(1, (const tex_t *)&tex, (const view_t *)&view);

    id<MTLTexture> live = read_id(1);
    CHECK(live && live.parentTexture, "a live texture's ID reads a view");
    h_nvnTextureFinalize(&tex);
    CHECK(g_treg[1].retired, "finalizing the texture retires its registered ID");

    uint64_t made0 = g_vc_new; id<MTLTexture> first = read_id(1);
    CHECK(first && first.parentTexture && own_ghost_has(ser), "the finalized texture's ID resolves through the ghost table");
    CHECK(!g_treg[1].view, "the ID's own view goes at the submission's end");
    uint64_t made1 = g_vc_new; id<MTLTexture> again = read_id(1); read_id(1);
    os_unfair_lock_lock(&g_rmu); vc_set *s = nvn_hm_get(&g_vc, &ser); os_unfair_lock_unlock(&g_rmu);
    if (!off) {
        CHECK(made1 - made0 == 1, "the first submission after finalization makes the view once");
        CHECK(g_vc_new == made1 && again == first, "later submissions reuse the finalized texture's view");
        CHECK(s && s->ghost, "the set is marked as a finalized texture's");
    } else {
        CHECK(g_vc_new == made1 && again != first && !s, "DM_NVN_VC_KEEP=0: no set is kept, each submission makes its view again");
    }
    os_unfair_lock_lock(&g_rmu);
    CHECK(!vc_set_for(ser, &pool, 0), "a texture buffer's rows are not kept in a finalized texture's set");
    os_unfair_lock_unlock(&g_rmu);

    // The sweep: once a frame, at a commit; a serial the ghost table no longer holds loses its set, one it holds keeps it
    uint64_t gone = nvn_new_serial(), gone2 = nvn_new_serial();
    os_unfair_lock_lock(&g_rmu); fake_ghost_set(gone); os_unfair_lock_unlock(&g_rmu);
    atomic_fetch_add(&g_cur_frame, 1); vc_ghost_commit();
    os_unfair_lock_lock(&g_rmu);
    CHECK(!nvn_hm_get(&g_vc, &gone), "the sweep drops a set whose serial the ghost table let go");
    CHECK(off || nvn_hm_get(&g_vc, &ser) == s, "the sweep keeps a set whose serial the ghost table holds");
    fake_ghost_set(gone2);
    os_unfair_lock_unlock(&g_rmu);
    vc_ghost_commit();
    os_unfair_lock_lock(&g_rmu); CHECK(nvn_hm_get(&g_vc, &gone2) != NULL, "a second commit in the same frame does not sweep"); os_unfair_lock_unlock(&g_rmu);
    atomic_fetch_add(&g_cur_frame, 1); vc_ghost_commit();
    os_unfair_lock_lock(&g_rmu); CHECK(!nvn_hm_get(&g_vc, &gone2) && g_vcg_n == (off ? 0u : 1u), "the next frame's commit sweeps it, and the list keeps the held serial only"); os_unfair_lock_unlock(&g_rmu);

    mtl_reg_tex(1, NULL, NULL); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    os_unfair_lock_lock(&g_rmu); CHECK(!nvn_hm_get(&g_vc, &ser), "the pool's finalization drops the finalized texture's set"); os_unfair_lock_unlock(&g_rmu);
    printf("t_vc_ghost%s: %d checks, %d failed\n", off ? " --off" : "", checks, failed);
    return failed != 0;
} }
