// Whole raw mappings supersede every chunk without invalidating other pools.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, s) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", s); } } while (0)
static id<MTLBuffer> lookup(uint8_t *mem, uint64_t n) {
    void *b = NULL; uint64_t off = 0, gpu = 0;
    CHECK(mtl_gpu_addr((uint64_t)(uintptr_t)mem, n, &b, &off, &gpu), "GPU lookup succeeds");
    id<MTLBuffer> out = (__bridge id<MTLBuffer>)b;
    CHECK((uint8_t *)out.contents + off == mem && off + n <= out.length, "lookup preserves guest storage");
    CHECK(gpu == out.gpuAddress + off, "GPU address matches the selected buffer");
    return out;
}
int main(void) { @autoreleasepool {
    mtl_init(); const size_t MB = 1u << 20, na = 1024 * MB, nb = 512 * MB, ns = 65536;
    uint8_t *a = mmap(NULL, na, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *b = mmap(NULL, nb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *small = mmap(NULL, ns, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (a == MAP_FAILED || b == MAP_FAILED || small == MAP_FAILED) return 1;
    int initial = g_npool;
    mtl_pool_add(a, na); mtl_pool_add(b, nb);
    id<MTLBuffer> first = lookup(a + 128, 4096);
    id<MTLBuffer> extended = lookup(a + 256, 160 * MB);
    id<MTLBuffer> far = lookup(a + 300 * MB, 64);
    id<MTLBuffer> other = lookup(b + 128, 64);
    mtl_pool_add(small, ns); id<MTLBuffer> ordinary = lookup(small, 64);
    CHECK(first != extended, "the initial chunk was lengthened");
    CHECK([g_res_objects containsObject:first], "prior chunk stays resident until consolidation retires it");
    *(uint32_t *)(a + 128) = 0x1234abcd;
    mtl_sub_begin(); CHECK(gmem_map_gpu() != 0, "raw-address map created");
    CHECK(g_npool == initial + 3 && g_nbig == 2, "only whole mappings and ordinary pool remain");
    id<MTLBuffer> whole = lookup(a + 128, 4096);
    CHECK(whole.length == na && lookup(a + 300 * MB, 64) == whole, "all first-pool addresses share the whole mapping");
    CHECK(lookup(b + 128, 64).length == nb, "moved second-pool indices are repaired");
    CHECK(lookup(small, 64) == ordinary, "unrelated pool survives swapped entries");
    CHECK([g_res_objects containsObject:first] && [g_res_objects containsObject:extended] &&
          [g_res_objects containsObject:far] && [g_res_objects containsObject:other], "retired buffers remain resident before their fence");
    id<MTLBuffer> readback = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(readback);
    if (!whole || !readback) return 1;
    enc_b_copy_buffer(blit_open(), whole, 128, readback, 0, 4);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "GPU submission completes");
    mtl_drain();
    CHECK(*(uint32_t *)readback.contents == 0x1234abcd, "whole mapping preserves GPU-visible data");
    CHECK(![g_res_objects containsObject:first] && ![g_res_objects containsObject:extended] &&
          ![g_res_objects containsObject:far] && ![g_res_objects containsObject:other], "all obsolete chunk buffers leave residency after completion");
    CHECK([g_res_objects containsObject:whole] && [g_res_objects containsObject:ordinary], "current mappings remain resident");
    mtl_pool_del(a); mtl_pool_del(b); mtl_pool_del(small); mtl_finish();
    CHECK(g_npool == initial && g_nbig == 0, "parent teardown removes current entries");
    munmap(a, na); munmap(b, nb); munmap(small, ns);
    printf("whole-pool retirement: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
