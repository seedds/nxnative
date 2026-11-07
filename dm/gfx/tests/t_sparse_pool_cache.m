// A real sparse mapping invalidates views; its mere existence does not.
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
_Static_assert(DMSH_POOL_HEAD == 16, "the Metal fixture includes the read-bitmap address");
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
// Read the actual resource-ID table on the GPU, including UBO-handle tables.
static void sample_table(id<MTLBuffer> table, unsigned slot, uint32_t expected) {
    static id<MTLComputePipelineState> ps;
    if (!ps) {
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct Pool { uint n, pad; ulong seen; texture2d<float> t[2]; };\nkernel void read_pool(constant Pool &p [[buffer(0)]], device uint *out [[buffer(1)]]) { uint4 c=uint4(round(p.t[out[1]].read(uint2(0))*255.0f)); out[0]=c.x|(c.y<<8)|(c.z<<16)|(c.w<<24); }\n" options:nil error:&error];
        id<MTLFunction> fn = [lib newFunctionWithName:@"read_pool"];
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
        CHECK(ps != nil, "native resource-ID reader compiles"); if (!ps) return;
    }
    uint32_t initial[2] = {0, slot};
    id<MTLBuffer> out = [g_dev newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared]; res_add(out);
    MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2;
    id<MTL4ArgumentTable> args = rec_table([g_dev newArgumentTableWithDescriptor:d error:nil]);
    [args setAddress:table.gpuAddress atIndex:0]; [args setAddress:out.gpuAddress atIndex:1];
    id<MTL4ComputeCommandEncoder> enc = enc_c_begin(nil); [enc setArgumentTable:args]; enc_c_pso(enc, ps);
    enc_c_dispatch(enc, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(enc);
    mtl_sub_commit(); mtl_drain();
    if (*(uint32_t *)out.contents != expected) fprintf(stderr, "pixel: got %#x expected %#x\n", *(uint32_t *)out.contents, expected);
    CHECK(!mtl_last_commit_error() && *(uint32_t *)out.contents == expected, "GPU table reads the current physical page's literal pixel");
    mtl_sub_begin();
}
static void remap_pixel(pool_t *phys, pool_t *virt, uint8_t *mem) {
    const uint32_t colors[] = {0xff112233, 0xff445566, 0xff778899};
    for (unsigned c = 0; c < 3; c++) for (unsigned i = 0; i < 0x4000 / 4; i++) ((uint32_t *)(mem + c * 0x10000))[i] = colors[c];
    map_req req = {.phys = (NVNmemoryPool *)phys, .size = 0x4000};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&req), "initial pixel mapping");
    NVNtextureBuilder tb; NVNtexture tx; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 32); h_nvnTextureBuilderSetHeight(&tb, 32);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)virt, 0);
    CHECK(h_nvnTextureInitialize(&tx, &tb), "small sparse texture initialized");
    mtl_reg_tex(1, (const tex_t *)&tx, NULL);
    void *view = NULL; CHECK(tex_fast_rid(1, &view) && view, "texture ID resolves before remapping");
    __unsafe_unretained id<MTLBuffer> table = nil;
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table); sample_table(table, 1, colors[0]);
    // A 4 KiB mapping changes no native CPU alias on a 16 KiB-page Mac.
    req.size = 0x1000; req.poff = 0x10000;
    uint64_t reg = g_reg_gen, serial = g_ser_gen, dirty = nvn_dirty_now();
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&req), "replace the first physical page");
    CHECK(g_reg_gen == reg && g_ser_gen == serial && nvn_dirty_now() == dirty, "pixel remap has no registration, serial or CPU-write note");
    g_walk_sub = g_sub_serial; g_walk_ms = POOL_SUB_MS; // a remap must not keep the old table when the ordinary walk budget is spent
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table); sample_table(table, 1, colors[1]);
    uint32_t *handles = (uint32_t *)(mem + 0x30000); handles[0] = 1; handles[1] = 0;
    S.ubo[0][0] = (p_bind_buf){.addr = (uintptr_t)handles, .size = 8};
    nvn_rbind binding = {.kind = NVN_RB_TEXH, .reg = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    htab_gpu(0, &binding, &table); sample_table(table, 0, colors[1]);
    req.poff = 0x20000;
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&req), "replace the page behind unchanged UBO handles");
    htab_gpu(0, &binding, &table); sample_table(table, 0, colors[2]);
    h_nvnTextureFinalize(&tx);
}
int main(void) { @autoreleasepool {
    mtl_init();mtl_sub_begin();void *mem=NULL;posix_memalign(&mem,16384,1<<20);memset(mem,0,1<<20);
    pool_t phys={0},virt={0};pool_builder_t pb={.mem=mem,.size=1<<20,.flags=0x24};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys,(NVNmemoryPoolBuilder *)&pb),"physical pool initialized");
    pb=(pool_builder_t){.size=DM_GAME_APP_POOL+(1ull<<32),.flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virt,(NVNmemoryPoolBuilder *)&pb),"sparse virtual pool initialized");
    map_req req={.phys=(NVNmemoryPool *)&phys,.size=0x10000};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt,1,(NVNmappingRequest *)&req),"first real sparse mapping accepted");
    __unsafe_unretained id<MTLBuffer> raw=nil;pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&raw);id<MTLBuffer> before=raw;uint64_t builds=g_stat_pool_builds;
    for(int i=0;i<128;i++){serial_put(98000+i,[NvProg new]);pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&raw);}
    CHECK(raw==before && g_stat_pool_builds==builds,"unchanged sparse mappings preserve the pool through program churn");
    before = raw; // compare the mapping change with the last table, independently of the churn check
    args_sync_dirty();memset(g_args_dirty,0,sizeof g_args_dirty);
    uint64_t serial=g_ser_gen,reg=g_reg_gen,dirty=nvn_dirty_now(),sparse=nvn_sparse_gen(),epoch=treg_ep();
    req.voff=0x1000;req.size=0x1000;   // NVN's 4 KiB page: cannot be CPU-aliased on this Apple silicon host's 16 KiB pages
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&virt,1,(NVNmappingRequest *)&req),"second real sparse mapping accepted");
    CHECK(g_ser_gen==serial && nvn_sparse_gen()!=sparse,"page-table change occurs without a serial change");
    CHECK(g_reg_gen==reg && nvn_dirty_now()==dirty,"page-table-only change does not rely on unrelated registration or CPU-write invalidation");
    args_sync_dirty();int all=1;for(unsigned i=0;i<sizeof g_args_dirty;i++)all &= g_args_dirty[i]!=0;
    CHECK(all,"page-table-only change invalidates cached shader argument blocks");
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&raw);
    CHECK(raw != before, "a page-table-only change refreshes the pool snapshot");
    CHECK(treg_ep() != epoch, "a page-table-only change invalidates texture ID views");
    remap_pixel(&phys, &virt, mem);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt);h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);mtl_sub_commit();mtl_drain();free(mem);
    printf("sparse pool cache: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
