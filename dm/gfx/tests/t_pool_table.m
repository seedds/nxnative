// The table pool_gpu builds for texture handles traced to no slot (dmsh_build.py pool_arrays): a replaced table becomes a spare that a later rebuild writes again only once the
// submission that could still read it has completed (pool_buf / pool_buf_retire), and an ID whose registration resolved to another texture type is passed over without its view being
// made again (treg_t.tt): a 3D texture in a 2D table, after the submission end dropped its view as it does for a retired registration. A new registration of the ID forgets the type.
// Last, the time that saves on a walk over 20,000 IDs of another type with dropped views, against looking each one up (under a quarter of it; it prints both).
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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static void make_tex(NVNtexture *tex, pool_t *pool, int target, int depth, ptrdiff_t off) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, (NVNtextureTarget)target); h_nvnTextureBuilderSetSize3D(&tb, 4, 4, depth); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)pool, off);
    CHECK(h_nvnTextureInitialize(tex, &tb), "texture initialized");
}
// The table for 2D float textures at base 0, as a draw binding it gets it; the caller holds g_rmu
static id<MTLBuffer> table(void) { __unsafe_unretained id<MTLBuffer> b = nil; pool_gpu(0, MTLTextureType2D, 0, 0, &b); return b; }
static uint64_t entry(id<MTLBuffer> b, int i) { return ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }

int main(int argc, const char **argv) { @autoreleasepool {
    (void)argc; (void)argv;
    mtl_init();
    const size_t len = 1 << 20;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    NVNtexture a = {0}, vol = {0}, b = {0};
    make_tex(&a, &pool, NVN_TEXTURE_TARGET_2D, 1, 0); make_tex(&vol, &pool, NVN_TEXTURE_TARGET_3D, 4, 65536); make_tex(&b, &pool, NVN_TEXTURE_TARGET_2D, 1, 131072);
    mtl_reg_tex(1, (const tex_t *)&a, NULL); mtl_reg_tex(2, (const tex_t *)&vol, NULL); mtl_reg_tex(3, (const tex_t *)&b, NULL);

    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    uint64_t made0 = g_stat_pool_new, reused0 = g_stat_pool_reused;
    id<MTLBuffer> t1 = table();
    CHECK(t1 && g_stat_pool_new == made0 + 1, "the first table is a new buffer");
    CHECK(entry(t1, 1) && entry(t1, 1) == g_treg[1].view_rid && entry(t1, 3) == g_treg[3].view_rid, "2D IDs hold their textures");
    uint64_t dummy = entry(t1, *(const uint32_t *)t1.contents);
    CHECK(entry(t1, 2) == dummy, "the 3D ID holds the dummy");
    CHECK(g_treg[2].tt == MTLTextureType3D + 1 && g_treg[1].tt == MTLTextureType2D + 1, "each registration's type is known after its first lookup");
    treg_drop_views(&g_treg[2]);   // as the submission end does for a retired registration
    os_unfair_lock_unlock(&g_rmu);

    mtl_reg_tex(4, (const tex_t *)&a, NULL);   // a registration: the next use rebuilds
    os_unfair_lock_lock(&g_rmu);
    uint64_t skip0 = g_stat_pool_tt_skip;
    id<MTLBuffer> t2 = table();
    CHECK(t2 && t2 != t1, "a rebuild while the replaced table may still be read writes another buffer");
    CHECK(g_stat_pool_tt_skip > skip0 && !g_treg[2].view, "the 3D ID is passed over without its view being made again");
    CHECK(entry(t2, 2) == dummy && entry(t2, 4) == g_treg[4].view_rid, "the rebuilt table holds the dummy for it and the new ID's texture");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission that could read the first table completes");

    mtl_sub_begin();
    mtl_reg_tex(5, (const tex_t *)&b, NULL);
    os_unfair_lock_lock(&g_rmu);
    id<MTLBuffer> t3 = table();
    CHECK(t3 == t1 && g_stat_pool_reused == reused0 + 1, "the table replaced before the completed submission is written again");
    CHECK(entry(t3, 5) == g_treg[5].view_rid && entry(t3, 1) == g_treg[1].view_rid && entry(t3, 2) == dummy, "the reused table holds this rebuild's entries");
    os_unfair_lock_unlock(&g_rmu);

    mtl_reg_tex(2, (const tex_t *)&b, NULL);   // the ID now holds a 2D texture
    os_unfair_lock_lock(&g_rmu);
    CHECK(g_treg[2].tt == 0, "a new registration forgets the type");
    id<MTLBuffer> t4 = table();
    CHECK(t4 && t4 != t3 && entry(t4, 2) && entry(t4, 2) == g_treg[2].view_rid, "the ID is looked up again and holds its new texture");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the last submission completes");
    CHECK(!mtl_last_commit_error(), "no GPU error");

    {   // What the type saves: a 2D-array table walked over 20,000 2D IDs whose views were dropped (a retired registration's go at every submission end), with the type cleared
        // (each ID looked up and its view made again, as before treg_t.tt) and known (passed over). Ocarina of Time keeps about 12,000 retired descriptors
        enum { NB = 20000, REPS = 10 };
        for (int i = 10; i < 10 + NB; i++) mtl_reg_tex(i, (const tex_t *)&a, NULL);
        mtl_sub_begin();
        double ms[2] = {0, 0};
        for (int known = 0; known < 2; known++) for (int r = 0; r < REPS; r++) {
            os_unfair_lock_lock(&g_rmu);
            for (int i = 10; i < 10 + NB; i++) { if (!known) g_treg[i].tt = 0; else if (!g_treg[i].tt) tex_by_id(i); treg_drop_views(&g_treg[i]); }
            for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf && g_psnap[k].dtt == MTLTextureType2DArray) g_psnap[k].reg = 0;   // out of date: the next use walks every ID
            uint64_t t0 = bp_now(); __unsafe_unretained id<MTLBuffer> tb = nil; pool_gpu(0, MTLTextureType2DArray, 0, 0, &tb); ms[known] += (bp_now() - t0) / 1e6 / REPS;
            os_unfair_lock_unlock(&g_rmu);
        }
        printf("a 2D-array table over %d 2D IDs with dropped views: %.3f ms a walk looking them up, %.3f ms passing over them\n", NB, ms[0], ms[1]);
        CHECK(ms[1] * 4 < ms[0], "passing over IDs of another type takes under a quarter of the time of looking them up");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the walks' submission completes");
        for (int i = 10; i < 10 + NB; i++) mtl_reg_tex(i, NULL, NULL);
    }
    for (int i = 1; i <= 5; i++) mtl_reg_tex(i, NULL, NULL);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    defer_drain(); munmap(mem, len);
    printf("t_pool_table: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
