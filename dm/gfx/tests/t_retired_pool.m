// The set of CPU-written textures (mtl_tex_cpu_written, tex_cpu_written, mtl_tex_release): membership, no duplicates, a released texture leaving it, and lookups of textures
// not in it staying cheap with thousands of consecutive serials in it (a CFSet that hashed serials as numbers probed a whole bucket run per miss: half of Batman: Arkham Knight's
// rendering thread with 6,442 CPU-written textures)
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

// Registered snapshots outlive their objects. Reusing storage must not make a
// whole bindless table walk recreate all obsolete, zero-filled GPU backings.
int main(void) { @autoreleasepool {
    mtl_init();
    enum { SLOTS = 32, ROUNDS = 40, STRIDE = 65536, FIRST = 256 };
    const size_t len = SLOTS * STRIDE;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    NVNmemoryPool pool = {0};
    pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize(&pool, (const NVNmemoryPoolBuilder *)&pb), "GPU pool initialized");
    uint64_t before = g_ghost_puts, take0 = g_alias_taken, walk_puts = 0;
    for (int round = 0; round < ROUNDS; round++) {
        mtl_sub_begin();
        for (int slot = 0; slot < SLOTS; slot++) {
            NVNtextureBuilder tb; NVNtexture tex = {0};
            h_nvnTextureBuilderSetDefaults(&tb);
            h_nvnTextureBuilderSetWidth(&tb, 64 + round);
            h_nvnTextureBuilderSetHeight(&tb, 64);
            h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
            h_nvnTextureBuilderSetStorage(&tb, &pool, (ptrdiff_t)slot * STRIDE);
            CHECK(h_nvnTextureInitialize(&tex, &tb), "texture initialized");
            CHECK(tex_mtl((tex_t *)&tex) != nil, "actual texture object gets storage");
            mtl_reg_tex(FIRST + round * SLOTS + slot, (const tex_t *)&tex, NULL);
            h_nvnTextureFinalize(&tex);
        }
        uint64_t at = g_ghost_puts;
        os_unfair_lock_lock(&g_rmu);
        __unsafe_unretained id<MTLBuffer> table = nil;
        pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
        CHECK(table != nil, "bindless table built");
        os_unfair_lock_unlock(&g_rmu);
        walk_puts += g_ghost_puts - at;
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "round completed");
    }
    printf("retired pool: %d objects, %llu ghost puts (%llu in walks), %llu takeovers, %u aliases\n",
           SLOTS * ROUNDS, (unsigned long long)(g_ghost_puts-before),
           (unsigned long long)walk_puts, (unsigned long long)(g_alias_taken-take0), g_alias.n);
    CHECK(walk_puts == 0, "table walks do not allocate missing zero-filled retired backings");
    CHECK(!mtl_last_commit_error(), "no GPU error");
    for (int i=0;i<SLOTS*ROUNDS;i++) mtl_reg_tex(FIRST+i,NULL,NULL);
    h_nvnMemoryPoolFinalize(&pool); defer_drain(); munmap(mem,len);
    printf("t_retired_pool: %d checks, %d failed\n",checks,failed);
    return failed != 0;
} }
