// Hashing guest shader code is CPU work: it must not pin the entire pool for
// GPU execution. Later real GPU use must still establish residency normally.
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
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)

int main(void) { @autoreleasepool {
    alarm(30); setenv("DM_PSO_PREPARE", "0", 1);
    mtl_init();
    size_t n = (size_t)getpagesize() * 2;
    uint8_t *mem = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return 1;
    for (size_t i = 0; i < n; i++) mem[i] = (uint8_t)(i * 13 + 7);
    uint32_t control[0x48 / 4] = {0}; control[0] = 0x98761234; control[1] = 0xc9; control[0x44 / 4] = 128;
    NVNshaderData sd = {.data = (uint64_t)(uintptr_t)mem, .control = control};
    uint8_t expected[8] = {0}, actual[8] = {0};
    CHECK("unregistered code is deferred", !keys_hash(&sd, 1, actual));
    CHECK("reference CPU key available", keys_hash(&sd, 0, expected));
    mtl_pool_add(mem, n);
    int before = g_pool_res_n;
    CHECK("registered shader key succeeds", keys_hash(&sd, 1, actual));
    CHECK("shader key is unchanged", !memcmp(expected, actual, sizeof actual));
    CHECK("CPU hashing does not add GPU residency", g_pool_res_n == before);
    sd.data = (uint64_t)(uintptr_t)(mem + n - 64);
    CHECK("code crossing pool end is deferred", !keys_hash(&sd, 1, actual));

    void *buf = NULL; uint64_t off = 0, gpu = 0;
    CHECK("later GPU access resolves", mtl_gpu_addr((uint64_t)(uintptr_t)mem, n, &buf, &off, &gpu));
    CHECK("GPU access adds residency", g_pool_res_n == before + 1);
    id<MTLBuffer> dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!buf || !dst) return 1;
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLBlitCommandEncoder> enc = [cb blitCommandEncoder];
    [enc copyFromBuffer:(__bridge id<MTLBuffer>)buf sourceOffset:off toBuffer:dst destinationOffset:0 size:n];
    [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK("GPU reads the original code bytes", !cb.error && !memcmp(dst.contents, mem, n));
    sd.data = (uint64_t)(uintptr_t)mem;
    mtl_pool_del(mem);
    CHECK("finalized pool is no longer readable by key lookup", !keys_hash(&sd, 1, actual));
    mtl_sub_begin(); uint64_t fence = mtl_sub_commit(); mtl_wait(fence, UINT64_MAX); defer_drain();
    munmap(mem, n);
    printf("shader residency: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
