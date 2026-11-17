// Live BUFFER reads through public NVN builder, registration and queue replay. Only
// loading a game shader is replaced by synthetic MSL with the same reflection.
// The GPU stays gated until both producers and their consumers are encoded.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#include <unistd.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void crash_install(void);
#define P(name) ((NVNT_##name)nvn_lookup(#name))
static int checks, failed;
static NSMutableArray *fixture_shaders;   // pointer-keyed production PSO caches outlive each case
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s (line %d)\n",label,__LINE__); } } while (0)
static void require(int ok, const char *label) { CHECK(ok,label); if (!ok) exit(2); }
enum { POOL_BYTES=0x100000, COMMAND_BYTES=0x20000, DESCRIPTORS=0x20000,
       SOURCE=0x30000, OUTPUT=0x40000, FIRST=0x50000, SECOND=0x51000,
       REMAP_SECOND=0x54000, COUNT=16, TEXTURE_ID=256 };
static NVNdevice device;
static NVNqueue queue;
static NVNcommandBuffer command;
static NVNmemoryPool pool;
static NVNtexturePool textures;
static NVNprogram program;
static NVNbufferAddress base;
static uint8_t *memory;
static uint32_t literal(int version, int i) { return (version ? 0x23456780u : 0x12345670u) ^ (uint32_t)i; }
static void texture_init_n(NVNtexture *texture, NVNmemoryPool *storage, unsigned offset, unsigned count) {
    NVNtextureBuilder builder={0};
    P(nvnTextureBuilderSetDefaults)(&builder); P(nvnTextureBuilderSetDevice)(&builder,&device);
    P(nvnTextureBuilderSetTarget)(&builder,NVN_TEXTURE_TARGET_BUFFER);
    P(nvnTextureBuilderSetSize1D)(&builder,count); P(nvnTextureBuilderSetFormat)(&builder,NVN_FORMAT_R32UI);
    P(nvnTextureBuilderSetStorage)(&builder,storage,offset);
    require(P(nvnTextureInitialize)(texture,&builder),"public BUFFER texture initializes");
    P(nvnTexturePoolRegisterTexture)(&textures,TEXTURE_ID,texture,NULL);
}
static void texture_init(NVNtexture *texture, NVNmemoryPool *storage, unsigned offset) { texture_init_n(texture, storage, offset, COUNT); }
static void bind_reader(unsigned result_offset) {
    P(nvnCommandBufferSetTexturePool)(&command,&textures);
    P(nvnCommandBufferBindProgram)(&command,&program,NVN_SHADER_STAGE_COMPUTE_BIT);
    P(nvnCommandBufferBindTexture)(&command,NVN_SHADER_STAGE_COMPUTE,0,
                                 P(nvnDeviceGetTextureHandle)(&device,TEXTURE_ID,0));
    P(nvnCommandBufferBindStorageBuffer)(&command,NVN_SHADER_STAGE_COMPUTE,0,base+OUTPUT+result_offset,COUNT*4);
    P(nvnCommandBufferDispatchCompute)(&command,COUNT,1,1);
}
static void gated_submit(NVNcommandHandle handle) {
    id<MTLSharedEvent> gate=[g_dev newSharedEvent]; require(gate!=nil,"real GPU gate allocates");
    [g_q4 waitForEvent:gate value:1];
    NVNsync sync; P(nvnSyncInitialize)(&sync,&device);
    P(nvnQueueSubmitCommands)(&queue,1,&handle);
    P(nvnQueueFenceSync)(&queue,&sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    uint64_t deadline=now_ns()+5000000000ull; int encoded=0;
    do {
        pthread_mutex_lock(&nq_mu); sync_t *s=(sync_t *)&sync;
        encoded=s->fenced==3 && ((nq_receipt *)(uintptr_t)s->fence)->published;
        pthread_mutex_unlock(&nq_mu);
        if (!encoded) usleep(1000);
    } while (!encoded && now_ns()<deadline);
    // Release even on setup failure, before assertions or teardown can wait.
    int still_gated=gate.signaledValue==0; gate.signaledValue=1;
    require(encoded&&still_gated,"both producer/consumer pairs encode before the GPU gate opens");
    NVNsyncWaitResult result=P(nvnSyncWait)(&sync,5000000000ull);
    require(result==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || result==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,"public queue fence completes");
    CHECK(!mtl_last_commit_error(),"public queue has no Metal execution error");
    P(nvnSyncFinalize)(&sync);
}
static void offset_case(unsigned offset) {
    printf("public NVN BUFFER offset %u\n",offset);
    NVNtexture texture; texture_init(&texture,&pool,SOURCE+offset);
    memset(memory+SOURCE,0,0x1000); memset(memory+OUTPUT,0xcd,COUNT*8);
    P(nvnMemoryPoolFlushMappedRange)(&pool,SOURCE,0x1000);
    P(nvnMemoryPoolFlushMappedRange)(&pool,OUTPUT,COUNT*8);
    uint64_t copies=(g_tbs_copies + g_tb2_copy);
    P(nvnCommandBufferBeginRecording)(&command);
    for (int version=0;version<2;version++) {
        P(nvnCommandBufferCopyBufferToBuffer)(&command,base+(version?SECOND:FIRST),base+SOURCE+offset,COUNT*4,0);
        bind_reader(version*COUNT*4);
    }
    gated_submit(P(nvnCommandBufferEndRecording)(&command));
    for (int version=0;version<2;version++) {
        int valid=1;
        for (int i=0;i<COUNT;i++) valid &= ((uint32_t *)(memory+OUTPUT))[version*COUNT+i]==literal(version,i);
        CHECK(valid,version?"unchanged public handle sees the later producer":"earlier public consumer retains every original literal");
    }
    NSUInteger alignment=[g_dev minimumTextureBufferAlignmentForPixelFormat:MTLPixelFormatR32Uint];
    require(alignment!=0,"device reports typed-buffer alignment");
    if (!(offset%alignment)) CHECK((g_tbs_copies + g_tb2_copy)==copies,"aligned public BUFFER path performs no snapshot copies");
    P(nvnTextureFinalize)(&texture);
}
static void row_case(id<MTLLibrary> library) {
    NvProg *compiled = (NvProg *)serial_get(((prog_t *)&program)->serial);
    NvShader *saved = compiled->sh[5], *shader = [NvShader new]; shader->stage = 5;
    [fixture_shaders addObject:shader];
    shader->fn = [library newFunctionWithName:@"sample_rows"];
    require(shader->fn != nil, "row shader exists");
    shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len = shader->ae.encodedLength;
    shader->rf = saved->rf; shader->rf.b[0].kind = NVN_RB_TEX; shader->rf.b[0].dtt = MTLTextureType2D; shader->rf.b[0].f1d = 1;
    strcpy(shader->key, "feed00000000caff"); compiled->sh[5] = shader;
    uint64_t copies = g_tb2_copy;
    offset_case(4);
    CHECK(g_tb2_copy == copies, "misaligned rows stay live without snapshot copies");
    NvShader *edge = [NvShader new]; [fixture_shaders addObject:edge]; edge->stage = 5;
    edge->fn = [library newFunctionWithName:@"sample_rows_edge"];
    require(edge->fn != nil, "row-boundary shader exists");
    edge->ae = [edge->fn newArgumentEncoderWithBufferIndex:0]; edge->arg_len = edge->ae.encodedLength;
    edge->rf = shader->rf; strcpy(edge->key, "feed00000000caf0"); compiled->sh[5] = edge;
    NVNtexture texture; texture_init_n(&texture, &pool, 0x80004, 16384 + COUNT);
    P(nvnCommandBufferBeginRecording)(&command);
    for (int v = 0; v < 2; v++) {
        P(nvnCommandBufferCopyBufferToBuffer)(&command, base + (v ? SECOND : FIRST), base + 0x80004 + 4 * 16380, COUNT * 4, 0);
        bind_reader(v * COUNT * 4);
    }
    gated_submit(P(nvnCommandBufferEndRecording)(&command));
    for (int v = 0; v < 2; v++) {
        int good = 1; for (int i = 0; i < COUNT; i++) good &= ((uint32_t *)(memory + OUTPUT))[v * COUNT + i] == literal(v, i);
        CHECK(good, "live row addressing carries the offset across the 16384-texel boundary");
    }
    P(nvnTextureFinalize)(&texture);
    compiled->sh[5] = shader;
    NVNmemoryPool private_pool; NVNmemoryPoolBuilder pb = {0};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory + 0x78000, getpagesize());
    P(nvnMemoryPoolBuilderSetFlags)(&pb, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    require(P(nvnMemoryPoolInitialize)(&private_pool, &pb), "private public source pool initializes");
    NVNbufferAddress private_base = P(nvnMemoryPoolGetBufferAddress)(&private_pool);
    texture_init(&texture, &private_pool, 4);
    P(nvnCommandBufferBeginRecording)(&command);
    for (int v = 0; v < 2; v++) {
        P(nvnCommandBufferCopyBufferToBuffer)(&command, base + (v ? SECOND : FIRST), private_base + 4, COUNT * 4, 0);
        bind_reader(v * COUNT * 4);
    }
    gated_submit(P(nvnCommandBufferEndRecording)(&command));
    for (int v = 0; v < 2; v++) {
        int good = 1; for (int i = 0; i < COUNT; i++) good &= ((uint32_t *)(memory + OUTPUT))[v * COUNT + i] == literal(v, i);
        CHECK(good, "private row sources read live GPU bytes without querying CPU contents");
    }
    P(nvnTextureFinalize)(&texture); P(nvnMemoryPoolFinalize)(&private_pool);
    compiled->sh[5] = saved;
}
static void virtual_remap_case(void) {
    // Map one host-sized page of a larger virtual pool, so the first request
    // selects the actual remappable page-table path rather than whole-pool aliasing.
    size_t page=(size_t)getpagesize(); NVNmemoryPool virtual_pool; NVNmemoryPoolBuilder pb={0};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb,&device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb,NULL,2*page); P(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    require(P(nvnMemoryPoolInitialize)(&virtual_pool,&pb),"public virtual pool initializes");
    // nvn.h leaves NVNmappingRequest opaque; map_req is the production ABI layout.
    map_req request={.phys=&pool,.poff=FIRST,.voff=0,.size=page};
    require(P(nvnMemoryPoolMapVirtual)(&virtual_pool,1,(const NVNmappingRequest *)&request),"first public virtual mapping succeeds");
    NVNtexture texture; texture_init(&texture,&virtual_pool,4);
    uint64_t descriptor_serial=((const tex_t *)&texture)->serial;
    memset(memory+OUTPUT,0xcd,COUNT*8); P(nvnMemoryPoolFlushMappedRange)(&pool,OUTPUT,COUNT*8);
    for (int version=0;version<2;version++) {
        if (version) {
            request.poff=REMAP_SECOND;
            require(P(nvnMemoryPoolMapVirtual)(&virtual_pool,1,(const NVNmappingRequest *)&request),"active public virtual mapping is replaced");
        }
        P(nvnCommandBufferBeginRecording)(&command); bind_reader(version*COUNT*4);
        gated_submit(P(nvnCommandBufferEndRecording)(&command));
        CHECK(((const tex_t *)&texture)->serial==descriptor_serial,"remap leaves the same public texture descriptor and registration");
    }
    for (int version=0;version<2;version++) {
        int valid=1;
        // The logical source starts at offset 4 within each mapped page.
        for (int i=0;i<COUNT;i++) valid &= ((uint32_t *)(memory+OUTPUT))[version*COUNT+i]==literal(version,i+1);
        CHECK(valid,version?"same registered descriptor follows its active remap":"first mapped descriptor samples original physical storage");
    }
    P(nvnTextureFinalize)(&texture); P(nvnMemoryPoolFinalize)(&virtual_pool);
}
static void virtual_remap_new_descriptor_case(void) {
    // Map one host-sized page of a larger virtual pool, so the first request
    // selects the actual remappable page-table path rather than whole-pool aliasing.
    size_t page=(size_t)getpagesize(); NVNmemoryPool virtual_pool; NVNmemoryPoolBuilder pb={0};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb,&device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb,NULL,2*page); P(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    require(P(nvnMemoryPoolInitialize)(&virtual_pool,&pb),"public virtual pool initializes");
    // nvn.h leaves NVNmappingRequest opaque; map_req is the production ABI layout.
    map_req request={.phys=&pool,.poff=FIRST,.voff=0,.size=page};
    require(P(nvnMemoryPoolMapVirtual)(&virtual_pool,1,(const NVNmappingRequest *)&request),"first public virtual mapping succeeds");
    NVNtexture texture, replacement; texture_init(&texture,&virtual_pool,4);
    uint64_t descriptor_serial=((const tex_t *)&texture)->serial;
    memset(memory+OUTPUT,0xcd,COUNT*8); P(nvnMemoryPoolFlushMappedRange)(&pool,OUTPUT,COUNT*8);
    for (int version=0;version<2;version++) {
        if (version) {
            request.poff=REMAP_SECOND;
            require(P(nvnMemoryPoolMapVirtual)(&virtual_pool,1,(const NVNmappingRequest *)&request),"active public virtual mapping is replaced");
            texture_init(&replacement, &virtual_pool, 4);
        }
        P(nvnCommandBufferBeginRecording)(&command); bind_reader(version*COUNT*4);
        gated_submit(P(nvnCommandBufferEndRecording)(&command));
        CHECK(((const tex_t *)&texture)->serial==descriptor_serial,"remap leaves the same public texture descriptor and registration");
    }
    for (int version=0;version<2;version++) {
        int valid=1;
        // The logical source starts at offset 4 within each mapped page.
        for (int i=0;i<COUNT;i++) valid &= ((uint32_t *)(memory+OUTPUT))[version*COUNT+i]==literal(version,i+1);
        CHECK(valid,version?"same registered descriptor follows its active remap":"first mapped descriptor samples original physical storage");
    }
    P(nvnTextureFinalize)(&replacement); P(nvnTextureFinalize)(&texture); P(nvnMemoryPoolFinalize)(&virtual_pool);
}
// Independent coverage for tables that previously held a zero-copy BUFFER RID.
// Keep the direct-slot remap case above unchanged. The table and descriptor stay
// registered across the remap; inspect actual GPU literals, not host view fields.
static void table_remap_case(id<MTLLibrary> library, int handles, unsigned offset) {
    printf("public NVN %s table remap offset %u\n", handles ? "TEXH" : "TEXP", offset);
    size_t page=(size_t)getpagesize(); NVNmemoryPool virtual_pool; NVNmemoryPoolBuilder pb={0};
    P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb,&device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb,NULL,2*page); P(nvnMemoryPoolBuilderSetFlags)(&pb,NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    require(P(nvnMemoryPoolInitialize)(&virtual_pool,&pb),"table virtual pool initializes");
    map_req request={.phys=&pool,.poff=FIRST,.voff=0,.size=page};
    require(P(nvnMemoryPoolMapVirtual)(&virtual_pool,1,(const NVNmappingRequest *)&request),"table initial mapping succeeds");
    NVNtexture texture; texture_init(&texture,&virtual_pool,offset); uint64_t descriptor_serial=((const tex_t *)&texture)->serial;
    NVNprogram table_program; require(P(nvnProgramInitialize)(&table_program,&device),"table program initializes");
    NvShader *shader=[NvShader new]; shader->stage=5;
    shader->fn=[library newFunctionWithName:handles?@"sample_handles":@"sample_pool"];
    shader->ae=[shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len=shader->ae.encodedLength;
    require(shader->fn&&shader->ae,"table shader has real function and reflection");
    snprintf(shader->key,sizeof shader->key,"feed%012llx",(unsigned long long)nvn_new_serial());
    shader->rf.nb=2; shader->rf.tg[0]=shader->rf.tg[1]=shader->rf.tg[2]=1;
    shader->rf.pool=!handles; shader->rf.htab=handles; shader->rf.bindless=1;
    shader->rf.b[0]=(nvn_rbind){.kind=handles?NVN_RB_TEXH:NVN_RB_TEXP,.reg=1,.aid=0,.dtt=MTLTextureTypeTextureBuffer,.dpf=NVN_DPF_UINT};
    shader->rf.b[1]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.aid=1,.rw=1};
    NvProg *compiled=[NvProg new]; compiled->sh[5]=shader; prog_t *pr=(prog_t *)&table_program;
    pr->serial=nvn_new_serial(); serial_put(pr->serial,compiled);
    // Match production shader cache ownership while pointer-keyed PSOs exist.
    if(!g_shcache)g_shcache=[NSMutableDictionary dictionary]; g_shcache[@(shader->key)]=shader;
    const unsigned handle_offset=0x59000;
    *(uint64_t *)(memory+handle_offset)=P(nvnDeviceGetTextureHandle)(&device,TEXTURE_ID,0);
    memset(memory+OUTPUT,0xcd,COUNT*8);
    P(nvnMemoryPoolFlushMappedRange)(&pool,handle_offset,8); P(nvnMemoryPoolFlushMappedRange)(&pool,OUTPUT,COUNT*8);
    // Record once and replay exactly the same bindings after MapVirtual. No
    // argument-dirty/cache resets or rebinding can mask stale table reuse.
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetTexturePool)(&command,&textures);
    P(nvnCommandBufferBindProgram)(&command,&table_program,NVN_SHADER_STAGE_COMPUTE_BIT);
    P(nvnCommandBufferBindUniformBuffer)(&command,NVN_SHADER_STAGE_COMPUTE,1,base+handle_offset,8);
    P(nvnCommandBufferBindStorageBuffer)(&command,NVN_SHADER_STAGE_COMPUTE,0,base+OUTPUT,COUNT*4);
    P(nvnCommandBufferDispatchCompute)(&command,COUNT,1,1);
    NVNcommandHandle recording=P(nvnCommandBufferEndRecording)(&command);
    for(int version=0;version<2;version++) {
        if(version) { g_walk_ms = POOL_SUB_MS; request.poff=REMAP_SECOND; require(P(nvnMemoryPoolMapVirtual)(&virtual_pool,1,(const NVNmappingRequest *)&request),"table active mapping replacement succeeds"); }
        gated_submit(recording);
        CHECK(((const tex_t *)&texture)->serial==descriptor_serial,"table remap does not replace the registered descriptor");
        int good=1; for(int i=0;i<COUNT;i++) good &= ((uint32_t *)(memory+OUTPUT))[i]==literal(version,i+(int)(offset/4));
        CHECK(good,version?"unchanged BUFFER table follows new mapped storage":"initial BUFFER table reads original mapped storage");
    }
    P(nvnProgramFinalize)(&table_program); P(nvnTextureFinalize)(&texture); P(nvnMemoryPoolFinalize)(&virtual_pool);
}
static void deferred_render_case(id<MTLLibrary> library, int kind) {
    NVNtexture source; texture_init(&source,&pool,SOURCE+4);
    NVNtexture target; NVNtextureBuilder builder={0};
    P(nvnTextureBuilderSetDefaults)(&builder); P(nvnTextureBuilderSetDevice)(&builder,&device);
    P(nvnTextureBuilderSetTarget)(&builder,NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetSize2D)(&builder,8,4);
    P(nvnTextureBuilderSetFormat)(&builder,NVN_FORMAT_R32UI); P(nvnTextureBuilderSetStorage)(&builder,&pool,0x60000);
    require(P(nvnTextureInitialize)(&target,&builder),"public integer render target initializes");
    NVNprogram graphics; require(P(nvnProgramInitialize)(&graphics,&device),"public graphics program initializes");
    NvProg *compiled=[NvProg new]; NvShader *vs=[NvShader new], *fs=[NvShader new]; [fixture_shaders addObject:vs]; [fixture_shaders addObject:fs];
    vs->fn=[library newFunctionWithName:@"plain_vertex"]; fs->stage=1; fs->fn=[library newFunctionWithName:@"sample_fragment"];
    require(vs->fn&&fs->fn,"synthetic graphics functions exist");
    fs->ae=[fs->fn newArgumentEncoderWithBufferIndex:0]; require(fs->ae!=nil,"fragment argument encoder exists");
    fs->arg_len=fs->ae.encodedLength; fs->rf.nb=2; fs->rf.tbias_n=4; fs->rf.color_mask=1; fs->rf.color_known=1; fs->rf.fcols_known=1; fs->rf.ucols=1;
    fs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TBUF,.reg=0,.aid=0,.dtt=MTLTextureTypeTextureBuffer,.dpf=NVN_DPF_UINT,.tbx=1};
    fs->rf.b[0].kind = kind; fs->rf.b[0].n = kind == NVN_RB_TEXD ? 1 : 0;
    fs->rf.b[1]=(nvn_rbind){.kind=NVN_RB_TBIAS,.aid=2,.n=4};
    strcpy(vs->key,"feed00000000caf1"); strcpy(fs->key,"feed00000000caf2");
    compiled->sh[0]=vs; compiled->sh[1]=fs;
    prog_t *p=(prog_t *)&graphics; p->serial=nvn_new_serial(); serial_put(p->serial,compiled);
    memset(memory+SOURCE,0,0x1000); memset(memory+OUTPUT,0xcd,8*4*4);
    P(nvnMemoryPoolFlushMappedRange)(&pool,SOURCE,0x1000); P(nvnMemoryPoolFlushMappedRange)(&pool,OUTPUT,8*4*4);
    uint64_t deferred=g_ubd_deferred;
    P(nvnCommandBufferBeginRecording)(&command);
    const NVNtexture *colors[]={&target}; const float viewport[]={0,0,8,4}; const uint32_t zero[4]={0};
    P(nvnCommandBufferSetRenderTargets)(&command,1,colors,NULL,NULL,NULL);
    P(nvnCommandBufferSetViewports)(&command,0,1,viewport); P(nvnCommandBufferSetScissor)(&command,0,0,8,4);
    P(nvnCommandBufferClearColorui)(&command,0,zero,15);
    P(nvnCommandBufferSetTexturePool)(&command,&textures); P(nvnCommandBufferBindProgram)(&command,&graphics,3);
    if (kind == NVN_RB_IMG) P(nvnCommandBufferBindImage)(&command,NVN_SHADER_STAGE_FRAGMENT,0,P(nvnDeviceGetImageHandle)(&device,TEXTURE_ID));
    else P(nvnCommandBufferBindTexture)(&command,NVN_SHADER_STAGE_FRAGMENT,0,P(nvnDeviceGetTextureHandle)(&device,TEXTURE_ID,0));
    for (int version=0;version<2;version++) {
        P(nvnCommandBufferUpdateUniformBuffer)(&command,base+SOURCE+4,COUNT*4,0,COUNT*4,memory+(version?SECOND:FIRST));
        P(nvnCommandBufferSetScissor)(&command,version*4,0,4,4);
        P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    }
    const NVNcopyRegion region={0,0,0,8,4,1};
    P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,base+OUTPUT,0);
    gated_submit(P(nvnCommandBufferEndRecording)(&command));
    CHECK(g_ubd_deferred>deferred,"a real in-pass update was deferred before the later texture consumer");
    int valid=1;
    for (int y=0;y<4;y++) for (int x=0;x<8;x++) valid &= ((uint32_t *)(memory+OUTPUT))[y*8+x]==literal(x>=4,0);
    CHECK(valid,"public fragment consumers preserve A before and B after the deferred update");
    P(nvnProgramFinalize)(&graphics); P(nvnTextureFinalize)(&source); P(nvnTextureFinalize)(&target);
}
int main(void) { @autoreleasepool {
    alarm(90);
    require(!setvbuf(stdout,NULL,_IONBF,0),"test output is unbuffered");
    crash_install(); // Pool pages use production write protection and its fault handler.
    setenv("DM_PSO_ARCHIVE","off",1); setenv("DM_PSO_PREBUILD","0",1);
    setenv("DM_PSO_USER","/dev/null",1); setenv("DM_PSO_RECORD","/dev/null",1);
    mtl_init(); fixture_shaders = [NSMutableArray array]; NSError *error=nil;
    id<MTLLibrary> library=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct B{texture_buffer<uint> source[[id(0)]];device uint*output[[id(1)]];constant uint4*bias[[id(2)]];};\n"
        "kernel void sample_buffer(constant B&a[[buffer(0)]],uint x[[thread_position_in_grid]]){a.output[x]=a.source.read(x+a.bias[0].x).x;}\n"
        "struct R{texture2d<uint> source[[id(0)]];device uint*output[[id(1)]];constant uint4*bias[[id(2)]];};\n"
        "kernel void sample_rows(constant R&a[[buffer(0)]],uint x[[thread_position_in_grid]]){uint c=x+a.bias[0].x;a.output[x]=a.source.read(uint2(c&16383,c>>14)).x;}\n"
        "kernel void sample_rows_edge(constant R&a[[buffer(0)]],uint x[[thread_position_in_grid]]){uint c=x+16380+a.bias[0].x;a.output[x]=a.source.read(uint2(c&16383,c>>14)).x;}\n"
        "struct T{uint n;uint bias;ulong seen;array<texture_buffer<uint>,257>e;};struct H{constant T*t[[id(0)]];device uint*output[[id(1)]];};\n"
        "kernel void sample_pool(constant H&a[[buffer(0)]],uint x[[thread_position_in_grid]]){a.output[x]=a.t->e[256].read(x+reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(a.t)+a.t->bias)[256]).x;}\n"
        "kernel void sample_handles(constant H&a[[buffer(0)]],uint x[[thread_position_in_grid]]){a.output[x]=a.t->e[0].read(x+reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(a.t)+a.t->bias)[0]).x;}\n"
        "vertex float4 plain_vertex(uint i[[vertex_id]]){return float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1);}\n"
        "fragment uint sample_fragment(constant B&a[[buffer(0)]]){return a.source.read(a.bias[0].x).x;}\n"
        options:nil error:&error];
    require(library!=nil,"synthetic MSL compiles");
    NVNdeviceBuilder db={0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device,&db),"public device initializes");
    NVNqueueBuilder qb={0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb,&device);
    require(P(nvnQueueInitialize)(&queue,&qb),"public queue initializes");
    require(!posix_memalign((void **)&memory,getpagesize(),POOL_BYTES),"page-aligned game storage allocates"); memset(memory,0,POOL_BYTES);
    NVNmemoryPoolBuilder pb={0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb,&device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb,memory,POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb,0x24);
    require(P(nvnMemoryPoolInitialize)(&pool,&pb),"public memory pool initializes"); base=P(nvnMemoryPoolGetBufferAddress)(&pool);
    require(base!=0,"public GPU address exists");
    require(P(nvnTexturePoolInitialize)(&textures,&pool,DESCRIPTORS,TEXTURE_ID+1),"public texture descriptor pool initializes");
    require(P(nvnCommandBufferInitialize)(&command,&device),"public command buffer initializes"); P(nvnCommandBufferAddCommandMemory)(&command,&pool,0,COMMAND_BYTES);
    for (int v=0;v<2;v++) for (int i=0;i<COUNT;i++) ((uint32_t *)(memory+(v?SECOND:FIRST)))[i]=literal(v,i);
    for (int i=0;i<=COUNT;i++) {
        ((uint32_t *)(memory+FIRST))[i]=literal(0,i);
        ((uint32_t *)(memory+REMAP_SECOND))[i]=literal(1,i);
    }
    P(nvnMemoryPoolFlushMappedRange)(&pool,FIRST,0x8000);
    require(P(nvnProgramInitialize)(&program,&device),"public program object initializes");
    NvShader *shader=[NvShader new]; shader->stage=5; shader->fn=[library newFunctionWithName:@"sample_buffer"];
    shader->ae=[shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len=shader->ae.encodedLength;
    require(shader->fn&&shader->ae,"real shader reflection encoder exists"); strcpy(shader->key,"feed00000000cafe");
    shader->rf.nb=3; shader->rf.tbias_n=4; shader->rf.tg[0]=shader->rf.tg[1]=shader->rf.tg[2]=1;
    shader->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TBUF,.reg=0,.aid=0,.dtt=MTLTextureTypeTextureBuffer,.dpf=NVN_DPF_UINT,.tbx=1};
    shader->rf.b[2]=(nvn_rbind){.kind=NVN_RB_TBIAS,.aid=2,.n=4};
    shader->rf.b[1]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.aid=1,.rw=1};
    NvProg *compiled=[NvProg new]; compiled->sh[5]=shader;
    prog_t *p=(prog_t *)&program; p->serial=nvn_new_serial(); serial_put(p->serial,compiled);
    const unsigned offsets[]={4,8,12,16};
    for (unsigned i=0;i<sizeof offsets/sizeof *offsets;i++) offset_case(offsets[i]);
    row_case(library);
    virtual_remap_case();
    virtual_remap_new_descriptor_case();
    deferred_render_case(library, NVN_RB_TBUF);
    NVNtexturePool first_pool = textures;
    require(P(nvnTexturePoolInitialize)(&textures, &pool, 0x70000, TEXTURE_ID + 1), "second public texture pool initializes");
    deferred_render_case(library, NVN_RB_IMG);
    deferred_render_case(library, NVN_RB_TEXD);
    P(nvnTexturePoolFinalize)(&textures); textures = first_pool;
    for(int handles=0;handles<2;handles++) { table_remap_case(library,handles,0); table_remap_case(library,handles,4); }
    P(nvnQueueFinish)(&queue); P(nvnProgramFinalize)(&program); P(nvnCommandBufferFinalize)(&command);
    P(nvnTexturePoolFinalize)(&textures); P(nvnMemoryPoolFinalize)(&pool); P(nvnQueueFinalize)(&queue); P(nvnDeviceFinalize)(&device);
    free(memory); printf("public NVN live buffers: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
