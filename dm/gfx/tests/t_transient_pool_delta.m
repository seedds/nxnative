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

// Retired descriptors whose views live for one submission must be refreshed,
// but their expiry must not force all other bindless entries through resolution.
int main(void) { @autoreleasepool {
    mtl_init();size_t len=65536;
    uint8_t *mem=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(mem==MAP_FAILED)abort();
    NVNmemoryPool pool={0};pool_builder_t pb={.mem=mem,.size=len,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize(&pool,(NVNmemoryPoolBuilder *)&pb),"pool initialized");
    NVNtextureBuilder tb;NVNtexture tex={0};h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb,NVN_TEXTURE_TARGET_BUFFER);
    h_nvnTextureBuilderSetWidth(&tb,64);h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_R32F);
    h_nvnTextureBuilderSetStorage(&tb,&pool,0);
    CHECK(h_nvnTextureInitialize(&tex,&tb),"buffer texture initialized");
    mtl_sub_begin();id<MTLTexture> backing=tex_mtl((tex_t *)&tex);
    CHECK(backing!=nil,"buffer texture backing exists");
    mtl_reg_tex(256,(tex_t *)&tex,NULL);h_nvnTextureFinalize(&tex);
    __unsafe_unretained id<MTLBuffer> table=nil;
    os_unfair_lock_lock(&g_rmu);pool_gpu(0,MTLTextureTypeTextureBuffer,NVN_DPF_FLOAT,0,&table);
    CHECK(g_treg[256].transient_sub==g_sub_serial,"retired view is transient");
    id<MTLTexture> view=(__bridge id<MTLTexture>)g_treg[256].view;
    uint64_t expected=view.gpuResourceID._impl;
    CHECK(((uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD))[256]==expected,"initial table names the backing");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"initial submission completed");
    CHECK(g_treg[256].view==NULL,"transient view expires at submission end");
    for(int i=0;i<3;i++) {
        mtl_sub_begin();uint64_t builds=g_stat_pool_builds;
        os_unfair_lock_lock(&g_rmu);pool_gpu(0,MTLTextureTypeTextureBuffer,NVN_DPF_FLOAT,0,&table);
        view=(__bridge id<MTLTexture>)g_treg[256].view;
        CHECK(view && ((uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD))[256]==view.gpuResourceID._impl,"next submission names refreshed view");
        CHECK(view.buffer==backing.buffer && view.bufferOffset==backing.bufferOffset && view.width==backing.width,"refreshed view names original storage");
        CHECK(g_stat_pool_builds==builds,"transient expiry updates entries without full rebuild");
        CHECK(g_treg[256].transient_sub==g_sub_serial,"refreshed view belongs to current submission");
        os_unfair_lock_unlock(&g_rmu);
        CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"next submission completed");
    }
    // Losing the expiry note to journal wrap must retain the full rebuild path.
    for(int i=0;i<=REG_LOG;i++)reg_bump(257,0);
    mtl_sub_begin();uint64_t builds=g_stat_pool_builds;
    os_unfair_lock_lock(&g_rmu);pool_gpu(0,MTLTextureTypeTextureBuffer,NVN_DPF_FLOAT,0,&table);
    view=(__bridge id<MTLTexture>)g_treg[256].view;
    CHECK(g_stat_pool_builds==builds+1,"journal overflow rebuilds conservatively");
    CHECK(view && ((uint64_t *)((uint8_t *)table.contents+DMSH_POOL_HEAD))[256]==view.gpuResourceID._impl,"overflow rebuild names refreshed view");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"overflow submission completed");
    CHECK(!mtl_last_commit_error(),"no GPU error");
    mtl_reg_tex(256,NULL,NULL);h_nvnMemoryPoolFinalize(&pool);backing=nil;defer_drain();munmap(mem,len);
    printf("t_transient_pool_delta: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
