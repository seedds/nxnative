// lightfix (DM_NVN_SB_FREEZE_WW, nvn_racediag.inc + nvn_mtl.m sbf_read, 2026-11-07), CPU only: no command buffer is committed.
//  A write-watched range gets an in-flight CPU store -> freeze requested -> frozen at the next commit with versions; a read-only binding reads a copy; no CPU write -> the copy
//  is reused; a CPU store faults once (marks the range written, opens the page, the value lands in memory); the next binding takes a new copy with the new bytes; a range never
//  written on the CPU under a reader (GPU-produced) is never frozen; a store after the reader completed does not freeze.
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
void crash_install(void);
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static pool_t g_pool;
int main(void) { @autoreleasepool {
    size_t plen = 4u << 20;
    uint8_t *mem = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    uint64_t A = (uint64_t)(uintptr_t)(mem + 0x100000 + 0x3400), G = (uint64_t)(uintptr_t)(mem + 0x200000);   // A: CPU-written table (0x40000); G: a "GPU-produced" range
    char ws[128]; snprintf(ws, sizeof ws, "%llx:0x40000,%llx:0x1000", (unsigned long long)A, (unsigned long long)G);
    setenv("DM_NVN_WRITEWATCH", ws, 1); setenv("DM_NVN_SB_FREEZE_WW", "1", 1);
    crash_install();
    mtl_init();
    pool_builder_t pb = {.mem = mem, .size = plen, .flags = 0x24};
    CHECK("pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_pool, (const NVNmemoryPoolBuilder *)&pb));
    CHECK("on", WW_ON() && lfx_on() && ww_auto_on() && atomic_load(&g_wwr_n) == 2);
    ww_range *rA = &g_wwr[0], *rG = &g_wwr[1];
    volatile uint32_t *pa = (volatile uint32_t *)(uintptr_t)A;
    for (int i = 0; i < 0x10000; i++) pa[i] = 0x1000 + i;   // frame N's table (pages not armed yet)
    // 1. no store under a reader: nothing frozen
    NvShader *sh = [NvShader new]; snprintf(sh->key, sizeof sh->key, "5aab38bb5ba67ef7");
    ww_bind(5, sh, 0, 1, A, 0x40000, 1); ww_bind(5, sh, 0, 2, G, 0x1000, 1);
    ww_commit(g_fence + 1);
    CHECK("armed at the commit", atomic_load(&rA->armed) && atomic_load(&rG->armed));
    CHECK("nothing frozen yet", atomic_load(&g_sbf_n) == 0);
    // 2. a store after the reader completed (reader fence <= done): no freeze
    pthread_mutex_lock(&g_ww_mu); ww_arm(rA, 0); pthread_mutex_unlock(&g_ww_mu);
    pa[0x57] = 0xaaaa; ww_commit(g_fence + 7);
    CHECK("completed reader: not frozen", atomic_load(&g_sbf_n) == 0 && pa[0x57] == 0xaaaa && !atomic_load(&rA->freeze_req));
    // 3. a CPU store while the reader is in flight: freeze requested, frozen at the next commit
    pthread_mutex_lock(&g_ww_mu); ww_arm(rA, g_done + 5); pthread_mutex_unlock(&g_ww_mu);
    pa[0x15c] = 0x2000;
    CHECK("in-flight store: lands, freeze requested", pa[0x15c] == 0x2000 && atomic_load(&rA->freeze_req) && atomic_load(&rA->hits_inflight) == 1);
    ww_commit(g_fence + 9);
    CHECK("frozen with versions", atomic_load(&g_sbf_n) == 1 && g_sbf[0].lo == A && g_sbf[0].n == 0x40000 && g_sbf[0].wwi1 == 1 && atomic_load(&rA->lfx) && atomic_load(&rA->dirty));
    CHECK("the GPU-produced range is not frozen", !atomic_load(&rG->lfx) && !sbf_find(G));
    // 4. a read-only binding reads a copy with the current bytes; the range is re-armed
    __unsafe_unretained id<MTLBuffer> b1 = nil; uint64_t g1 = sbf_read(A + 0x100, &b1);
    CHECK("copy bound", g1 && b1 && g1 == b1.gpuAddress + 0x100);
    const uint32_t *c1 = (const uint32_t *)b1.contents;
    CHECK("copy holds the bytes at encode", c1[0x15c] == 0x2000 && c1[0x57] == 0xaaaa && c1[3] == 0x1003);
    CHECK("re-armed, not dirty", atomic_load(&rA->armed) && !atomic_load(&rA->dirty) && g_lfx_versions == 1);
    // 5. no CPU write since: the next binding reuses the copy
    __unsafe_unretained id<MTLBuffer> b2 = nil; uint64_t g2 = sbf_read(A, &b2);
    CHECK("reused", g2 == g1 - 0x100 && b2 == b1 && g_lfx_versions == 1);
    // 6. the CPU writes the next version: one fault marks it, the page opens, the store lands in memory, the copy keeps the old bytes
    uint64_t lf0 = atomic_load(&rA->lfx_faults);
    pa[0x15c] = 0x3000; pa[0x15d] = 0x3001;
    CHECK("store in memory, copy unchanged", pa[0x15c] == 0x3000 && pa[0x15d] == 0x3001 && c1[0x15c] == 0x2000);
    CHECK("one fault, dirty", atomic_load(&rA->lfx_faults) == lf0 + 1 && atomic_load(&rA->dirty));
    // 7. the next reader takes a new version
    g_fence += 1;   // the first copy's submission is still "in flight" (fence > done): a new buffer of the ring
    __unsafe_unretained id<MTLBuffer> b3 = nil; uint64_t g3 = sbf_read(A, &b3);
    CHECK("new version", g3 && b3 && b3 != b1 && ((const uint32_t *)b3.contents)[0x15c] == 0x3000 && g_lfx_versions == 2);
    CHECK("old version untouched (its reader still reads it)", c1[0x15c] == 0x2000);
    // 8. slack after the range is filled from memory
    CHECK("slack from memory", b3.length >= 0x40000 + (1u << 20) && ((const uint8_t *)b3.contents)[0x40000] == *(const uint8_t *)(uintptr_t)(A + 0x40000));
    // 9. a writable binding this frame: memory
    sbf_written(A, 0x40000, "abcd", 5, "storage binding"); __unsafe_unretained id<MTLBuffer> b4 = nil;
    CHECK("writable binding: memory this frame", sbf_read(A, &b4) == 0);
    lfx_summary();
    fprintf(stderr, "%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
