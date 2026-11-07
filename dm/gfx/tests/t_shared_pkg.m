// Registered CPU-backed textures outlive their API objects without repeated uploads.
// The set of CPU-written textures (mtl_tex_cpu_written, tex_cpu_written, mtl_tex_release): membership, no duplicates, a released texture leaving it, and lookups of textures
// not in it staying cheap with thousands of consecutive serials in it (a CFSet that hashed serials as numbers probed a whole bucket run per miss: half of Batman: Arkham Knight's
// rendering thread with 6,442 CPU-written textures)
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



int main(void) { @autoreleasepool {
 mtl_init(); enum { N=1100, STRIDE=65536, FIRST=256 };
 uint8_t *mem=mmap(NULL,STRIDE,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0); if(mem==MAP_FAILED) return 2;
 memset(mem,0x37,STRIDE); NVNmemoryPool pool={0};
 pool_builder_t pb={.mem=mem,.size=STRIDE,.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
 CHECK(h_nvnMemoryPoolInitialize(&pool,(const NVNmemoryPoolBuilder *)&pb),"pool init");
 tex_t first={0};
 for(int i=0;i<N;i++) {
  NVNtextureBuilder tb; NVNtexture t={0}; h_nvnTextureBuilderSetDefaults(&tb);
  h_nvnTextureBuilderSetWidth(&tb,64); h_nvnTextureBuilderSetHeight(&tb,64); h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8);
  h_nvnTextureBuilderSetStorage(&tb,&pool,0); CHECK(h_nvnTextureInitialize(&t,&tb),"texture initialized");
  ((tex_t *)&t)->has_layout=1; if(!i) first=*(tex_t *)&t;
  mtl_reg_tex(FIRST+i,(tex_t *)&t,NULL); h_nvnTextureFinalize(&t);
 }
 uint64_t up=g_pkg_uploads;
 mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); __unsafe_unretained id<MTLBuffer> table=nil;
 pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table); os_unfair_lock_unlock(&g_rmu);
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"shared uploads finish");
 CHECK(g_pkg_uploads==up+1,"1100 serials of identical storage upload once");
 NvPkg *pk=tex_get(first.serial); CHECK(pk && g_pkg_storage_hits>=N-1,"package shared across serials");
 mtl_sub_begin(); os_unfair_lock_lock(&g_rmu);
 id<MTLTexture> a=tex_by_id(FIRST),b=tex_by_id(FIRST+N-1); os_unfair_lock_unlock(&g_rmu);
 CHECK(a==b,"aliases see same uploaded texture");
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"views finish");
 uint64_t changed=g_pkg_reuploads; dm_guest_write_prepare(mem,STRIDE); memset(mem,0x71,STRIDE);
 mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); a=tex_by_id(FIRST); b=tex_by_id(FIRST+N-1); os_unfair_lock_unlock(&g_rmu);
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"write finishes");
 CHECK(g_pkg_reuploads==changed+1 && a==b,"one CPU write refreshes every alias once");
 id<MTLCommandQueue> q=[g_dev newCommandQueue]; id<MTLCommandBuffer> cb=[q commandBuffer];
 id<MTLBuffer> rb=[g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared]; id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
 [be copyFromTexture:b sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,1,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
 [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
 CHECK(cb.status==MTLCommandBufferStatusCompleted && *(uint8_t *)rb.contents==0x71,"second alias sees changed pixels on GPU");
 tex_t different=first; different.layout[0]=1; CHECK(pkg_storage_get(&different)==nil,"packaged layout is part of identity");
 different=first; different.d.width=32; CHECK(pkg_storage_get(&different)==nil,"different shapes are not conflated");
 h_nvnMemoryPoolFinalize(&pool);
 CHECK(g_pkg_storage.n==0 && g_reg_pkg.n==0,"pool destruction drops both owners and cache");
 for(int i=0;i<N;i++) mtl_reg_tex(FIRST+i,NULL,NULL);
 defer_drain();munmap(mem,STRIDE);
 printf("t_shared_pkg: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
