// Storage bounds (GitHub issue #2618): a storage buffer the shader takes by address from a game uniform slot or the push constants (SBUFU / SBUFQ) has no size, and on the console the
// shader reads the GPU's page table as far as it indexes, an unmapped page reading 0. The host binds the Metal buffer that holds the address (mtl_gpu_addr) and gives the shader the
// memory bound there in its length table (nvn_mtl.m sblen_put, dmsh_abi.h DMSH_SBLEN_BANK): the words readable before the address and in all. The kernels below do what a translated
// shader does with them (Instructions.cs TryGetBoundedStoragePointer: in = idx + before < total, unsigned; a load reads word 0 and gives 0 outside, as a select (dmsh_fetch.select_guards), a store or an
// atomic is skipped).
// Cases: an address inside an ordinary pool (words before it readable too, nothing past the pool), PRAGMATA's hangar binding (a 1 MB sparse pool with one 0x30000-byte run mapped at its
// base, read 0x50000 bytes: the run, then zeros where its GI voxeliser ef4ad01151918469 read other memory), and a dummy binding (nothing readable). Runs under Metal validation
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

enum { SLOT = 3, POOL = 64u << 10, AT = 0x1000, VPOOL = 1u << 20, RUN = 0x30000, READ = 0x50000 };
static const char *kSource =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct L { uint4 m[16]; };\n"
    // the translator's comparison for storage slot 3: its words at u32 6 and 7 of the table
    "static bool inside(constant L &t, int idx) { return (uint(idx) + t.m[1].z) < t.m[1].w; }\n"
    "kernel void rd(device uint *buf [[buffer(0)]], constant L &t [[buffer(1)]], constant int &first [[buffer(2)]], device uint *out [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
    "    int idx = first + int(i); bool in = inside(t, idx);\n"
    "    out[i] = select(0u, buf[in ? idx : 0], in);\n"   // as dmsh_fetch.select_guards writes the translator's read
    "}\n"
    "kernel void wr(device uint *buf [[buffer(0)]], constant L &t [[buffer(1)]], constant int *idxs [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
    "    int idx = idxs[i]; bool in = inside(t, idx);\n"
    "    if (in) buf[in ? idx : 0] = 0xC0DE0000u + i;\n"
    "}\n"
    "kernel void at(device uint *buf [[buffer(0)]], constant L &t [[buffer(1)]], constant int *idxs [[buffer(2)]], device uint *out [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
    "    int idx = idxs[i]; bool in = inside(t, idx); uint r;\n"
    "    if (in) r = atomic_fetch_add_explicit((device atomic_uint *)&buf[in ? idx : 0], 1u, memory_order_relaxed); else r = 0u;\n"
    "    out[i] = r;\n"
    "}\n";

