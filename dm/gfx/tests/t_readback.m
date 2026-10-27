// NVN readback contract: a pending clear must reach both GPU buffer copies and CPU reads.
// Expected 8x4 RGBA values are literal bytes, independent of the Metal replay's state.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int cases, failed;
#define EXPECT(n,w,v) do { int a=(w), b=(v); cases++; printf("%s expected %d -> got %d %s\n",n,a,b,a==b?"PASS":"FAIL"); failed+=a!=b; } while (0)

// Observe the real readback buffer without retaining it. The caller's outer pool stays open,
// as on a long-lived guest thread; ReadTexels must drain its own temporary Metal objects.
@interface ReadbackDevice : NSObject
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, weak) id<MTLBuffer> lastBuffer;
@property(nonatomic) NSUInteger allocations;
@end
@implementation ReadbackDevice
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    id<MTLBuffer> buffer = [_device newBufferWithLength:length options:options];
    _lastBuffer = buffer; _allocations++; return buffer;
}
- (id)forwardingTargetForSelector:(SEL)selector { (void)selector; return _device; }
@end

int main(void) { @autoreleasepool {
    mtl_init();
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 524288)) abort(); memset(mem, 0, 524288);
    // Registered with the dirty tracker as nvnMemoryPoolInitialize does: a packaged texture in an untracked pool is re-uploaded from its storage at every use, which would discard what the GPU wrote into it
    pool_t pool = {.mem=mem, .size=524288}; mtl_pool_add(mem, 524288); nvn_dirty_add_range(mem, 524288);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 131072); EXPECT("texture init", 1, h_nvnTextureInitialize(&tex, &tb));
    NVNcommandBuffer cmd; EXPECT("command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    const NVNtexture *targets[] = {&tex}; const NVNtextureView *views[] = {NULL};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets, views, NULL, NULL);
    float color[] = {0.2f, 0.4f, 0.6f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color, 15);
    NVNcopyRegion region = {0, 0, 0, 8, 4, 1}; uint8_t *out = (uint8_t *)mem + 196608; memset(out, 0xEE, 256);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 64);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (NVNbufferAddress)out, 0);
    // A copy from the cleared (GPU-written) texture into a packaged texture, whose storage is where the host takes its content from: the copy's bytes are read back into the storage
    // when the submission completes (mirror_readback), so ReadTexels, which reads a packaged texture's storage, sees the clear colour
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtexture pk; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840); EXPECT("packaged texture init", 1, h_nvnTextureInitialize(&pk, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &tex, NULL, &region, &pk, NULL, &region, 0);
    // A copy from that packaged texture while its readback is pending: its storage does not hold the content yet, so this copy is read back as well, landing after the first
    NVNtexture pk2; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 262144); EXPECT("packaged texture 2 init", 1, h_nvnTextureInitialize(&pk2, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk, NULL, &region, &pk2, NULL, &region, 0);
    // A buffer a copy queued in this submission writes (out → src2 on the GPU) holds stale bytes on the CPU: the copy from it into a packaged texture is read back
    uint8_t *src2 = (uint8_t *)mem + 200704; memset(src2, 0xEE, 256);
    h_nvnCommandBufferCopyBufferToBuffer(&cmd, (NVNbufferAddress)out, (NVNbufferAddress)src2, 256, 0);
    NVNtexture pk4; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 294912); EXPECT("packaged texture 4 init", 1, h_nvnTextureInitialize(&pk4, &tb));
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src2, &pk4, NULL, &region, 0);
    // A format-converting copy (RGBA8 → RGBA8 sRGB, the same bytes per texel) from a packaged texture the CPU wrote is drawn on the GPU: the storage takes the converted bytes by readback,
    // not the source's raw bytes. Every texel of the source is the same, whatever its block-linear order
    static const uint8_t expected[] = {51, 102, 153, 255};
    NVNtexture pk7; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 425984); EXPECT("packaged texture 7 init", 1, h_nvnTextureInitialize(&pk7, &tb));
    for (int i = 0; i < 1024; i++) memcpy((uint8_t *)mem + 425984 + i * 4, expected, 4);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    NVNtexture pk3; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 327680); EXPECT("packaged sRGB texture init", 1, h_nvnTextureInitialize(&pk3, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk7, NULL, &region, &pk3, NULL, &region, 0);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    // A packaged texture the GPU cleared (the clear gives its host texture render-target use) has stale storage: a copy from it is read back, not mirrored from the storage
    NVNtexture pk5, pk6; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 360448); EXPECT("packaged texture 5 init", 1, h_nvnTextureInitialize(&pk5, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 393216); EXPECT("packaged texture 6 init", 1, h_nvnTextureInitialize(&pk6, &tb));
    float color2[] = {0.8f, 0.6f, 0.4f, 1.f}; h_nvnCommandBufferClearTexture(&cmd, &pk5, NULL, &region, color2, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk5, NULL, &region, &pk6, NULL, &region, 0);
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)h); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("recorded fence wait", 1, wait == 0 || wait == 1);
    EXPECT("GPU success", 0, mtl_last_commit_error());
    int bad = 0;
    for (int y=0; y<4; y++) { for (int x=0; x<8; x++) bad += memcmp(out+y*64+x*4, expected, 4) != 0;
        for (int x=32; x<64; x++) bad += out[y*64+x] != 0xEE; }
    EXPECT("pending clear -> GPU copy, literal pixels and untouched row padding", 0, bad);
    ReadbackDevice *device = [ReadbackDevice new]; device.device = g_dev; g_dev = (id)device;
    uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&tex, NULL, &region, cpu); bad = 0;
    g_dev = device.device;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("GPU clear -> CPU ReadTexels, 32 literal pixels", 0, bad);
    EXPECT("ReadTexels used the GPU readback path", 1, device.allocations);
    EXPECT("ReadTexels releases its readback buffer before the caller's pool drains", 1, device.lastBuffer == nil);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("GPU copy into a packaged texture -> its storage (ReadTexels), 32 literal pixels", 0, bad);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk2, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("copy from a packaged texture with a readback pending -> read back too, 32 literal pixels", 0, bad);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk4, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("copy from a buffer a queued copy writes -> read back, 32 literal pixels", 0, bad);
    static const uint8_t expected_srgb[] = {124, 170, 203, 255};   // the clear colour sRGB-encoded (within 1: the GPU's encoding curve), not the source's 51, 102, 153
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk3, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) for (int k=0; k<4; k++) bad += abs((int)cpu[i*4+k] - expected_srgb[k]) > 1;
    EXPECT("format-converting copy from a CPU-written packaged texture -> converted bytes in its storage, 32 pixels within 1", 0, bad);
    static const uint8_t expected2[] = {204, 153, 102, 255}; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk6, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected2, 4) != 0;
    EXPECT("copy from a packaged texture the GPU cleared -> read back, 32 literal pixels", 0, bad);
    h_nvnTextureFinalize(&pk6); h_nvnTextureFinalize(&pk5); h_nvnTextureFinalize(&pk3); h_nvnTextureFinalize(&pk7); h_nvnTextureFinalize(&pk4); h_nvnTextureFinalize(&pk2);
    h_nvnTextureFinalize(&pk); h_nvnTextureFinalize(&tex); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
    printf("READBACK cases=%d failed=%d\n", cases, failed); return failed != 0;
} }
