// Later explicit writes replace pending query values at conditional rendering.
// Public NVN commands exercise the predicate; a bounded Metal gate holds reports
// unfinished. OPEN diagnostics separately expose pre-existing CPU-post query
// byte ordering; passing this test does not claim native GPU report bytes work.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#define P(name) ((NVNT_##name)nvn_lookup(#name))
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failures;
#define CHECK(c, name) do { int check_ok = !!(c); checks++; failures += !check_ok; printf("%s: %s\n", check_ok ? "PASS" : "FAIL", name); } while (0)
static void require(int ok, const char *name) { CHECK(ok, name); if (!ok) exit(2); }
enum { W = 8, H = 8, BYTES = W * H * 4, POOL_BYTES = 1 << 20, TEX_A = 0x80000, TEX_B = 0x90000, OUTPUT = 0xa0000, EVENT = 0xa1000 };
static NVNdevice device; static NVNqueue queues[2]; static NVNmemoryPool pool; static NVNcommandBuffer command;
static NVNevent event; static NVNtexture target, replacement; static uint8_t *memory; static NVNbufferAddress output;
static const NVNcopyRegion region = {0, 0, 0, W, H, 1};

static int landed(NVNsync *sync, uint64_t timeout) {
    NVNsyncWaitResult r = P(nvnSyncWait)(sync, timeout);
    return r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
}
static void submit(int queue, NVNcommandHandle handle, NVNsync *sync) {
    P(nvnSyncInitialize)(sync, &device); P(nvnQueueSubmitCommands)(&queues[queue], 1, &handle);
    P(nvnQueueFenceSync)(&queues[queue], sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
}
static void finish(NVNsync *sync) {
    require(landed(sync, 5000000000ull), "queue tail and GPU work complete");
    P(nvnSyncFinalize)(sync);
}
static int pixels(unsigned offset, unsigned r, unsigned g, unsigned b) {
    const uint8_t want[] = {r, g, b, 255};
    for (int i = 0; i < W * H; i++) if (memcmp(memory + OUTPUT + offset + i * 4, want, 4)) return 0;
    return 1;
}
static void init_texture(NVNtexture *texture, unsigned offset) {
    NVNtextureBuilder tb = {0}; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
    P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetSize2D)(&tb, W, H);
    P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetStorage)(&tb, &pool, offset);
    require(P(nvnTextureInitialize)(texture, &tb), "texture initialized on retained backing storage");
}
static void init_program(NVNprogram *program, NvProg *compiled) {
    require(P(nvnProgramInitialize)(program, &device), "program object initialized");
    // Synthetic MSL replaces only game shader loading; recording and submission
    // use the public NVN entry points and the real serial ownership table.
    prog_t *p = (prog_t *)program; p->serial = nvn_new_serial(); serial_put(p->serial, compiled);
}
static NVNcommandHandle record(NVNtexture *texture, NVNprogram *program, int wait, int draw, unsigned offset) {
    P(nvnCommandBufferBeginRecording)(&command);
    if (wait) P(nvnCommandBufferWaitEvent)(&command, &event, NVN_EVENT_WAIT_MODE_EQUAL, 1);
    const NVNtexture *colors[] = {texture}; const float viewport[] = {0, 0, W, H}, blue[] = {0, 0, 1, 1};
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, NULL, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    if (program) P(nvnCommandBufferBindProgram)(&command, program, 3);
    if (draw) P(nvnCommandBufferDrawArrays)(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, texture, NULL, &region, output + offset, 0);
    return P(nvnCommandBufferEndRecording)(&command);
}
int main(void) { @autoreleasepool {
    if (setvbuf(stdout, NULL, _IONBF, 0)) abort(); mtl_init(); NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { const float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.5,1); }\n"
        "fragment float4 red() { return float4(1,0,0,1); } fragment float4 green() { return float4(0,1,0,1); }\n"
        options:nil error:&error];
    if (!library) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    require(library != nil, "synthetic shaders compile");
    NvShader *vs = [NvShader new]; vs->fn = [library newFunctionWithName:@"v"];
    NvProg *red = [NvProg new], *green = [NvProg new]; red->sh[0] = green->sh[0] = vs;
    for (int i = 0; i < 2; i++) {
        NvShader *fs = [NvShader new]; fs->stage = 1; fs->rf.color_mask = 1; fs->fn = [library newFunctionWithName:i ? @"green" : @"red"];
        (i ? green : red)->sh[1] = fs;
    }
    NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    for (int i = 0; i < 2; i++) { NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device); require(P(nvnQueueInitialize)(&queues[i], &qb), "queue initialized"); }
    require(!posix_memalign((void **)&memory, getpagesize(), POOL_BYTES), "pool storage allocated"); memset(memory, 0, POOL_BYTES);
    NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24); require(P(nvnMemoryPoolInitialize)(&pool, &pb), "pool initialized");
    NVNbufferBuilder bb = {0}; NVNbuffer buffer; P(nvnBufferBuilderSetDefaults)(&bb); P(nvnBufferBuilderSetDevice)(&bb, &device);
    P(nvnBufferBuilderSetStorage)(&bb, &pool, OUTPUT, BYTES * 2); require(P(nvnBufferInitialize)(&buffer, &bb), "readback buffer initialized"); output = P(nvnBufferGetAddress)(&buffer);
    NVNeventBuilder eb = {0}; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb, &pool, EVENT); require(P(nvnEventInitialize)(&event, &eb), "event initialized");
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized"); P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, TEX_A);
    init_texture(&target, TEX_A); init_texture(&replacement, TEX_B);
    NVNprogram stable_red, stable_green; init_program(&stable_red, red); init_program(&stable_green, green);


    NVNsync sync, split;P(nvnSyncInitialize)(&split,&device);
    const NVNtexture *colors[]={&target};const float blue[]={0,0,1,1},red_clear[]={1,0,0,1};
    const char *names[]={"UpdateUB1", "UpdateUB1 then fence", "ClearBuffer1", "CopyBuffer1", "CopyBuffer0", "UpdateUB high32"};
    for(int k=0;k<6;k++) {
        id<MTLSharedEvent> gate=[g_dev newSharedEvent];
        if (!gate) abort();
        dispatch_semaphore_t cancel=dispatch_semaphore_create(0), retired=dispatch_semaphore_create(0);
        [g_q4 waitForEvent:gate value:1];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
            if(dispatch_semaphore_wait(cancel,dispatch_time(DISPATCH_TIME_NOW,2000000000ll)))gate.signaledValue=1;
            dispatch_semaphore_signal(retired);
        });
        uint64_t *counts=(uint64_t *)(memory+EVENT+64),*source=(uint64_t *)(memory+EVENT+0x4000),*gpu_value=(uint64_t *)(memory+EVENT+256);
        counts[0]=1;counts[2]=0;*source=k==4?0:1;*gpu_value=0xdeadbeef;
        uint64_t one=1,want=k==2?0x100000001ull:k==4?0:k==5?0x100000000ull:1;
        NVNbufferAddress addr=(uintptr_t)counts;
        P(nvnCommandBufferBeginRecording)(&command);
        P(nvnCommandBufferSetRenderEnable)(&command,1);
        P(nvnCommandBufferSetRenderTargets)(&command,1,colors,NULL,NULL,NULL);
        P(nvnCommandBufferClearColor)(&command,0,blue,15);
        P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_TIMESTAMP,addr);
        P(nvnCommandBufferFenceSync)(&command,&split,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        if(k==0 || k==1)P(nvnCommandBufferUpdateUniformBuffer)(&command,addr,16,0,8,&one);
        if(k==2)P(nvnCommandBufferClearBuffer)(&command,addr,8,1);
        if(k==3 || k==4)P(nvnCommandBufferCopyBufferToBuffer)(&command,(uintptr_t)source,addr,8,0);
        if(k==5)P(nvnCommandBufferUpdateUniformBuffer)(&command,addr,16,4,4,&one);
        if(k==1)P(nvnCommandBufferFenceSync)(&command,&split,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        P(nvnCommandBufferSetRenderEnableConditional)(&command,(NVNconditionalRenderMode)1,addr);
        P(nvnCommandBufferClearColor)(&command,0,red_clear,15);
        P(nvnCommandBufferSetRenderEnable)(&command,1);
        P(nvnCommandBufferCopyBufferToBuffer)(&command,addr,(uintptr_t)gpu_value,8,0);
        P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
        submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);
        uint64_t deadline=now_ns()+1000000000ull;int encoded=0;
        do {
            pthread_mutex_lock(&nq_mu);sync_t *st=(sync_t *)&sync;
            encoded=st->fenced==3 && ((nq_receipt *)(uintptr_t)st->fence)->published;
            pthread_mutex_unlock(&nq_mu);if(!encoded)usleep(1000);
        }while(!encoded && now_ns()<deadline);
        CHECK(encoded,"overlap commands encode while GPU completion gated");
        gate.signaledValue=1;dispatch_semaphore_signal(cancel);
        require(!dispatch_semaphore_wait(retired,dispatch_time(DISPATCH_TIME_NOW,3000000000ll)),"GPU gate rescue retired");
        finish(&sync);
        int good=pixels(0,want?255:0,0,want?0:255);
        CHECK(good,names[k]);if(k!=5) CHECK(*gpu_value==want,"GPU command-ordered bytes match latest full write");
        else printf("OPEN partial-update native GPU_A expected=%#llx actual=%#llx: timestamp report is currently CPU-post only\n",(unsigned long long)want,(unsigned long long)*gpu_value);
        printf("RESULT %s predicate=%d GPU_A=%#llx final_CPU_A=%#llx expected=%#llx\n",names[k],good,(unsigned long long)*gpu_value,(unsigned long long)counts[0],(unsigned long long)want);
    }
    P(nvnSyncFinalize)(&split);
    P(nvnQueueFinish)(&queues[0]); P(nvnQueueFinish)(&queues[1]);
    P(nvnProgramFinalize)(&stable_red); P(nvnProgramFinalize)(&stable_green);
    P(nvnTextureFinalize)(&target); P(nvnTextureFinalize)(&replacement); P(nvnCommandBufferFinalize)(&command);
    P(nvnBufferFinalize)(&buffer); P(nvnEventFinalize)(&event); P(nvnMemoryPoolFinalize)(&pool);
    P(nvnQueueFinalize)(&queues[0]); P(nvnQueueFinalize)(&queues[1]); P(nvnDeviceFinalize)(&device); free(memory);
    printf("counter overlap: %d checks, %d failed\n", checks, failures); return failures != 0;
} }
