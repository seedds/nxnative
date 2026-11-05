// Raw address exposure, retired descriptors and compatible texture classes.
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

static void raw_address_apis(void) {
    enum { SIZE = 65536, N = 3 };
    void *mem = mmap(NULL, N*SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    NVNmemoryPool pools[N] = {0};
    for (int i=0; i<N; i++) {
        pool_builder_t b = {.mem=(uint8_t *)mem+i*SIZE, .size=SIZE, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
        CHECK(h_nvnMemoryPoolInitialize(&pools[i], (const NVNmemoryPoolBuilder *)&b), "raw API pool initializes");
    }
    int before = g_pool_res_n;
    mtl_sub_begin(); gmem_map_gpu(); CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "unexposed table commits");
    CHECK(g_pool_res_n == before, "unexposed CPU-only pools stay unpinned");
    CHECK(h_nvnMemoryPoolGetBufferAddress(&pools[0]) == (NVNbufferAddress)(uintptr_t)mem, "pool address getter preserves the address");
    NVNbuffer buffer = {0}; buf_builder_t b = {.pool=(pool_t *)&pools[1], .off=16, .size=128};
    int made = h_nvnBufferInitialize(&buffer, (const NVNbufferBuilder *)&b);
    CHECK(made, "buffer initialization exposes its pool");
    if (!made) { for (int i=0;i<N;i++) h_nvnMemoryPoolFinalize(&pools[i]); mtl_finish(); munmap(mem,N*SIZE); return; }
    CHECK(h_nvnBufferGetAddress(&buffer) == (NVNbufferAddress)(uintptr_t)((uint8_t *)mem+SIZE+16), "buffer getter preserves the address and offset");
    tex_t texture = {.pool=(pool_t *)&pools[2], .off=32};
    CHECK(h_nvnTextureGetTextureAddress((const NVNtexture *)&texture) == (NVNtextureAddress)(uintptr_t)((uint8_t *)mem+2*SIZE+32), "texture address getter exposes its pool");
    mtl_sub_begin(); gmem_map_gpu(); CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(), "exposed address table commits");
    CHECK(g_pool_res_n == before+N, "every API-exposed pool becomes resident");
    h_nvnBufferFinalize(&buffer);
    for (int i=0; i<N; i++) h_nvnMemoryPoolFinalize(&pools[i]);
    mtl_finish(); munmap(mem,N*SIZE);
}
static void retired_gpu_aliases(void) {
    enum { N = 32, ROUNDS = 32, FIRST = 10000, STRIDE = 65536 };
    size_t len = N * STRIDE;
    void *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=len,
        .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, len);
    tex_t probe = {.pool=&pool, .mem=mem, .L={.size=65536}};
    int resident = g_pool_res_n;
    CHECK(tex_storage_live(&probe) && g_pool_res_n == resident, "storage validation does not pin its pool");
    probe.mem = (uint8_t *)mem + len - 1;
    CHECK(!tex_storage_live(&probe), "storage validation rejects a range past its pool");
    for (int round = 0; round < ROUNDS; round++) {
        mtl_sub_begin();
        for (int i = 0; i < N; i++) {
            tex_t t = {.magic=MAGIC('T','E','X',0), .serial=nvn_new_serial(), .pool=&pool,
                .off=i * STRIDE, .mem=(uint8_t *)mem+i * STRIDE,
                .d={.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8,
                    .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width=32+round, .height=64, .depth=1, .levels=1, .samples=1}};
            nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t);
            mtl_reg_tex(FIRST+round*N+i, &t, NULL);
            CHECK(tex_mtl(&t) != nil, "live GPU texture is created before object finalization");
            mtl_tex_release(&t);
        }
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "alias replacement submission completes");
    }
    uint64_t taken = g_alias_taken; unsigned aliases = g_alias.n;
    mtl_sub_begin(); __unsafe_unretained id<MTLBuffer> table = nil;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, FIRST, &table);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(table != nil, "retired descriptor table resolves");
    CHECK(g_alias_taken == taken && g_alias.n == aliases, "a descriptor scan cannot recreate evicted retired GPU textures");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "retired descriptor scan completes");
    for (int i = 0; i < N*ROUNDS; i++) mtl_reg_tex(FIRST+i, NULL, NULL);
    mtl_alias_drop_pool(&pool); mtl_pool_del(mem); mtl_finish(); munmap(mem, len);
}
static void texture_classes(void) {
    const int SIZE = 65536, TID = 128;
    void *mem = mmap(NULL, SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=SIZE,
        .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem,SIZE);
    tex_t volume = {.magic=MAGIC('T','E','X',0), .serial=nvn_new_serial(), .mem=mem, .pool=&pool,
        .d={.target=NVN_TEXTURE_TARGET_3D, .format=NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=4, .levels=1, .samples=1}};
    tex_t cube = volume; cube.serial=nvn_new_serial(); cube.off=8192; cube.mem=(uint8_t *)mem+8192; cube.d.target=NVN_TEXTURE_TARGET_CUBEMAP; cube.d.depth=1;
    nvn_tex_layout(&volume.d,&volume.L); nvn_tex_layout(&cube.d,&cube.L);
    mtl_tex_init(&volume); mtl_tex_init(&cube); mtl_reg_tex(TID,&volume,NULL); mtl_reg_tex(TID+1,&cube,NULL);
    __unsafe_unretained id<MTLBuffer> table = nil;
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2DArray,NVN_DPF_FLOAT,0,&table);
    const uint64_t *ids=(const uint64_t *)((const uint8_t *)table.contents+DMSH_POOL_HEAD);
    CHECK(!tex_made(&volume), "incompatible 3D storage is not allocated for a 2D-array table");
    CHECK(ids[TID+1] != ids[TID] && tex_made(&cube).textureType == MTLTextureType2DArray, "cube faces retain their real 2D-array backing in the table");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(), "compatible-class table completes");
    tex_t linear = cube; linear.serial=nvn_new_serial(); linear.off=16384; linear.mem=(uint8_t *)mem+16384; linear.d.target=NVN_TEXTURE_TARGET_2D;
    nvn_tex_layout(&linear.d,&linear.L); mtl_tex_init(&linear);
    mtl_sub_begin(); id<MTLTexture> base=tex_mtl(&linear);
    tex_t srgb=linear; srgb.serial=nvn_new_serial(); srgb.d.format=NVN_FORMAT_RGBA8_SRGB;
    mtl_tex_init(&srgb); mtl_reg_tex(TID+2,&srgb,NULL); mtl_tex_release(&srgb);
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> sibling=tex_by_id(TID+2); os_unfair_lock_unlock(&g_rmu);
    CHECK(sibling && sibling.pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB && sibling.parentTexture==base, "a retired descriptor keeps the existing linear/sRGB storage twin");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(), "retired storage-twin view completes");
    mtl_reg_tex(TID+2,NULL,NULL); mtl_tex_release(&linear);
    tex_t compressed = {.d={.flags=NVN_TEXTURE_FLAGS_IMAGE,.format=NVN_FORMAT_RGBA_DXT1}};
    CHECK(!img_usage(&compressed,MTLPixelFormatBC1_RGBA), "compressed IMAGE storage is read-only");
    compressed.d.format=NVN_FORMAT_RGBA8;
    CHECK(img_usage(&compressed,MTLPixelFormatRGBA8Unorm)&MTLTextureUsageShaderWrite, "uncompressed IMAGE storage remains writable");
    mtl_reg_tex(TID,NULL,NULL); mtl_reg_tex(TID+1,NULL,NULL); mtl_tex_release(&volume); mtl_tex_release(&cube);
    mtl_alias_drop_pool(&pool); mtl_pool_del(mem); mtl_finish(); munmap(mem,SIZE);
}
int main(void) { @autoreleasepool {
    mtl_init(); retired_gpu_aliases(); raw_address_apis(); texture_classes();
    printf("raw residency: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
