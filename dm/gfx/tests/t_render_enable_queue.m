// Logical render-enable state and pending query values survive scheduler chunks.
// Every path uses public NVN recording; the last case gates only GPU completion.
// It checks the profile key render_cond's replay, so the key is on whatever profile GAME names:
// without it the host draws every render-enable block and posts no pending counter values.
#include "game_profile.h"
#undef DM_GAME_RENDER_COND
#define DM_GAME_RENDER_COND 1
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

    NVNsync sync, dependency;
    P(nvnSyncInitialize)(&dependency, &device);
    P(nvnQueueFenceSync)(&queues[1], &dependency, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    const NVNtexture *colors[] = {&target}; const float viewport[] = {0,0,W,H}, blue[] = {0,0,1,1};
    const char *names[] = {"unsplit disabled control", "disabled across WaitEvent", "disabled across SignalEvent", "disabled across WaitSync", "disabled across FenceSync", "conditional false across WaitEvent"};
    NVNsync split_sync; P(nvnSyncInitialize)(&split_sync, &device);
    for (int k=0;k<6;k++) {
        P(nvnCommandBufferBeginRecording)(&command);
        P(nvnCommandBufferSetRenderEnable)(&command,1);
        P(nvnCommandBufferSetRenderTargets)(&command,1,colors,NULL,NULL,NULL);
        P(nvnCommandBufferSetViewports)(&command,0,1,viewport); P(nvnCommandBufferSetScissor)(&command,0,0,W,H);
        P(nvnCommandBufferClearColor)(&command,0,blue,15);
        P(nvnCommandBufferBindProgram)(&command,&stable_red,3);
        if(k==5) {
            uint64_t *counts=(uint64_t *)(memory+EVENT+64); counts[0]=0;counts[2]=1;
            P(nvnCommandBufferSetRenderEnableConditional)(&command,(NVNconditionalRenderMode)0,(NVNbufferAddress)(uintptr_t)counts);
        } else P(nvnCommandBufferSetRenderEnable)(&command,0);
        if(k==1 || k==5) P(nvnCommandBufferWaitEvent)(&command,&event,NVN_EVENT_WAIT_MODE_EQUAL,1);
        if(k==2) P(nvnCommandBufferSignalEvent)(&command,&event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,1);
        if(k==3) P(nvnCommandBufferWaitSync)(&command,&dependency);
        if(k==4) P(nvnCommandBufferFenceSync)(&command,&split_sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        P(nvnCommandBufferSetRenderEnable)(&command,1);
        P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
        submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
        CHECK(pixels(0,0,0,255),names[k]);
    }
    // No object or backing reuse: only the queue-owned render-enable state crosses jobs.
    submit(0,record(&target,NULL,0,0,0),&sync);finish(&sync);
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetRenderEnable)(&command,0);
    submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
    submit(1,record(&replacement,&stable_green,0,1,BYTES),&sync);finish(&sync);
    CHECK(pixels(BYTES,0,255,0),"independent queue draws with its default enabled state");
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferBindProgram)(&command,&stable_red,3);
    P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
    P(nvnCommandBufferSetRenderEnable)(&command,1);
    submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
    CHECK(pixels(0,0,0,255),"original queue retains disabled state after another queue runs");

    // Hold GPU completion while the real NVN recorder/replayer crosses a fence.
    // The render predicate must forward the pending timestamp's known counter0.
    id<MTLSharedEvent> gpu_gate=[g_dev newSharedEvent];
    dispatch_semaphore_t gate_cancel=dispatch_semaphore_create(0), gate_retired=dispatch_semaphore_create(0);
    [g_q4 waitForEvent:gpu_gate value:1];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        if(dispatch_semaphore_wait(gate_cancel,dispatch_time(DISPATCH_TIME_NOW,2000000000ll))) gpu_gate.signaledValue=1;
        dispatch_semaphore_signal(gate_retired);
    });
    uint64_t *counts=(uint64_t *)(memory+EVENT+64);counts[0]=1;counts[2]=0;
    const float red_clear[]={1,0,0,1};
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetRenderEnable)(&command,1);
    P(nvnCommandBufferClearColor)(&command,0,blue,15);
    P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_TIMESTAMP,(NVNbufferAddress)(uintptr_t)counts);
    P(nvnCommandBufferFenceSync)(&command,&split_sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    P(nvnCommandBufferSetRenderEnableConditional)(&command,(NVNconditionalRenderMode)1,(NVNbufferAddress)(uintptr_t)counts);
    P(nvnCommandBufferClearColor)(&command,0,red_clear,15);
    P(nvnCommandBufferSetRenderEnable)(&command,1);
    P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
    submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);
    uint64_t deadline=now_ns()+1000000000ull;int encoded=0;
    do {
        pthread_mutex_lock(&nq_mu);
        sync_t *st=(sync_t *)&sync;encoded=st->fenced==3 && ((nq_receipt *)(uintptr_t)st->fence)->published;
        pthread_mutex_unlock(&nq_mu);
        if(!encoded) usleep(1000);
    } while(!encoded && now_ns()<deadline);
    CHECK(encoded,"both physical chunks encode while GPU completion is gated");
    CHECK(counts[0]==1,"timestamp counter is not CPU-visible before GPU release");
    // Release before any fatal assertion so even a broken implementation drains.
    gpu_gate.signaledValue=1;dispatch_semaphore_signal(gate_cancel);
    require(!dispatch_semaphore_wait(gate_retired,dispatch_time(DISPATCH_TIME_NOW,3000000000ll)),"GPU gate rescue retired");
    finish(&sync);
    CHECK(pixels(0,0,0,255),"pending timestamp zero survives FenceSync chunk split");
    counts[0]=1;counts[2]=0;
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferClearColor)(&command,0,blue,15);
    P(nvnCommandBufferSetRenderEnableConditional)(&command,(NVNconditionalRenderMode)1,(NVNbufferAddress)(uintptr_t)counts);
    P(nvnCommandBufferClearColor)(&command,0,red_clear,15);
    P(nvnCommandBufferSetRenderEnable)(&command,1);
    P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
    submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
    CHECK(pixels(0,255,0,0),"CPU counter write after completed report replaces pending zero");

    // The samples-passed counter is one running total in queue order. A game that never resets it reads
    // end - begin of two reports (Red Dead Redemption); this is the process's first samples-passed command.
    // Slots left at a submission's end are read at its own completion, before a later submission reuses
    // the visibility buffer (the half-scissored second draw lands in the same slot of the same buffer).
    uint64_t *r0=(uint64_t *)(memory+EVENT+384),*r1=(uint64_t *)(memory+EVENT+448),*r2=(uint64_t *)(memory+EVENT+512);
    r0[0]=r1[0]=r2[0]=7;
    for (int k=0;k<4;k++) {
        P(nvnCommandBufferBeginRecording)(&command);
        P(nvnCommandBufferSetRenderTargets)(&command,1,colors,NULL,NULL,NULL);
        P(nvnCommandBufferSetViewports)(&command,0,1,viewport); P(nvnCommandBufferSetScissor)(&command,0,0,k&1 ? W/2 : W,H);
        if (k==0) P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED,(NVNbufferAddress)(uintptr_t)r0);   // no reset before it
        if (k==2) P(nvnCommandBufferResetCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED);
        P(nvnCommandBufferBindProgram)(&command,&stable_red,3);
        P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        if (k&1) P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED,(NVNbufferAddress)(uintptr_t)(k==1 ? r1 : r2));
        submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
    }
    CHECK(r0[0]!=7 && r1[0]-r0[0]==W*H+W*H/2,"with no reset, end - begin of two reports counts the samples between them, across submissions");
    CHECK(r2[0]==W*H+W*H/2,"after a reset, the total counts the samples of every submission up to the report");

    // Occlusion conditions: a samples-passed report that has landed decides the
    // block as the GPU would; one still in flight keeps the block drawing.
    uint64_t *empty=(uint64_t *)(memory+EVENT+256),*seen=(uint64_t *)(memory+EVENT+320);
    empty[0]=seen[0]=7;empty[2]=seen[2]=0;   // +16 stays 0: RENDER_IF_NOT_EQUAL draws when samples passed
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetRenderTargets)(&command,1,colors,NULL,NULL,NULL);
    P(nvnCommandBufferSetViewports)(&command,0,1,viewport); P(nvnCommandBufferSetScissor)(&command,0,0,W,H);
    P(nvnCommandBufferResetCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED);
    P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED,(NVNbufferAddress)(uintptr_t)empty);   // no draw counted
    P(nvnCommandBufferResetCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED);
    P(nvnCommandBufferBindProgram)(&command,&stable_red,3);
    P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);   // covers the target
    P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED,(NVNbufferAddress)(uintptr_t)seen);
    submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
    CHECK(empty[0]==0 && seen[0]==W*H,"samples-passed reports land with 0 and the drawn samples");
    const char *occ[]={"landed zero query culls its block","landed nonzero query draws its block"};
    for (int k=0;k<2;k++) {
        P(nvnCommandBufferBeginRecording)(&command);
        P(nvnCommandBufferClearColor)(&command,0,blue,15);
        P(nvnCommandBufferBindProgram)(&command,&stable_green,3);
        P(nvnCommandBufferSetRenderEnableConditional)(&command,(NVNconditionalRenderMode)1,(NVNbufferAddress)(uintptr_t)(k ? seen : empty));
        P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
        P(nvnCommandBufferSetRenderEnable)(&command,1);
        P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
        submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
        CHECK(k ? pixels(0,0,255,0) : pixels(0,0,0,255),occ[k]);
    }
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferClearColor)(&command,0,blue,15);
    P(nvnCommandBufferResetCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED);
    P(nvnCommandBufferReportCounter)(&command,NVN_COUNTER_TYPE_SAMPLES_PASSED,(NVNbufferAddress)(uintptr_t)empty);
    P(nvnCommandBufferBindProgram)(&command,&stable_green,3);
    P(nvnCommandBufferSetRenderEnableConditional)(&command,(NVNconditionalRenderMode)1,(NVNbufferAddress)(uintptr_t)empty);
    P(nvnCommandBufferDrawArrays)(&command,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
    P(nvnCommandBufferSetRenderEnable)(&command,1);
    P(nvnCommandBufferCopyTextureToBuffer)(&command,&target,NULL,&region,output,0);
    submit(0,P(nvnCommandBufferEndRecording)(&command),&sync);finish(&sync);
    CHECK(pixels(0,0,255,0),"query in flight keeps its block drawing");
    P(nvnSyncFinalize)(&dependency);P(nvnSyncFinalize)(&split_sync);
    P(nvnQueueFinish)(&queues[0]); P(nvnQueueFinish)(&queues[1]);
    P(nvnProgramFinalize)(&stable_red); P(nvnProgramFinalize)(&stable_green);
    P(nvnTextureFinalize)(&target); P(nvnTextureFinalize)(&replacement); P(nvnCommandBufferFinalize)(&command);
    P(nvnBufferFinalize)(&buffer); P(nvnEventFinalize)(&event); P(nvnMemoryPoolFinalize)(&pool);
    P(nvnQueueFinalize)(&queues[0]); P(nvnQueueFinalize)(&queues[1]); P(nvnDeviceFinalize)(&device); free(memory);
    printf("render enable continuation: %d checks, %d failed\n", checks, failures); return failures != 0;
} }
