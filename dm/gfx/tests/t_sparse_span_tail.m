// A storage buffer bound by its address alone (NVN_RB_SBUFU / NVN_RB_SBUFQ: nvn_mtl.m sbuf_addr) in a sparse pool, read by a compute kernel past its last mapped run, as a shader
// that indexes from the address does. On the Switch the GPU reads the pages mapped there and 0 for unmapped ones. The binding runs DM_NVN_SPARSE_SPAN_TAIL_MB (4) past the last run,
// at most to the end of the reservation, over zero pages (hle_nvn.c nvn_sparse_span_bind). Before, it ended at the last run, and the read ran past the Metal buffer.
//  B. PRAGMATA's layout: an unmapped head, 64 KB runs at +0xb0000 .. +0x180000 with a hole; read 2 MB past the last run;
//  C. the same from an unaligned address;
//  D. a page mapped into the tail later is read through the next binding (the span over it was retired);
//  E. the tail stops at the end of the reservation;
//  F. one run of 0xc0000 bytes at the address, no hole, read 0x150000 bytes (PRAGMATA's GI voxeliser ef4ad011 at the R53 hangar's entry);
//  G. a tail page whose unmap is still queued: the tail stops before it, and reads 0 there once the unmap has released it.
// The kernel runs only when the bound buffer holds the read, so the old binding fails on its length and reads no memory past its end.
// Run: ./t_sparse_span_tail ; DM_NVN_SPARSE_SPAN_TAIL_MB=0 ./t_sparse_span_tail (fails B to G)
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
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static pool_t g_phys, g_sparse;
static void map_at(uint64_t voff, uint64_t size, uint64_t poff, int mapped) {
    map_req r = {.phys = mapped ? (NVNmemoryPool *)&g_phys : NULL, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
static uint64_t blen(void *mb, uint64_t mo) { return mb ? (uint64_t)[(__bridge id<MTLBuffer>)mb length] - mo : 0; }
static id<MTLComputePipelineState> g_ps;
static const char *k_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void rd(device const uint *src [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
    "  if (i < n) out[i] = src[i]; }\n";   // reads by index from the bound address, as the translated shader does
// What a compute kernel reads at [mo, mo + n) of the binding; nil (no dispatch) when the buffer does not hold it
static NSData *k_read(void *mb, uint64_t mo, uint64_t n) {
    if (blen(mb, mo) < n) return nil;
    id<MTLBuffer> src = (__bridge id<MTLBuffer>)mb, dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared]; uint32_t cnt = (uint32_t)(n / 4);
    if (!dst) return nil;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:g_ps]; [ce setBuffer:src offset:mo atIndex:0]; [ce setBuffer:dst offset:0 atIndex:1]; [ce setBytes:&cnt length:4 atIndex:2];
    [ce dispatchThreads:MTLSizeMake(cnt, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? [NSData dataWithBytes:dst.contents length:n] : nil;
}
// The guest's view of the sparse pool's [voff, voff + n): the mapped pages' bytes, 0 elsewhere
static int guest_match(const uint8_t *g, uint64_t voff, uint64_t n) {
    const struct vpool *v = vpool_of(&g_sparse);
    pthread_mutex_lock(&g_vp_mu); int ok = 1;
    for (uint64_t o = 0; o < n && ok; ) { uint64_t at = voff + o, k = 0x1000 - (at & 0xFFF); if (k > n - o) k = n - o;
        const vpte *e = vpt_get(v, at & ~0xFFFull);
        if (e && e->phys) ok = !memcmp(g + o, e->phys + (at & 0xFFF), k); else for (uint64_t j = 0; j < k && ok; j++) ok = !g[o + j];
        o += k; }
    pthread_mutex_unlock(&g_vp_mu); return ok;
}
static uint8_t *g_base;
static void read_case(const char *name, uint64_t voff, uint64_t len) {   // an SBUFU binding as encode_args_ resolves it, then a kernel reading len bytes from it
    uint64_t a = (uint64_t)(uintptr_t)g_base + voff; void *mb = NULL; uint64_t mo = 0, gpu = 0; char lab[200];
    int ok = sbuf_addr(a, &mb, &mo, &gpu);
    printf("  %-44s binding holds %#llx, read %#llx\n", name, (unsigned long long)blen(mb, mo), (unsigned long long)len);
    snprintf(lab, sizeof lab, "%s: bound", name); CHECK(lab, ok);
    snprintf(lab, sizeof lab, "%s: the binding holds the read", name); CHECK(lab, blen(mb, mo) >= len);
    snprintf(lab, sizeof lab, "%s: its GPU address is the buffer's", name); CHECK(lab, !ok || gpu == [(__bridge id<MTLBuffer>)mb gpuAddress] + mo);
    NSData *d = ok ? k_read(mb, mo, len) : nil;
    snprintf(lab, sizeof lab, "%s: the kernel reads the mapped pages, and 0 elsewhere", name); CHECK(lab, d && guest_match(d.bytes, voff, len));
}
int main(void) { @autoreleasepool {
    mtl_init();
    { NSError *er = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_src) options:nil error:&er]; if (!lib) { printf("FAIL: kernel: %s\n", er.description.UTF8String); return 1; }
      id<MTLFunction> fn = [lib newFunctionWithName:@"rd"]; if (!fn) { printf("FAIL: no kernel function\n"); return 1; }
      g_ps = [g_dev newComputePipelineStateWithFunction:fn error:&er]; if (!g_ps) { printf("FAIL: pipeline\n"); return 1; } }
    size_t plen = 32u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u | 1u;   // never 0
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    enum { P = 0x10000 };
    map_at(0, P, 0, 1);   // the first mapping, not the whole pool: the pool is sparse
    const struct vpool *v = vpool_of(&g_sparse); if (!v) { printf("FAIL: no sparse pool\n"); return 1; }
    g_base = v->base;
    // B. [vo, vo + 0xb0000) unmapped, 64 KB runs at +0xb0000 .. +0x180000 (a hole at +0x110000), nothing after
    uint64_t vo = 256ull << 20;
    for (uint64_t o = 0xb0000; o < 0x180000; o += P) if (o != 0x110000) map_at(vo + o, P, (1u << 20) + o * 3, 1);
    read_case("B 2 MB past the last run", vo + 0xb0000, 0xd0000 + (2u << 20));
    // C. from an unaligned address
    read_case("C unaligned address", vo + 0xb0010, 0x180000 - 0xb0010 + (1u << 20));
    // D. a page mapped later inside the tail: the span over it is retired, the next binding reads the page
    map_at(vo + 0x280000, P, 9u << 20, 1);
    read_case("D a page mapped into the tail later", vo + 0xb0000, 0x1e0000 + (1u << 20));
    // F. one run of 0xc0000 bytes at the address and no hole, read 0x150000 bytes
    { uint64_t vf = 640ull << 20; map_at(vf, 0xc0000, 4u << 20, 1); read_case("F one run, no hole (ef4ad011)", vf, 0x150000); }
    // E. a run 0x40000 before the reservation's end: the tail stops there
    { uint64_t ve = (1ull << 30) - 0x40000, a = (uint64_t)(uintptr_t)g_base + ve; void *mb = NULL; uint64_t mo = 0, gpu = 0;
      map_at(ve, P, 12u << 20, 1);
      CHECK("E bound", sbuf_addr(a, &mb, &mo, &gpu));
      CHECK("E the binding ends at the reservation's end", blen(mb, mo) == 0x40000);
      NSData *d = k_read(mb, mo, 0x40000); CHECK("E the kernel reads the run, then 0", d && guest_match(d.bytes, ve, 0x40000));
      printf("  %-44s binding holds %#llx (the reservation ends %#x past)\n", "E at the end of the reservation", (unsigned long long)blen(mb, mo), 0x40000); }
    // G. a page 0x40000 past a lone run, mapped, then unmapped: while its unmap is queued (the old physical page still there) the tail stops before it; once the unmap has
    //    released it, the tail runs over it and reads 0 there
    { uint64_t vg = 896ull << 20, a = (uint64_t)(uintptr_t)g_base + vg; void *mb = NULL; uint64_t mo = 0, gpu = 0;
      map_at(vg, P, 14u << 20, 1); map_at(vg + 0x40000, P, 15u << 20, 1); map_at(vg + 0x40000, P, 0, 0);
      CHECK("G bound with the unmap queued", sbuf_addr(a, &mb, &mo, &gpu));
      CHECK("G the tail stops before the queued page", blen(mb, mo) == 0x40000);
      mtl_sub_begin(); CHECK("G a submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));   // the unmap's fence
      CHECK("G bound once the unmap has released it", sbuf_addr(a, &mb, &mo, &gpu) && blen(mb, mo) >= 0x50000);
      NSData *d = k_read(mb, mo, 0x50000); CHECK("G the kernel reads the run, then 0, the released page included", d && !memcmp(d.bytes, phys + (14u << 20), P) && guest_match(d.bytes, vg, 0x50000)); }
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&g_sparse);
    int left = 0; for (int i = 0; i < SPAN_C; i++) left += g_span_c[i].buf != nil;
    CHECK("no span buffer left after Finalize", !left);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&g_phys);
    printf("%s: %d checks, %d failed (span buffers made %llu)\n", failed ? "FAIL" : "ok", checks, failed, (unsigned long long)g_span_made);
    return failed != 0;
} }