static id<MTLCommandQueue> g_q; static id<MTLLibrary> g_lib;
static id<MTLComputePipelineState> kernel_named(NSString *name) {
    NSError *error = nil; id<MTLFunction> fn = [g_lib newFunctionWithName:name];
    id<MTLComputePipelineState> pso = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
    if (!pso) fprintf(stderr, "%s: %s\n", name.UTF8String, error.localizedDescription.UTF8String);
    return pso;
}
// The binding of an address as the replay makes it (encode_args, NVN_RB_SBUFQ): the buffer that holds it and the offset there, or none, with its length table
typedef struct { id<MTLBuffer> buf; uint64_t off; id<MTLBuffer> table; uint32_t before, total; } binding;
static binding bind_addr(uint64_t addr) {
    binding b = {0}; void *mb; uint64_t mo, gpu;
    uint32_t t[DMSH_SBLEN_SLOTS * 2]; memset(t, 0xEE, sizeof t);
    nvn_rbind rb = {.kind = NVN_RB_SBUFQ, .sbx = SLOT + 1};
    if (addr && mtl_gpu_addr(addr, 1, &mb, &mo, &gpu)) { b.buf = (__bridge id<MTLBuffer>)mb; b.off = mo; sblen_put(t, &rb, b.buf, mo); }
    else { b.buf = dummy_buf(); b.off = 0; sblen_put(t, &rb, nil, 0); }
    b.before = t[2 * SLOT]; b.total = t[2 * SLOT + 1];
    b.table = [g_dev newBufferWithBytes:t length:sizeof t options:MTLResourceStorageModeShared];
    return b;
}
static id<MTLBuffer> run_kernel(NSString *name, const binding *b, const void *arg, size_t arg_len, uint32_t n, size_t out_len) {
    id<MTLComputePipelineState> k = kernel_named(name); if (!k) return nil;
    id<MTLBuffer> out = out_len ? [g_dev newBufferWithLength:out_len options:MTLResourceStorageModeShared] : nil;
    id<MTLCommandBuffer> cmd = [g_q commandBuffer]; id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:k]; [enc setBuffer:b->buf offset:b->off atIndex:0]; [enc setBuffer:b->table offset:0 atIndex:1]; [enc setBytes:arg length:arg_len atIndex:2];
    if (out) [enc setBuffer:out offset:0 atIndex:3];
    [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
    CHECK("kernel completed", cmd.status == MTLCommandBufferStatusCompleted);
    return out;
}

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil; g_lib = [g_dev newLibraryWithSource:@(kSource) options:nil error:&error];
    if (!g_lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    g_q = [g_dev newCommandQueue];

    // 1. An address 0x1000 into an ordinary 64 KB pool: 1,024 words before it and 15,360 from it are the pool's; one word further either way reads 0
    uint32_t *mem; if (posix_memalign((void **)&mem, (size_t)getpagesize(), POOL)) abort();
    for (uint32_t i = 0; i < POOL / 4; i++) mem[i] = 0x9e3779b9u * (i + 1);
    mtl_pool_add(mem, POOL);
    uint64_t a = (uint64_t)(uintptr_t)mem + AT;
    binding b = bind_addr(a);
    CHECK("the pool's buffer, at the address's offset", b.buf && b.off == AT && b.buf.length >= POOL);
    CHECK("words before the address: its offset in the buffer", b.before == AT / 4);
    CHECK("words in all: the buffer's", b.total == b.before + (uint32_t)((b.buf.length - AT) / 4));
    {   int first = -(int)(AT / 4) - 4; uint32_t n = POOL / 4 + 8;
        id<MTLBuffer> out = run_kernel(@"rd", &b, &first, sizeof first, n, n * 4);
        const uint32_t *o = out.contents; int in_ok = 1, out_ok = 1;
        for (uint32_t i = 0; i < n; i++) { int64_t w = (int64_t)first + i + AT / 4;   // the pool word this index reads
            if (w >= 0 && w < POOL / 4) in_ok &= o[i] == mem[w]; else out_ok &= o[i] == 0; }
        CHECK("every word of the pool, before the address and after it, reads as it is", in_ok);
        CHECK("a word before the pool or past its end reads 0", out_ok); }
    {   const int32_t idxs[] = {0, 5, -1, -(int)(AT / 4), -(int)(AT / 4) - 1, (int)((POOL - AT) / 4) - 1, (int)((POOL - AT) / 4), 0x7fffffff, (int)0x80000000};
        uint32_t keep[POOL / 4]; memcpy(keep, mem, POOL);
        run_kernel(@"wr", &b, idxs, sizeof idxs, sizeof idxs / 4, 0);
        int ok = 1;
        for (uint32_t i = 0; i < sizeof idxs / 4; i++) { int64_t w = (int64_t)idxs[i] + AT / 4;
            if (w >= 0 && w < POOL / 4) { ok &= mem[w] == 0xC0DE0000u + i; keep[w] = mem[w]; } }
        CHECK("a store inside lands on its word", ok);
        CHECK("nothing else in the pool is written: the stores outside it are skipped", !memcmp(keep, mem, POOL));
        memcpy(keep, mem, POOL);
        id<MTLBuffer> out = run_kernel(@"at", &b, idxs, sizeof idxs, sizeof idxs / 4, sizeof idxs);
        const uint32_t *o = out.contents; ok = 1; int zero = 1;
        for (uint32_t i = 0; i < sizeof idxs / 4; i++) { int64_t w = (int64_t)idxs[i] + AT / 4;
            if (w >= 0 && w < POOL / 4) { ok &= o[i] == keep[w] && mem[w] == keep[w] + 1; keep[w]++; } else zero &= o[i] == 0; }
        CHECK("an atomic inside gives the old word and adds to it", ok);
        CHECK("an atomic outside gives 0", zero);
        CHECK("and changes nothing", !memcmp(keep, mem, POOL)); }
    mtl_pool_del(mem); free(mem);

    // 2. PRAGMATA's R53 hangar: a storage buffer at the base of a sparse pool with one run mapped there, read further than the run
    uint8_t *phys = mmap(NULL, VPOOL, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK("physical storage", phys != MAP_FAILED);
    for (uint32_t i = 0; i < VPOOL / 4; i++) ((uint32_t *)(void *)phys)[i] = 0x85ebca6bu * (i + 7);
    pool_t pp; pool_builder_t pb = {.mem = phys, .size = VPOOL, .flags = 0x18c};   // PHYSICAL | COMPRESSIBLE | GPU_NO_ACCESS | CPU_CACHED
    CHECK("physical pool initialize", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pp, (const NVNmemoryPoolBuilder *)&pb));
    pool_t vp; pool_builder_t vb = {.size = VPOOL, .flags = 0x2a1};   // VIRTUAL | COMPRESSIBLE | GPU_CACHED | CPU_NO_ACCESS
    CHECK("virtual pool initialize", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&vp, (const NVNmemoryPoolBuilder *)&vb));
    NVNbufferAddress base = h_nvnMemoryPoolGetBufferAddress((const NVNmemoryPool *)&vp);   // an address taken first: the pool stays sparse
    map_req r = {.phys = (NVNmemoryPool *)&pp, .poff = 0, .voff = 0, .size = RUN};
    CHECK("map the run", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&vp, 1, (const NVNmappingRequest *)&r));
    CHECK("sparse, one run", vp.vmode == 2 && vp.vp && vp.vp->nrun == 1);
    binding s = bind_addr(base);
    CHECK("the run's buffer, from its start", s.buf && s.off == 0 && s.buf.length == RUN);
    CHECK("nothing before; the run's words in all", s.before == 0 && s.total == RUN / 4);
    {   int first = -4; uint32_t n = READ / 4 + 4;
        id<MTLBuffer> out = run_kernel(@"rd", &s, &first, sizeof first, n, n * 4);
        const uint32_t *o = out.contents; int run_ok = 1, zeros = 1;
        for (uint32_t i = 0; i < n; i++) { int64_t w = (int64_t)first + i;
            if (w >= 0 && w < RUN / 4) run_ok &= o[i] == ((const uint32_t *)(const void *)phys)[w]; else zeros &= o[i] == 0; }
        CHECK("the mapped run reads the physical pages", run_ok);
        CHECK("past the run, where nothing is mapped, every word reads 0, as on the console", zeros); }
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vp); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pp);

    // 3. An address in no pool: the dummy, nothing readable
    binding d = bind_addr(0x7000000000ull);
    CHECK("an address in no pool: nothing readable", d.before == 0 && d.total == 0);
    {   int first = 0; id<MTLBuffer> out = run_kernel(@"rd", &d, &first, sizeof first, 64, 64 * 4);
        const uint32_t *o = out.contents; int zeros = 1; for (int i = 0; i < 64; i++) zeros &= o[i] == 0;
        CHECK("every word reads 0", zeros); }

    printf("t_storage_bounds: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
