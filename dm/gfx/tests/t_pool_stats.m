// The pool tables' counts for the power line (nvn_mtl.m mtl_pool_stats) are read on the game's present thread (hle_nvn.c power_line) while the encoding thread rebuilds the
// tables under g_rmu, replacing each table's buffer and releasing the one before (issue #3300: a SIGSEGV in mtl_pool_stats every 10-15 minutes in Ocarina of Time). Here one thread
// builds the 2D float texture table whole, again and again, while another reads the counts. Built with -fsanitize=thread (t_pool_stats_tsan, which
// check_t_pool_stats runs after the plain build), an unlocked read is a data race report; both check the counts the reader sees and what they say once the threads are done
#include <pthread.h>
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

enum { LEN = 1 << 20, ROUNDS = 400 };
static _Atomic int t_stop;
static _Atomic uint64_t t_reads, t_bad;
static void *reader(void *arg) {
    (void)arg;
    while (!atomic_load(&t_stop)) {
        uint64_t s[POOL_STATS]; mtl_pool_stats(s);
        if (s[12] % 0x10000 || s[12] > 0x10000) atomic_fetch_add(&t_bad, 1);   // bytes in tables: none, or the one table's buffer (pool_table_take rounds to 64 KB)
        atomic_fetch_add(&t_reads, 1);
        sched_yield();   // the lock is not fair: leave the walk room to take it
    }
    return NULL;
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_RT_RECYCLE", "0", 1);
    mtl_init();
    g_pool_spare_cap = 0;   // no spares: each table built again lets its earlier buffer go once the submission that used it completes
    uint8_t *ma = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (ma == MAP_FAILED) abort();
    pool_t a = {0}; pool_builder_t pb = {.mem = ma, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&a, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    NVNtextureBuilder tb = {0}; NVNtexture t1 = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&a, 0);
    CHECK(h_nvnTextureInitialize(&t1, &tb), "texture initialized");
    mtl_reg_tex(1, (const tex_t *)&t1, NULL);

    pthread_t th; if (pthread_create(&th, NULL, reader, NULL)) abort();
    uint64_t builds = g_stat_pool_builds;
    for (int i = 0; i < ROUNDS; i++) { @autoreleasepool {
        mtl_reg_tex(2 + (i & 1), (const tex_t *)&t1, NULL); mtl_reg_tex(2 + !(i & 1), NULL, NULL);
        atomic_fetch_add(&g_reg_gen, 1);   // a change without a note: the next walk builds the table whole, into a new buffer
        mtl_sub_begin();
        __unsafe_unretained id<MTLBuffer> b = nil;
        os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b); os_unfair_lock_unlock(&g_rmu);
        CHECK(b != nil, "the walk gives a table");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    } }
    atomic_store(&t_stop, 1); pthread_join(th, NULL);
    CHECK(g_stat_pool_builds - builds >= ROUNDS, "each round built the table whole");
    CHECK(atomic_load(&t_reads) > 0, "the reader read the counts while the tables were rebuilt");
    CHECK(atomic_load(&t_bad) == 0, "each read of the bytes in tables is none or the one table's 64 KB");
    uint64_t s[POOL_STATS]; mtl_pool_stats(s);
    CHECK(s[12] == 0x10000, "once the threads are done, the bytes in tables are the one table's");
    CHECK(s[13] == 0, "no spare bytes are kept with a cap of 0");
    printf("t_pool_stats: %d checks, %d failed (%llu reads)\n", checks, failed, (unsigned long long)atomic_load(&t_reads));
    return failed != 0;
} }
