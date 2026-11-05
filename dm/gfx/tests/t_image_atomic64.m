// 64-bit image atomics (Ampere SURED.D.2D.MAX.U64, PRAGMATA's visibility buffer; dmsh_atomic.wide_atomics): an NVN RG32UI image is an RG32Uint Metal texture with the
// shader-atomic usage on Apple GPU family 9, a 64-bit atomic slot accepts it (and a 32-bit one does not), the write sink of a 64-bit slot is RG32Uint, and texture2d<ulong>
// atomic_max / atomic_min compare the whole 64-bit texel on the GPU: R is the low word, G the high one. Also run with a translated shader: t_image_atomic64 <metallib>
// (test_dmsh_ampere_mem.py Sured64), whose kernel main0 does SURED.MAX.U64 of {lo 0xffffffff, hi 1} at (5, 2) and of {lo 7, hi 3} at (9, 2), outside the 8 x 4 image
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { W = 8, H = 4 };
static void put(id<MTLTexture> t, const uint64_t *texels) {
    id<MTLBuffer> b = [g_dev newBufferWithBytes:texels length:W * H * 8 options:MTLResourceStorageModeShared];
    CHECK(b != nil, "seed buffer"); if (!b) return;
    id<MTLCommandBuffer> c = q_cb(); id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:W * 8 sourceBytesPerImage:W * H * 8 sourceSize:MTLSizeMake(W, H, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding]; [c commit]; [c waitUntilCompleted];
    CHECK(c.status == MTLCommandBufferStatusCompleted, "seed completed");
}
static void get(id<MTLTexture> t, uint64_t *texels) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:W * H * 8 options:MTLResourceStorageModeShared];
    CHECK(b != nil, "readback buffer"); if (!b) return;
    id<MTLCommandBuffer> c = q_cb(); id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:W * 8 destinationBytesPerImage:W * H * 8];
    [e endEncoding]; [c commit]; [c waitUntilCompleted];
    CHECK(c.status == MTLCommandBufferStatusCompleted, "readback completed");
    memcpy(texels, b.contents, W * H * 8);
}
static void run(id<MTLFunction> fn, id<MTLTexture> t, int argbuf, NSUInteger threads) {
    NSError *error = nil;
    id<MTLComputePipelineState> pso = [g_dev newComputePipelineStateWithFunction:fn error:&error];
    CHECK(pso != nil, "64-bit atomic kernel builds"); if (!pso) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    id<MTLCommandBuffer> c = q_cb(); id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
    [e setComputePipelineState:pso];
    if (argbuf) {   // the translated shader's argument buffer: the image is member 0 (spvDescriptorSetBuffer0)
        id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> ab = [g_dev newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
        [args setArgumentBuffer:ab offset:0]; [args setTexture:t atIndex:0];
        [e setBuffer:ab offset:0 atIndex:0]; [e useResource:t usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    } else [e setTexture:t atIndex:0];
    [e dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(MIN(threads, 64), 1, 1)];
    [e endEncoding]; [c commit]; [c waitUntilCompleted];
    CHECK(c.status == MTLCommandBufferStatusCompleted, "64-bit atomic dispatch completed");
}
// 64 threads contend on texel 0 (max) and texel 2 (min) with values whose high and low words order differently, so a per-word max / min gives another answer
static const char *KERNEL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void k(texture2d<ulong, access::read_write> t [[texture(0)]], uint i [[thread_position_in_grid]]) {\n"
    "    t.atomic_max(uint2(0, 0), ulong4(as_type<ulong>(uint2(0xffffffffu - i, i % 3u))));\n"
    "    t.atomic_max(uint2(1, 0), ulong4(as_type<ulong>(uint2(0xffffffffu - i, i % 3u))));\n"
    "    t.atomic_min(uint2(2, 0), ulong4(as_type<ulong>(uint2(i, 9u - i % 3u))));\n"
    "}\n";
static void wide_case(id<MTLTexture> t, const char *what) {
    uint64_t in[W * H], out[W * H];
    for (int i = 0; i < W * H; i++) in[i] = 0x1111111100000000ull | i;
    in[0] = 0x0000000200000001ull;   // max: the largest high word is 2 (i = 2, 5, ...), its largest low word 0xffffffff - 2; a per-word max takes low 0xffffffff
    in[1] = 0x0000000500000000ull;   // max: every value is smaller (high <= 2), so it stays; a per-word max would raise its low word
    in[2] = 0x0000000800000000ull;   // min: the smallest high word is 7 (i = 2, 5, ...), its smallest low word 2; a per-word min takes low 0
    put(t, in);
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(KERNEL) options:nil error:&error];
    CHECK(lib != nil, "64-bit atomic kernel compiles"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    run([lib newFunctionWithName:@"k"], t, 0, 64);
    get(t, out);
    char label[160];
    snprintf(label, sizeof label, "%s: atomic_max compares the whole 64-bit texel (%#llx)", what, (unsigned long long)out[0]); CHECK(out[0] == 0x00000002fffffffdull, label);
    snprintf(label, sizeof label, "%s: atomic_max keeps a larger texel (%#llx)", what, (unsigned long long)out[1]); CHECK(out[1] == 0x0000000500000000ull, label);
    snprintf(label, sizeof label, "%s: atomic_min compares the whole 64-bit texel (%#llx)", what, (unsigned long long)out[2]); CHECK(out[2] == 0x0000000700000002ull, label);
    int same = 1; for (int i = 3; i < W * H; i++) same &= out[i] == in[i];
    snprintf(label, sizeof label, "%s: other texels untouched", what); CHECK(same, label);
}
static void translated_case(id<MTLTexture> t, const char *metallib) {
    uint64_t in[W * H], out[W * H];
    for (int i = 0; i < W * H; i++) in[i] = 0x0000000200000001ull;
    put(t, in);
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error];
    CHECK(lib != nil, "translated library loads"); if (!lib) return;
    run([lib newFunctionWithName:@"main0"], t, 1, 1);
    get(t, out);
    // (5, 2): {lo 0xffffffff, hi 1} is below {lo 1, hi 2}: unchanged (a per-word max gives 0x2ffffffff); (9, 2) is outside the image (IGN): nothing written
    int same = 1; for (int i = 0; i < W * H; i++) same &= out[i] == in[i];
    CHECK(same, "translated SURED.MAX.U64 below the texel leaves the image unchanged");
    for (int i = 0; i < W * H; i++) in[i] = 0x0000000100000000ull;
    put(t, in); run([lib newFunctionWithName:@"main0"], t, 1, 1); get(t, out);
    same = 1; for (int i = 0; i < W * H; i++) same &= out[i] == (i == 2 * W + 5 ? 0x00000001ffffffffull : in[i]);
    char label[160]; snprintf(label, sizeof label, "translated SURED.MAX.U64 raises texel (5, 2) only (%#llx)", (unsigned long long)out[2 * W + 5]); CHECK(same, label);
}
int main(int argc, const char **argv) { @autoreleasepool {
    mtl_init();
    if (!dev_atomic64()) { printf("64-bit image atomics: this GPU is not Apple family 9, skipped\n"); return 0; }
    tex_t image = {.magic=MAGIC('T','E','X',0), .serial=99701};
    image.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RG32UI, .flags=NVN_TEXTURE_FLAGS_IMAGE, .width=W, .height=H, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&image.d, &image.L); mtl_tex_init(&image);
    id<MTLTexture> t = tex_mtl(&image);
    CHECK(t && t.pixelFormat == MTLPixelFormatRG32Uint, "an NVN RG32UI image is an RG32Uint texture");
    CHECK(t && tex_atomic_capable(t), "a shader-writable RG32Uint texture is atomic-capable on Apple family 9");
    nvn_rbind wide = {.kind=NVN_RB_IMG, .dtt=MTLTextureType2D, .dpf=NVN_DPF_UINT, .rw=1, .atom=2}, narrow = wide; narrow.atom = 1;
    CHECK(t && img_atomic_ok(t, &wide), "a 64-bit atomic slot accepts it");
    CHECK(t && !img_atomic_ok(t, &narrow), "a 32-bit atomic slot does not");
    id<MTLTexture> sink = dummy_img_atomic(MTLTextureType2D, NVN_DPF_UINT, 1);
    CHECK(sink.pixelFormat == MTLPixelFormatRG32Uint && img_atomic_ok(sink, &wide), "a 64-bit slot's write sink is an atomic-capable RG32Uint texture");
    CHECK(dummy_img_atomic(MTLTextureType2D, NVN_DPF_UINT, 0).pixelFormat == MTLPixelFormatR32Uint, "a 32-bit slot's write sink stays R32Uint");
    tex_t plain = image; plain.serial++; plain.off = 1 << 20; plain.d.flags = 0; mtl_tex_init(&plain);   // elsewhere in the pool: the same offset would share the image's texture
    CHECK(tex_mtl(&plain) && !tex_atomic_capable(tex_mtl(&plain)), "an RG32Uint texture that is not an image does not get the atomic usage");
    if (t) wide_case(t, "host-made RG32UI image");
    {   // a linear image over pool memory is a texture over a buffer: the same atomics on it
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Uint width:W height:H mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | img_usage(&image, MTLPixelFormatRG32Uint); d.storageMode = MTLStorageModeShared;
        NSUInteger row = [g_dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRG32Uint]; row = (W * 8 + row - 1) / row * row;
        id<MTLBuffer> pool = [g_dev newBufferWithLength:row * H options:MTLResourceStorageModeShared];
        id<MTLTexture> linear = [pool newTextureWithDescriptor:d offset:0 bytesPerRow:row];
        CHECK(linear != nil, "a linear RG32Uint image with the atomic usage is made");
        if (linear) wide_case(linear, "linear RG32Uint image");
    }
    if (argc > 1 && t) translated_case(t, argv[1]);
    printf("64-bit image atomics: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
