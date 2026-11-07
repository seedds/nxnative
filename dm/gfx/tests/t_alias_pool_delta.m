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

// An alias takeover changes one storage family. Unrelated bindless entries
// must retain their resource IDs without rebuilding the entire table.
int main(void) { @autoreleasepool {
    mtl_init();
    enum { COUNT = 64, STRIDE = 65536, FIRST = 256 };
    size_t len = (COUNT + 1) * STRIDE;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    NVNmemoryPool pool = {0};
    pool_builder_t pb = {.mem=mem,.size=len,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize(&pool, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    mtl_sub_begin();
    for (int i=0;i<COUNT;i++) {
        NVNtextureBuilder tb; NVNtexture tex = {0};
        h_nvnTextureBuilderSetDefaults(&tb);
        h_nvnTextureBuilderSetWidth(&tb,64); h_nvnTextureBuilderSetHeight(&tb,64);
        h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);
        h_nvnTextureBuilderSetStorage(&tb,&pool,(ptrdiff_t)i*STRIDE);
        CHECK(h_nvnTextureInitialize(&tex,&tb), "initial texture initialized");
        CHECK(tex_mtl((tex_t *)&tex)!=nil, "initial backing created");
        mtl_reg_tex(FIRST+i,(tex_t *)&tex,NULL);
        h_nvnTextureFinalize(&tex);
    }
    // A retired descriptor can initially have no backing, then acquire one
    // when another live object creates the same storage family.
    NVNtextureBuilder future_tb; NVNtexture future={0};
    h_nvnTextureBuilderSetDefaults(&future_tb);
    h_nvnTextureBuilderSetWidth(&future_tb,64); h_nvnTextureBuilderSetHeight(&future_tb,64);
    h_nvnTextureBuilderSetFormat(&future_tb,NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&future_tb,&pool,COUNT*STRIDE);
    CHECK(h_nvnTextureInitialize(&future,&future_tb), "future descriptor initialized");
    mtl_reg_tex(FIRST+COUNT,(tex_t *)&future,NULL);
    h_nvnTextureFinalize(&future);
    NVNtextureBuilder twin_tb=future_tb; NVNtexture twin={0};
    h_nvnTextureBuilderSetFormat(&twin_tb,NVN_FORMAT_RGBA8_SRGB);
    CHECK(h_nvnTextureInitialize(&twin,&twin_tb), "sRGB twin initialized");
    mtl_reg_tex(FIRST+COUNT+1,(tex_t *)&twin,NULL); h_nvnTextureFinalize(&twin);
    NVNtextureBuilder image_tb=future_tb; NVNtexture image={0};
    h_nvnTextureBuilderSetFlags(&image_tb,((tex_builder_t *)&image_tb)->d.flags | NVN_TEXTURE_FLAGS_IMAGE);
    CHECK(h_nvnTextureInitialize(&image,&image_tb), "IMAGE variant initialized");
    mtl_reg_tex(FIRST+COUNT+2,(tex_t *)&image,NULL); h_nvnTextureFinalize(&image);
    os_unfair_lock_lock(&g_rmu);
    __unsafe_unretained id<MTLBuffer> table=nil;
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    uint64_t saved[COUNT];
    memcpy(saved,(uint8_t *)table.contents+DMSH_POOL_HEAD+FIRST*8,sizeof saved);
    uint64_t *initial=(uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD);
    uint64_t dummy=initial[*(uint32_t *)table.contents];
    CHECK(initial[FIRST+COUNT]==dummy, "missing retired backing starts as dummy");
    CHECK(initial[FIRST+COUNT+1]==dummy, "missing sRGB backing starts as dummy");
    CHECK(initial[FIRST+COUNT+2]==dummy, "missing IMAGE backing starts as dummy");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "initial submission completed");
    for (int i=0;i<ALIAS_IDLE+2;i++) { mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "idle submission completed"); }
    mtl_sub_begin();
    NVNtextureBuilder tb; NVNtexture replacement={0};
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb,65); h_nvnTextureBuilderSetHeight(&tb,64);
    h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb,&pool,0);
    CHECK(h_nvnTextureInitialize(&replacement,&tb), "replacement initialized");
    uint64_t taken=g_alias_taken;
    CHECK(tex_mtl((tex_t *)&replacement)!=nil, "replacement backing created");
    CHECK(g_alias_taken>taken, "overlapping idle storage was taken over");
    uint64_t builds=g_stat_pool_builds;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    uint64_t *entries=(uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD);
    for (int i=1;i<COUNT;i++) CHECK(entries[FIRST+i]==saved[i], "unrelated resource ID preserved");
    void *resolved=NULL;
    CHECK(entries[FIRST]==tex_fast_rid(FIRST,&resolved), "affected entry follows current storage");
    CHECK(g_stat_pool_builds==builds, "storage takeover patches affected entries without a full table rebuild");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "replacement submission completed");
    mtl_sub_begin();
    NVNtexture created={0};
    CHECK(h_nvnTextureInitialize(&created,&future_tb), "future storage object initialized");
    CHECK(tex_mtl((tex_t *)&created)!=nil, "future backing created");
    builds=g_stat_pool_builds;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table);
    entries=(uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD);
    CHECK(entries[FIRST+COUNT]!=dummy, "new alias replaces previously cached dummy");
    CHECK(entries[FIRST+COUNT]==tex_fast_rid(FIRST+COUNT,&resolved), "new alias matches resolved descriptor");
    CHECK(entries[FIRST+COUNT+1]!=dummy, "new alias updates sRGB twin");
    CHECK(entries[FIRST+COUNT+1]==tex_fast_rid(FIRST+COUNT+1,&resolved), "sRGB twin follows current storage");
    CHECK(resolved && ((__bridge id<MTLTexture>)resolved).pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB, "sRGB interpretation retained");
    CHECK(entries[FIRST+COUNT+2]==entries[FIRST+COUNT], "IMAGE flag retains shared storage");
    CHECK(g_stat_pool_builds==builds, "new alias patches without full rebuild");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "new backing submission completed");
    CHECK(!mtl_last_commit_error(), "no GPU error");
    h_nvnTextureFinalize(&replacement);
    h_nvnTextureFinalize(&created);
    for (int i=0;i<=COUNT+2;i++) mtl_reg_tex(FIRST+i,NULL,NULL);
    CHECK(g_treg_alias.n==0, "unregister removes every storage-family index entry");
    h_nvnMemoryPoolFinalize(&pool); defer_drain(); munmap(mem,len);
    printf("t_alias_pool_delta: %d checks, %d failed\n",checks,failed);
    return failed!=0;
} }
