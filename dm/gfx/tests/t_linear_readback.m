// NVN linear image storage: GPU image writes must be visible through the memory-pool CPU mapping.
// Synthetic 160x90 RGBA8_SRGB IMAGE|LINEAR textures use a valid 640-byte row pitch. A linear texture whose rows no view can take (DM_TBUF_ALIGN=64) is uploaded at its stride.
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
static int cases, failed;
#define EXPECT(n,w,v) do { int a=(w), b=(v); cases++; printf("%s expected %d -> got %d %s\n",n,a,b,a==b?"PASS":"FAIL"); failed+=a!=b; } while (0)
static void run_case(int format) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=262144, .flags=0x24}; mtl_pool_add(mem, 262144);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 160);
    h_nvnTextureBuilderSetHeight(&tb, 90); h_nvnTextureBuilderSetFormat(&tb, format);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, 640);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 65536); EXPECT("linear texture init", 1, h_nvnTextureInitialize(&tex, &tb));
    id<MTLTexture> t = tex_mtl((const tex_t *)&tex);
    printf("LINEAR format=%#x Metal=%lu minimumAlignment=%lu bufferBacked=%d row=640\n", format, (unsigned long)t.pixelFormat,
        (unsigned long)[g_dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat], t.buffer != nil);
    EXPECT("host texture resolved", 1, t != nil); if (!t) return;
    // NVN image stores use the shader image format, without sRGB conversion. A uint image view writes literal packed bytes.
    mtl_reg_tex(1, (const tex_t *)&tex, NULL); uint64_t rid;
    mtl_sub_begin(); id<MTLTexture> image = img_fit(1, t, NVN_DPF_UINT, t.textureType, &rid);
    if (!image) { img_fit_upgrade(); t = tex_mtl((const tex_t *)&tex); image = img_fit(1, t, NVN_DPF_UINT, t.textureType, &rid); }
    mtl_wait(mtl_sub_commit(), 5000000000ull); EXPECT("production uint image view", 1, image != nil);
    NSError *err = nil; const char *src = "#include <metal_stdlib>\nusing namespace metal;\nkernel void fill(texture2d<uint,access::write> out [[texture(0)]], uint2 p [[thread_position_in_grid]]) { if(p.x<160&&p.y<90) out.write(uint4(0xff996633u,0,0,0),p); }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&err]; if (!lib) { fprintf(stderr,"%s\n",err.description.UTF8String); abort(); }
    id<MTLComputePipelineState> ps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err]; if (!ps) abort();
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder]; [en setComputePipelineState:ps]; [en setTexture:image atIndex:0];
    [en dispatchThreads:MTLSizeMake(160,90,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("image compute complete", MTLCommandBufferStatusCompleted, cb.status);
    h_nvnMemoryPoolInvalidateMappedRange((const NVNmemoryPool *)&pool, 65536, 640*90);
    uint8_t *map = (uint8_t *)h_nvnMemoryPoolMap((const NVNmemoryPool *)&pool) + 65536; int bad = 0;
    static const uint8_t want[] = {51,102,153,255}; for(int i=0;i<160*90;i++) bad += memcmp(map+i*4,want,4) != 0;
    EXPECT("GPU image writes -> CPU pool map, 14400 literal pixels", 0, bad);
    h_nvnTextureFinalize(&tex); mtl_pool_del(mem); mtl_alias_drop_pool(&pool); free(mem);   // as nvnMemoryPoolFinalize: the next case's pool object has the same address
}
static void render_target_stride_case(void) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 65536)) abort(); memset(mem, 0, 65536);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=65536, .flags=0x24}; mtl_pool_add(mem, 65536);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 16); h_nvnTextureBuilderSetHeight(&tb, 16);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 0);
    EXPECT("linear target inferred stride storage", 2048, h_nvnTextureBuilderGetStorageSize(&tb));
    EXPECT("linear target init", 1, h_nvnTextureInitialize(&tex, &tb));
    id<MTLTexture> t = tex_mtl((const tex_t *)&tex);
    EXPECT("linear target has buffer backing", 1, t.buffer != nil);
    EXPECT("linear target inferred row pitch", 128, t.bufferBytesPerRow);
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    pd.colorAttachments[0].texture=t; pd.colorAttachments[0].loadAction=MTLLoadActionClear;
    pd.colorAttachments[0].storeAction=MTLStoreActionStore; pd.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
    [[cb renderCommandEncoderWithDescriptor:pd] endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("linear target clear completed", MTLCommandBufferStatusCompleted, cb.status);
    static const uint8_t red[]={255,0,0,255}; int bad=0, padding=0;
    for (int y=0;y<16;y++) {
        for (int x=0;x<16;x++) bad += memcmp((uint8_t *)mem+y*128+x*4,red,4)!=0;
        for (int x=64;x<128;x++) padding += ((uint8_t *)mem)[y*128+x]!=0;
    }
    EXPECT("linear target clear uses padded rows", 0, bad); EXPECT("linear target leaves row padding alone", 0, padding);
    h_nvnTextureFinalize(&tex); mtl_pool_del(mem); free(mem);
}
static void scaled_copy_case(void) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=262144, .flags=0x24}; mtl_pool_add(mem, 262144);
    NVNtextureBuilder tb; NVNtexture dst; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 160);
    h_nvnTextureBuilderSetHeight(&tb, 90); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, 640);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 65536); EXPECT("scaled copy destination init", 1, h_nvnTextureInitialize(&dst, &tb));
    tex_t src = {.magic=MAGIC('T','E','X',0), .serial=900001}; src.d.width=320; src.d.height=180; src.d.depth=src.d.levels=src.d.samples=1;
    src.d.target=NVN_TEXTURE_TARGET_2D; src.d.format=NVN_FORMAT_RGBA8_SRGB;
    id<MTLTexture> source = tex_mtl(&src); id<MTLCommandBuffer> seed = q_cb();
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor]; pd.colorAttachments[0].texture=source;
    pd.colorAttachments[0].loadAction=MTLLoadActionClear; pd.colorAttachments[0].storeAction=MTLStoreActionStore; pd.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
    [[seed renderCommandEncoderWithDescriptor:pd] endEncoding]; [seed commit]; [seed waitUntilCompleted];
    EXPECT("scaled copy source seeded", MTLCommandBufferStatusCompleted, seed.status);
    NVNcommandBuffer cmd; EXPECT("scaled copy command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion sr={0,0,0,320,180,1}, dr={0,0,0,160,90,1};
    h_nvnCommandBufferCopyTextureToTexture(&cmd, (const NVNtexture *)&src, NULL, &sr, &dst, NULL, &dr, NVN_COPY_FLAGS_LINEAR_FILTER_BIT);
    NVNcommandHandle h=h_nvnCommandBufferEndRecording(&cmd); state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL));
    EXPECT("scaled copy committed", 1, mtl_wait(mtl_sub_commit(),5000000000ull)); EXPECT("scaled copy GPU error",0,mtl_last_commit_error());
    h_nvnMemoryPoolInvalidateMappedRange((const NVNmemoryPool *)&pool,65536,640*90); uint8_t *map=(uint8_t *)h_nvnMemoryPoolMap((const NVNmemoryPool *)&pool)+65536;
    static const uint8_t want[]={255,0,0,255}; int bad=0; for(int i=0;i<160*90;i++)bad+=memcmp(map+i*4,want,4)!=0;
    EXPECT("scaled render copy -> CPU pool map, 14400 literal red pixels",0,bad);
    EXPECT("scaled destination keeps pool backing",1,tex_mtl((const tex_t *)&dst).buffer!=nil);
    h_nvnTextureFinalize(&dst); mtl_tex_release(&src); mtl_pool_del(mem); free(mem);
}
// Linear textures whose rows the device cannot view on the pool's buffer (DM_TBUF_ALIGN=64, as iOS asks; Axiom Verge's 8x8 RGBA8 textures have 32-byte rows). 6x6 RGBA8: 24 bytes of
// pixels in each row, the rest of the row filled so that a wrong stride shows.
//   0: LINEAR, 32-byte rows: uploaded from its storage at its stride, and ReadTexels reads its pixels; then an NVN copy into it, a CPU write, reads on another thread and an upload
//   1: LINEAR, 64-byte rows: stays a view
//   2: LINEAR, 32-byte rows, read by ReadTexels on another thread before its first use: the read has its pixels, and the first use on the submit thread still uploads it
//   3: IMAGE|LINEAR, 32-byte rows, only sampled (Hades II's Bink planes, GitHub issue #1956): CPU-written while the GPU has not written its storage, uploaded again after a CPU
//      write; a recorded GPU write into it makes it a GPU texture that keeps its content
//   4: IMAGE|LINEAR, 32-byte rows, its storage written by the GPU before its first use: a GPU texture, not CPU-written, its first fill at its stride
//   5: LINEAR_RENDER_TARGET, 32-byte rows, only sampled: CPU-written, as 3
static int blit_bad(id<MTLTexture> t, const uint8_t *stored, int stride) {   // rows of the texture unlike the storage's
    id<MTLBuffer> out = [g_dev newBufferWithLength:24 * 6 options:MTLResourceStorageModeShared]; if (!out) return 6;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(6, 6, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:24 destinationBytesPerImage:24 * 6];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int bad = 0; for (int y = 0; y < 6; y++) bad += memcmp((uint8_t *)out.contents + y * 24, stored + y * stride, 24) != 0;
    return bad;
}
static int read_bad(const NVNtexture *tex, const uint8_t *stored, int stride) {   // rows ReadTexels returns unlike the storage's
    uint8_t got[24 * 6]; memset(got, 0, sizeof got); NVNcopyRegion r = {0, 0, 0, 6, 6, 1};
    h_nvnTextureReadTexels(tex, NULL, &r, got);
    int bad = 0; for (int y = 0; y < 6; y++) bad += memcmp(got + y * 24, stored + y * stride, 24) != 0;
    return bad;
}
typedef struct { const NVNtexture *tex; const uint8_t *stored; int bad; } read_job;
static void *read_on_thread(void *p) { read_job *j = p; j->bad = read_bad(j->tex, j->stored, 32); return NULL; }
static void *read_tight_on_thread(void *p) { read_job *j = p; j->bad = read_bad(j->tex, j->stored, 24); return NULL; }
static void read_job_tight(read_job *j) { pthread_t th; if (pthread_create(&th, NULL, read_tight_on_thread, j)) abort(); pthread_join(th, NULL); }   // rows compared 24 bytes apart
static void unaligned_upload_case(void) {   // a pool made through NVN, so that its pages are write-tracked as a game's are (an uploaded texture is uploaded again only after a write)
    const size_t len = 262144; uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort(); memset(mem, 0xee, len);
    pool_t pool = {0}; pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    EXPECT("unaligned case: pool init", 1, h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb));
    enum { N = 6 }; NVNtexture tex[N]; id<MTLTexture> t[N];
    int stride[N] = {32, 64, 32, 32, 32, 32}, flags[N] = {NVN_TEXTURE_FLAGS_LINEAR, NVN_TEXTURE_FLAGS_LINEAR, NVN_TEXTURE_FLAGS_LINEAR, NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR,
                                                    NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET};
    const uint8_t *stored[N];
    for (int i = 0; i < N; i++) {
        uint64_t at = 65536 + (uint64_t)i * 4096; stored[i] = mem + at;
        for (int y = 0; y < 6; y++) for (int x = 0; x < 6; x++) { uint8_t *p = mem + at + y * stride[i] + x * 4; p[0] = (uint8_t)(x * 40 + i); p[1] = (uint8_t)(y * 40); p[2] = 0x5a; p[3] = 0xff; }
        NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 6); h_nvnTextureBuilderSetHeight(&tb, 6);
        h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, flags[i]); h_nvnTextureBuilderSetStride(&tb, stride[i]);
        h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, (ptrdiff_t)at);
        EXPECT("unaligned case: texture init", 1, h_nvnTextureInitialize(&tex[i], &tb));
    }
    h_nvnMemoryPoolFlushMappedRange((const NVNmemoryPool *)&pool, 65536 + 4 * 4096, 32 * 6);   // 4: flushed, so its first fill takes the storage (tex_init_fill; the pool is CPU_CACHED)
    mtl_tex_gpu_written((const tex_t *)&tex[4]);   // 4: a GPU write into its storage is recorded before its first use, as a render target, clear, copy or image binding records it
    mtl_sub_begin();   // this thread is the submit thread from here on (tex_mtl_ uploads only there)
    read_job job = {&tex[2], stored[2], -1}; pthread_t th;
    if (pthread_create(&th, NULL, read_on_thread, &job)) abort();
    pthread_join(th, NULL);
    EXPECT("2: ReadTexels on another thread before the first use has its 6 rows", 0, job.bad);
    for (int i = 0; i < N; i++) t[i] = tex_mtl((const tex_t *)&tex[i]);
    EXPECT("0: not a view on the pool's buffer", 1, t[0] && !t[0].buffer);
    EXPECT("1: a view on the pool's buffer", 1, t[1] && t[1].buffer && t[1].bufferBytesPerRow == 64);
    EXPECT("2: not a view on the pool's buffer after the read", 1, t[2] && !t[2].buffer);
    for (int i = 3; i < N; i++) EXPECT(i == 3 ? "3: not a view on the pool's buffer" : i == 4 ? "4: not a view on the pool's buffer" : "5: not a view on the pool's buffer", 1, t[i] && !t[i].buffer);
    EXPECT("0: CPU-written", 1, tex_cpu_written(((const tex_t *)&tex[0])->serial));
    EXPECT("2: CPU-written", 1, tex_cpu_written(((const tex_t *)&tex[2])->serial));
    EXPECT("3: CPU-written (the GPU has not written its storage)", 1, tex_cpu_written(((const tex_t *)&tex[3])->serial));
    EXPECT("4: not CPU-written (the GPU wrote its storage)", 0, tex_cpu_written(((const tex_t *)&tex[4])->serial));
    EXPECT("5: CPU-written (the GPU has not written its storage)", 1, tex_cpu_written(((const tex_t *)&tex[5])->serial));
    mtl_wait(mtl_sub_commit(), 5000000000ull);
    static const char *const up[N] = {"0: 6 rows uploaded at the storage's stride", "1: 6 rows read as stored", "2: 6 rows uploaded at the storage's stride", "3: 6 rows uploaded at the storage's stride",
                                      "4: 6 rows filled at the storage's stride", "5: 6 rows uploaded at the storage's stride"};
    for (int i = 0; i < N; i++) if (t[i]) EXPECT(up[i], 0, blit_bad(t[i], stored[i], stride[i]));
    EXPECT("0: ReadTexels has its 6 rows", 0, read_bad(&tex[0], stored[0], 32));
    EXPECT("1: ReadTexels has its 6 rows", 0, read_bad(&tex[1], stored[1], 64));
    // 0, after an NVN copy from 1 into it: the copy lands in its storage too (tex_storage_dst), so ReadTexels on another thread has the copied rows; a CPU write after that is read there,
    // without the texture being uploaded on that thread; and the next upload on the submit thread carries both
    NVNcommandBuffer cmd; EXPECT("unaligned case: command buffer init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion cr = {0, 0, 0, 6, 6, 1}; h_nvnCommandBufferCopyTextureToTexture(&cmd, &tex[1], NULL, &cr, &tex[0], NULL, &cr, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd); state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL));
    EXPECT("0: the copy from 1 committed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    uint8_t want[24 * 6]; for (int y = 0; y < 6; y++) memcpy(want + y * 24, stored[1] + y * 64, 24);
    read_job after_copy = {&tex[0], want, -1}; read_job_tight(&after_copy);
    EXPECT("0: ReadTexels on another thread after the copy has the copied rows", 0, after_copy.bad);
    dm_guest_write_prepare(mem + 65536, 24); memset(mem + 65536, 0x77, 24); memset(want, 0x77, 24);   // the CPU writes row 0 (marked written, as the write fault does for a game)
    read_job after_write = {&tex[0], want, -1}; read_job_tight(&after_write);
    EXPECT("0: ReadTexels on another thread after a CPU write has it, and the copied rows", 0, after_write.bad);
    mtl_sub_begin(); id<MTLTexture> again = tex_mtl((const tex_t *)&tex[0]); mtl_wait(mtl_sub_commit(), 5000000000ull);
    EXPECT("0: uploaded again on the submit thread with the CPU's row and the copied ones", 0, again ? blit_bad(again, want, 24) : 6);
    // 3, as a movie decoder writes a frame: the CPU's row is read on another thread and uploaded at the next use. Then a recorded GPU write into its storage makes it a GPU texture
    // (cpu_unc_drop at the next resolve), with the content it has
    uint8_t want3[24 * 6]; for (int y = 0; y < 6; y++) memcpy(want3 + y * 24, stored[3] + y * 32, 24);
    dm_guest_write_prepare(mem + 65536 + 3 * 4096, 24); memset(mem + 65536 + 3 * 4096, 0x66, 24); memset(want3, 0x66, 24);
    read_job frame = {&tex[3], want3, -1}; read_job_tight(&frame);
    EXPECT("3: ReadTexels on another thread after a CPU write has it", 0, frame.bad);
    mtl_sub_begin(); id<MTLTexture> next = tex_mtl((const tex_t *)&tex[3]); mtl_wait(mtl_sub_commit(), 5000000000ull);
    EXPECT("3: uploaded again on the submit thread with the CPU's row", 0, next ? blit_bad(next, want3, 24) : 6);
    mtl_tex_gpu_written((const tex_t *)&tex[3]);
    mtl_sub_begin(); id<MTLTexture> gpu = tex_mtl((const tex_t *)&tex[3]); mtl_wait(mtl_sub_commit(), 5000000000ull);
    EXPECT("3: not CPU-written after a GPU write into its storage", 0, tex_cpu_written(((const tex_t *)&tex[3])->serial));
    EXPECT("3: the GPU texture keeps its content", 0, gpu ? blit_bad(gpu, want3, 24) : 6);
    EXPECT("3: ReadTexels reads the GPU texture", 0, read_bad(&tex[3], want3, 24));
    h_nvnCommandBufferFinalize(&cmd);
    for (int i = 0; i < N; i++) h_nvnTextureFinalize(&tex[i]);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool); defer_drain(); munmap(mem, len);
}
int main(void) { @autoreleasepool { setenv("DM_TBUF_ALIGN", "64", 1);   // before the first lookup (tbuf_align reads it once); the other cases' rows and offsets are 64-byte aligned
    mtl_init(); run_case(NVN_FORMAT_RGBA8); run_case(NVN_FORMAT_RGBA8_SRGB);
    render_target_stride_case();
    scaled_copy_case();
    unaligned_upload_case();
    printf("LINEAR_READBACK cases=%d failed=%d\n",cases,failed); return failed != 0;
} }
