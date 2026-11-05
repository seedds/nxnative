// A texture upload must read live, page-aligned guest storage without keeping
// unrelated pages of its pool resident after the upload has completed.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)

int main(void) { @autoreleasepool {
    alarm(30); if (setvbuf(stdout, NULL, _IONBF, 0)) return 1;
    setenv("DM_NVN_UPLOAD_RANGE", "1", 1); setenv("DM_PSO_PREPARE", "0", 1);
    mtl_init();
    size_t page = (size_t)getpagesize(), size = 8u << 20, offset = page * 7 + 128;
    char path[] = "/tmp/dm-upload-range-XXXXXX";
    int fd = mkstemp(path); if (fd < 0) return 1;
    unlink(path); if (ftruncate(fd, (off_t)size)) { close(fd); return 1; }
    uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); if (mem == MAP_FAILED) return 1;
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=99621, .has_layout=1};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=17, .height=9, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); t.mem = mem + offset;
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=size, .flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    t.pool = &pool; t.off = offset;
    mtl_pool_add(mem, size); nvn_dirty_add_range(mem, size);
    uint8_t expected[17 * 9 * 4]; memset(expected, 27, sizeof expected);
    CHECK("initial swizzle", !nvn_swizzle(&t.d, 0, 0, expected, 17 * 4, t.mem));
    int resident_before = g_pool_res_n;
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    if (!gate) return 1;
    [g_q4 waitForEvent:gate value:1];
    mtl_sub_begin(); uint64_t stamp = 0;
    id<MTLTexture> image = pkg_upload_(&t, &stamp, nil, 0);
    CHECK("production GPU upload encoded", image != nil && g_up_cb != nil);
    CHECK("upload does not pin its whole pool", g_pool_res_n == resident_before);
    id<MTLBuffer> range = nil;
    id<MTLResidencySet> sources = g_up_sources;
    for (id<MTLBuffer> b in sources.allAllocations) if (b.contents == mem + (offset & ~(page - 1))) range = b;
    size_t want = ((offset & (page - 1)) + t.L.size + page - 1) & ~(page - 1);
    CHECK("only source pages are exposed", range && range.length == want && range.length < size);
    uint64_t fence = mtl_sub_commit();
    CHECK("upload remains pending behind producer gate", !mtl_wait(fence, 0));
    CHECK("range remains resident until GPU completion", range && [sources containsAllocation:range]);
    CHECK("other game submissions do not inherit upload sources", range && ![g_res containsAllocation:range]);
    // Change the shared-file bytes after CPU encoding, before GPU execution.
    // A CPU snapshot would return the old texels and fail the readback below.
    dm_guest_write_prepare(t.mem, t.L.size);
    for (unsigned i = 0; i < sizeof expected; i++) expected[i] = (uint8_t)(i * 17 + 91);
    CHECK("late source write", !nvn_swizzle(&t.d, 0, 0, expected, 17 * 4, t.mem));
    gate.signaledValue = 1;
    CHECK("submission completes", mtl_wait(fence, 5ull * NSEC_PER_SEC));
    mtl_finish_gpu();
    CHECK("range leaves residency after upload", range && ![sources containsAllocation:range]);
    id<MTLBuffer> out = [g_dev newBufferWithLength:sizeof expected options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(17, 9, 1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:17 * 4 destinationBytesPerImage:sizeof expected];
    [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK("GPU reads the live source bytes", !cb.error && !memcmp(out.contents, expected, sizeof expected));
    CHECK("completed upload leaves no whole-pool residency", g_pool_res_n == resident_before);
    // One source can survive a batch split. Finishing the first batch must not
    // remove residency owned by the next batch, still being encoded.
    id<MTLBuffer> raw = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    res_add(raw); res_commit();
    id<MTLSharedEvent> first_done = [g_dev newSharedEvent];
    if (!raw || !first_done || !range) return 1;
    [g_q4 waitForEvent:gate value:2];
    up_begin(); up_source_use(range); id<MTLResidencySet> first_sources = g_up_sources;
    [g_up_ce copyFromBuffer:range sourceOffset:0 toBuffer:raw destinationOffset:0 size:32];
    up_flush(); rec_drain(); [g_q4 signalEvent:first_done value:1];
    up_begin(); up_source_use(range); id<MTLResidencySet> next_sources = g_up_sources;
    [g_up_ce copyFromBuffer:range sourceOffset:0 toBuffer:raw destinationOffset:32 size:32];
    gate.signaledValue = 2;
    CHECK("first range batch completes independently", [first_done waitUntilSignaledValue:1 timeoutMS:5000]);
    up_wait_zero(&g_up_pending, WS_DIAG);
    CHECK("completed batch retires only its own residency", ![first_sources containsAllocation:range] && [next_sources containsAllocation:range]);
    [g_q4 waitForEvent:gate value:3];
    up_flush(); rec_drain();
    gate.signaledValue = 3; up_wait_zero(&g_up_pending, WS_DIAG);
    CHECK("both batches read the same live pages", !memcmp(raw.contents, range.contents, 32) && !memcmp((uint8_t *)raw.contents + 32, range.contents, 32));
    CHECK("last source batch retires", ![next_sources containsAllocation:range]);
    res_remove(raw);
    void *whole = NULL; uint64_t off = 0;
    CHECK("a later real buffer binding still resolves", mtl_addr((uint64_t)(uintptr_t)mem, size, &whole, &off));
    CHECK("that real binding establishes full residency", g_pool_res_n == resident_before + 1);
    res_remove(image); nvn_dirty_remove_range(mem); mtl_pool_del(mem); mtl_drain();
    range = nil; munmap(mem, size);
    printf("upload range: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
