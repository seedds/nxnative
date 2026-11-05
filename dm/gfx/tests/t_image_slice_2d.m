// An image slot gets a view in the dimension its shader declares. The host holds a texture with layers as a 2D array, and a view of one slice of it stays a 2D array view; Metal does not take
// that for a texture2d argument, its writes are lost and its reads undefined (Mario Kart World's mip generator, whose six texture2d outputs were slices and levels of a 64x64x128 array). The
// slot is fitted to 2D with the slice and level it names, a plain 2D texture to a 2D array slot, and any other mismatch is bound as before.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { W = 8, LAYERS = 4, SLICE = 2, LEVEL = 1, N = W >> LEVEL };
static void read_level(id<MTLTexture> texture, unsigned slice, unsigned level, uint8_t *out) {
    NSUInteger n = W >> level;
    id<MTLBuffer> readback = [g_dev newBufferWithLength:n * n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = q_cb(); id<MTLBlitCommandEncoder> copy = command ? [command blitCommandEncoder] : nil;
    CHECK(readback && command && copy, "readback encoder created");
    if (!readback || !command || !copy) { memset(out, 0xff, n * n); return; }
    [copy copyFromTexture:texture sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(n,n,1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:n destinationBytesPerImage:n * n];
    [copy endEncoding]; [command commit]; [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted, "readback completed");
    memcpy(out, readback.contents, n * n);
}
static void write_pixel(id<MTLTexture> image) {
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void put(texture2d<float, access::write> t [[texture(0)]]) { t.write(float4(1.0), uint2(1, 1)); }\n" options:nil error:&error];
    id<MTLFunction> put = lib ? [lib newFunctionWithName:@"put"] : nil;
    id<MTLComputePipelineState> pipeline = put ? [g_dev newComputePipelineStateWithFunction:put error:&error] : nil;
    CHECK(pipeline != nil, "image writer compiled");
    if (!pipeline) return;
    id<MTLCommandBuffer> write = q_cb(); id<MTLComputeCommandEncoder> encoder = write ? [write computeCommandEncoder] : nil;
    CHECK(write && encoder, "image write encoder created");
    if (!write || !encoder) return;
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:image atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [encoder endEncoding]; [write commit]; [write waitUntilCompleted];
    CHECK(write.status == MTLCommandBufferStatusCompleted, "image write completed");
}
static tex_t make(int serial, int target, int layers) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .off=(uint64_t)serial << 20};
    t.d = (nvn_texdesc){.target=target, .format=NVN_FORMAT_R8, .flags=NVN_TEXTURE_FLAGS_IMAGE, .width=W, .height=W, .depth=layers, .levels=3, .samples=1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
int main(void) { @autoreleasepool {
    mtl_init();
    NvShader *shader = [NvShader new]; shader->rf.nb = 1;
    // a level and a slice of a 2D array, written by a shader that declares texture2d
    tex_t array = make(99601, NVN_TEXTURE_TARGET_2D_ARRAY, LAYERS);
    view_t slice = {.set_levels=1, .base_level=LEVEL, .nlevels=1, .set_layers=1, .min_layer=SLICE, .nlayers=1};
    mtl_reg_image(701, &array, &slice);
    shader->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .dtt=MTLTextureType2D, .dpf=NVN_DPF_FLOAT, .reg=0, .rw=1};
    uint64_t rid; int real;
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> image = img_binding(701, &shader->rf.b[0], &rid, &real);
    os_unfair_lock_unlock(&g_rmu);
    id<MTLTexture> root = tex_mtl(&array);
    CHECK(real && image, "the slice is a real image binding");
    CHECK(image.textureType == MTLTextureType2D, "a texture2d slot gets a 2D view of the array's slice");
    CHECK(image.parentTexture == root && image.parentRelativeLevel == LEVEL && image.parentRelativeSlice == SLICE && image.mipmapLevelCount == 1, "made from the array itself, at the named level and slice");
    CHECK(image.usage & MTLTextureUsageShaderWrite, "the view takes shader writes");
    CHECK(rid == image.gpuResourceID._impl, "the argument names that view");
    if (image.textureType == MTLTextureType2D) {
        write_pixel(image);
        uint8_t px[N * N]; read_level(root, SLICE, LEVEL, px);
        int ok = 1; for (int i = 0; i < N * N; i++) ok &= px[i] == (i == N + 1 ? 255 : 0);
        CHECK(ok, "the write lands in that level of that slice only");
        for (unsigned s = 0; s < LAYERS; s++) if (s != SLICE) { read_level(root, s, LEVEL, px); ok = 1; for (int i = 0; i < N * N; i++) ok &= px[i] == 0; CHECK(ok, "the other slices stay untouched"); }
        read_level(root, SLICE, 0, px); ok = 1; for (int i = 0; i < W * W; i++) ok &= px[i] == 0; CHECK(ok, "level 0 of the slice stays untouched");
    }
    // the same texture where the shader declares a 2D array: as it is
    shader->rf.b[0].dtt = MTLTextureType2DArray;
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> same = img_binding(701, &shader->rf.b[0], &rid, &real); os_unfair_lock_unlock(&g_rmu);
    CHECK(real && same.textureType == MTLTextureType2DArray, "a 2D array slot keeps the array view");
    // a plain 2D texture in a 2D array slot
    tex_t plain = make(99602, NVN_TEXTURE_TARGET_2D, 1);
    mtl_reg_image(702, &plain, NULL);
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> up = img_binding(702, &shader->rf.b[0], &rid, &real); os_unfair_lock_unlock(&g_rmu);
    CHECK(real && up.textureType == MTLTextureType2DArray && up.arrayLength == 1 && up.parentTexture == tex_mtl(&plain), "a 2D texture in a 2D array slot gets a one-layer array view");
    shader->rf.b[0].dtt = MTLTextureType2D;
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> flat = img_binding(702, &shader->rf.b[0], &rid, &real); os_unfair_lock_unlock(&g_rmu);
    CHECK(real && flat.textureType == MTLTextureType2D, "a 2D texture in a 2D slot is bound as it is");
    // any other mismatch is not ours to fit: bound as before
    shader->rf.b[0].dtt = MTLTextureType3D;
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> other = img_binding(701, &shader->rf.b[0], &rid, &real); os_unfair_lock_unlock(&g_rmu);
    CHECK(real && other.textureType == MTLTextureType2DArray, "a 3D slot over a 2D array view is bound as before");
    printf("image slots in the declared dimension: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
