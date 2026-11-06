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
 mtl_init(); enum { STRIDE=65536, FIRST=256 }; int N=1024;
 size_t len=N*STRIDE; uint8_t *mem=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
 if(mem==MAP_FAILED) return 2; memset(mem,0x37,len);
 NVNmemoryPool pool={0}; pool_builder_t pb={.mem=mem,.size=len,.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
 CHECK(h_nvnMemoryPoolInitialize(&pool,(const NVNmemoryPoolBuilder *)&pb),"pool init");
 for(int i=0;i<N;i++) {
  NVNtextureBuilder tb; NVNtexture t={0}; h_nvnTextureBuilderSetDefaults(&tb);
  h_nvnTextureBuilderSetWidth(&tb,64); h_nvnTextureBuilderSetHeight(&tb,64);
  h_nvnTextureBuilderSetFormat(&tb,NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetStorage(&tb,&pool,(ptrdiff_t)i*STRIDE);
  CHECK(h_nvnTextureInitialize(&t,&tb),"texture init");
  mtl_tex_cpu_written((tex_t *)&t); mtl_reg_tex(FIRST+i,(tex_t *)&t,NULL); h_nvnTextureFinalize(&t);
 }
 for(int round=0;round<4;round++) {
  uint64_t up=g_pkg_uploads,re=g_pkg_reuploads; mtl_sub_begin();
  os_unfair_lock_lock(&g_rmu); __unsafe_unretained id<MTLBuffer> table=nil;
  pool_gpu(0,MTLTextureType2D,NVN_DPF_FLOAT,0,&table); os_unfair_lock_unlock(&g_rmu);
  CHECK(table!=nil,"table exists"); CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"submit done");
  CHECK(g_pkg_uploads-up == (round ? 0 : N), "unchanged retired registrations upload only once");
  printf("ROUND %d uploads=%llu reuploads=%llu ghosts=%u\n",round,(unsigned long long)(g_pkg_uploads-up),(unsigned long long)(g_pkg_reuploads-re),g_ghost.n);
 }

 CHECK(g_reg_pkg.n == N, "registrations own their packages");
 uint64_t serial=g_treg[FIRST].t.serial;
 mtl_reg_tex(FIRST+N,&g_treg[FIRST].t,NULL);
 mtl_reg_tex(FIRST,NULL,NULL);
 CHECK(nvn_hm_get(&g_reg_pkg_refs,&serial)==(void *)1, "duplicate registration shares ownership");
 CHECK(hm_get_id(&g_reg_pkg,&serial)!=nil, "removing one registration keeps shared package");
 mtl_reg_tex(FIRST+N,NULL,NULL);
 CHECK(hm_get_id(&g_reg_pkg,&serial)==nil, "last registration releases package");
 mtl_sub_begin();
 tex_t snapshot=g_treg[FIRST+1].t;
 dm_guest_write_prepare(snapshot.mem,snapshot.L.size); memset(snapshot.mem,0x71,snapshot.L.size);
 uint64_t before=g_pkg_reuploads;
 id<MTLTexture> updated=tex_mtl(&snapshot);
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"updated upload completes");
 CHECK(g_pkg_reuploads==before+1,"retained package still follows CPU writes");
 id<MTLCommandQueue> q=[g_dev newCommandQueue]; id<MTLCommandBuffer> cb=[q commandBuffer];
 id<MTLBuffer> b=[g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
 id<MTLBlitCommandEncoder> be=[cb blitCommandEncoder];
 [be copyFromTexture:updated sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,1,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
 [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
 CHECK(cb.status==MTLCommandBufferStatusCompleted && *(uint8_t *)b.contents==0x71,"GPU sees updated pixels");
 treg_drop_pool_views(&pool);
 CHECK(g_reg_pkg.n==0 && g_reg_pkg_refs.n==0,"pool destruction releases every descriptor owner");
 for(int i=0;i<N;i++) mtl_reg_tex(FIRST+i,NULL,NULL);
 CHECK(g_reg_pkg.n==0 && g_reg_pkg_refs.n==0,"unregistering gone descriptors does not resurrect ownership");

 h_nvnMemoryPoolFinalize(&pool); defer_drain(); munmap(mem,len);
 printf("checks=%d failed=%d\n",checks,failed); return failed!=0;
} }
