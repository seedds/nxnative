// Sparse mappings as notes (treg_sparse_notes): a mapping request that changes a sparse texture's pages, with no registration, serial change or CPU write, reaches the texture
// ID views, the pool tables and the handle tables at the next submission, and the GPU reads the newly mapped page; a request outside the texture notes nothing; serial changes
// while a sparse pool exists keep the pool tables. Physical page contents are literal colours. After the fixture of PR #1721 (NicholasDominici)
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
_Static_assert(DMSH_POOL_HEAD == 16, "the reader below takes the table head as 16 bytes");
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void next_sub(void) { mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); }
// The pixel at (0, 0) of element `slot` of a resource-ID table, read on the GPU; then the next submission
static uint32_t gpu_read(id<MTLBuffer> table, unsigned slot) {
    static id<MTLComputePipelineState> ps;
    if (!ps) {
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct Pool { uint n, pad; ulong seen; texture2d<float> t[2]; };\n"
                              "kernel void read_pool(constant Pool &p [[buffer(0)]], device uint *out [[buffer(1)]]) { uint4 c = uint4(round(p.t[out[1]].read(uint2(0)) * 255.0f)); out[0] = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }\n"
                                                     options:nil error:&error];
        id<MTLFunction> fn = [lib newFunctionWithName:@"read_pool"];
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
        CHECK(ps != nil, "the table reader compiles"); if (!ps) return 0;
    }
    uint32_t initial[2] = {0, slot};
    id<MTLBuffer> out = [g_dev newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared]; res_add(out);
    MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2;
    id<MTL4ArgumentTable> args = rec_table([g_dev newArgumentTableWithDescriptor:d error:nil]);
    [args setAddress:table.gpuAddress atIndex:0]; [args setAddress:out.gpuAddress atIndex:1];
    id<MTL4ComputeCommandEncoder> enc = enc_c_begin(nil); [enc setArgumentTable:args]; enc_c_pso(enc, ps);
    enc_c_dispatch(enc, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(enc);
    next_sub();
    CHECK(!mtl_last_commit_error(), "the reader's submission completes");
    return *(uint32_t *)out.contents;
}
static int map(pool_t *virt, pool_t *phys, uint64_t voff, uint64_t poff, uint64_t size) {
    map_req req = {.phys = (NVNmemoryPool *)phys, .voff = (int64_t)voff, .poff = (int64_t)poff, .size = size};
    return h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&req);
}
int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    const uint32_t A = 0xff112233, B = 0xff445566, C = 0xff778899;
    uint8_t *mem = NULL; posix_memalign((void **)&mem, 16384, 1 << 20); memset(mem, 0, 1 << 20);
    for (unsigned i = 0; i < 0x1000 / 4; i++) { ((uint32_t *)mem)[i] = A; ((uint32_t *)(mem + 0x10000))[i] = B; ((uint32_t *)(mem + 0x20000))[i] = C; }
    pool_t phys = {0}, virt = {0}; pool_builder_t pb = {.mem = mem, .size = 1 << 20, .flags = 0x24};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys, (NVNmemoryPoolBuilder *)&pb), "physical pool");
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};   // larger than the application pool: sparse from the start
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virt, (NVNmemoryPoolBuilder *)&pb), "sparse virtual pool");
    CHECK(map(&virt, &phys, 0, 0, 0x4000), "first mapping");
    CHECK(nvn_sparse_pool(&virt), "the pool reads through its page table");
    // A 32x32 RGBA8 texture: one 4 KB page at the pool's start
    NVNtextureBuilder tb; NVNtexture tx; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 32); h_nvnTextureBuilderSetHeight(&tb, 32);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, 0);
    CHECK(h_nvnTextureInitialize(&tx, &tb), "sparse texture");
    mtl_reg_tex(1, (const tex_t *)&tx, NULL);
    g_seen_done[0] |= 2; g_seen_typed[0][0] |= 2;   // a shader has read ID 1 through a 2D table: the walk makes its texture (no stand-in)
    __unsafe_unretained id<MTLBuffer> table = nil;
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 1) == A, "the pool table reads the first mapping's page");

    // Serial changes while a sparse pool exists keep the table: they note no sparse texture
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table); id<MTLBuffer> before = table; uint64_t builds = g_stat_pool_builds;
    for (int i = 0; i < 128; i++) { serial_put(98000 + i, [NvProg new]); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table); }
    CHECK(table == before && g_stat_pool_builds == builds, "128 program changes keep the pool table and build none");

    // A request past the texture's storage (a sparse texture's size is whole 64 KB tiles): nothing noted at the next submission
    uint64_t tsize = ((const tex_t *)&tx)->L.size, past = (tsize + 0xFFFF) & ~0xFFFFull;
    CHECK(tsize >= 0x1000 && tsize <= past, "the texture's storage size");
    uint64_t sg = g_ser_gen;
    CHECK(map(&virt, &phys, past, 0x30000, 0x1000), "a mapping beside the texture");
    next_sub();
    CHECK(g_ser_gen == sg, "a mapping outside every texture notes nothing");

    // A 4 KB request over the texture's page: no registration, serial change or CPU write at the request, a note at the next submission
    uint64_t rg = atomic_load(&g_reg_gen), dg = nvn_dirty_now(); sg = g_ser_gen;
    args_sync_dirty(); memset(g_args_dirty, 0, sizeof g_args_dirty);
    CHECK(map(&virt, &phys, 0, 0x10000, 0x1000), "the texture's page mapped from elsewhere");
    CHECK(atomic_load(&g_reg_gen) == rg && g_ser_gen == sg && nvn_dirty_now() == dg, "the request itself changes no registration, serial or CPU-write generation");
    next_sub();
    CHECK(g_ser_gen == sg + 1, "the next submission notes the texture's serial once");
    args_sync_dirty(); int all = 1; for (unsigned i = 0; i < sizeof g_args_dirty; i++) all &= g_args_dirty[i] != 0;
    CHECK(all, "the note sends cached argument blocks through their check");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(g_stat_pool_builds == builds, "the pool table is patched, not built whole");
    CHECK(gpu_read(table, 1) == B, "the pool table reads the newly mapped page");

    // A table of handles in a uniform buffer, unchanged while the page behind its texture is mapped again
    uint32_t *handles = (uint32_t *)(mem + 0x40000); handles[0] = 1; handles[1] = 0;
    S.ubo[0][0] = (p_bind_buf){.addr = (uintptr_t)handles, .size = 8};
    nvn_rbind binding = {.kind = NVN_RB_TEXH, .reg = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    htab_gpu(0, &binding, &table);
    CHECK(gpu_read(table, 0) == B, "the handle table reads the current page");
    CHECK(map(&virt, &phys, 0, 0x20000, 0x1000), "the texture's page mapped again");
    next_sub();
    htab_gpu(0, &binding, &table);
    CHECK(gpu_read(table, 0) == C, "the handle table reads the newly mapped page");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 1) == C, "so does the pool table");

    h_nvnTextureFinalize(&tx);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    mtl_sub_commit(); mtl_drain(); free(mem);
    printf("sparse pool cache: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
