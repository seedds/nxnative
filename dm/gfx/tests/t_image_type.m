// An image slot takes a view of the type its shader declares (img_fit): a one-layer view of a 2D array in a slot declared 2D gets a writable 2D view of that layer and level, and the
// shader's stores land there. Bound as is, Metal dropped them: Mario Kart World's blob-shadow mips stayed zero (GitHub issue #1648). A type no view bridges is bound as is, logged red
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
enum { TID = 500, W = 8, LAYERS = 4, LEVELS = 3, LAYER = 2, LEVEL = 1 };
static uint8_t texel_at(id<MTLTexture> base, NSUInteger level, NSUInteger slice) {
    uint8_t px[W * W] = {0}; NSUInteger w = W >> level;
    [base getBytes:px bytesPerRow:w bytesPerImage:w * w fromRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:level slice:slice];
    uint8_t v = px[0]; for (NSUInteger i = 1; i < w * w; i++) if (px[i] != v) return 0xee;   // 0xee: not uniform
    return v;
}
int main(void) { @autoreleasepool {
    mtl_init();
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=99600};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D_ARRAY, .format=NVN_FORMAT_R8, .width=W, .height=W, .depth=LAYERS, .levels=LEVELS, .samples=1, .flags=NVN_TEXTURE_FLAGS_IMAGE};
    MTLTextureDescriptor *d = tex_desc(&t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    CHECK(base && base.textureType == MTLTextureType2DArray && base.arrayLength == LAYERS && (base.usage & MTLTextureUsageShaderWrite), "a writable R8 2D array");
    serial_put(t.serial, base);
    view_t one = {.set_levels=1, .base_level=LEVEL, .nlevels=1, .set_layers=1, .min_layer=LAYER, .nlayers=1, .set_target=1, .target=NVN_TEXTURE_TARGET_2D_ARRAY};   // one layer, one level, as an array
    mtl_reg_tex(TID, &t, &one);
    nvn_rbind b2d = {.kind=NVN_RB_IMG, .dtt=MTLTextureType2D, .dpf=NVN_DPF_FLOAT, .rw=1}, barr = b2d, b3d = b2d;
    barr.dtt = MTLTextureType2DArray; b3d.dtt = MTLTextureType3D;
    os_unfair_lock_lock(&g_rmu);
    uint64_t rid = 0, rid2 = 0, rid_arr = 0, rid_3d = 0; int real = 0, real2 = 0, real_arr = 0, real_3d = 0;
    id<MTLTexture> img = img_binding(TID, &b2d, &rid, &real);
    id<MTLTexture> again = img_binding(TID, &b2d, &rid2, &real2);
    id<MTLTexture> arr = img_binding(TID, &barr, &rid_arr, &real_arr);
    int red_before = g_red_once[157];
    id<MTLTexture> vol = img_binding(TID, &b3d, &rid_3d, &real_3d);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(real && img.textureType == MTLTextureType2D, "a slot declared 2D gets a 2D view");
    CHECK(img.parentTexture == base && img.parentRelativeSlice == LAYER && img.parentRelativeLevel == LEVEL && img.mipmapLevelCount == 1, "of the registered view's layer and level, made from the texture under it");
    CHECK((img.usage & MTLTextureUsageShaderWrite) && rid == img.gpuResourceID._impl, "writable, with its own resource ID");
    CHECK(real2 && again == img && rid2 == rid, "kept for the next bind");
    CHECK(real_arr && arr.textureType == MTLTextureType2DArray && arr.arrayLength == 1 && arr != img, "a slot declared 2D array gets the registered view as it is");
    CHECK(real_3d && vol.textureType == MTLTextureType2DArray && g_red_once[157] == red_before + 1, "a 3D slot: no view bridges it, bound as is and logged red");
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void store(texture2d<float, access::write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(float4(0.5), p); }\n" options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"store"];
    id<MTLComputePipelineState> pipe = function ? [g_dev newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipe) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pipe]; [e setTexture:img atIndex:0];
    [e dispatchThreads:MTLSizeMake(W >> LEVEL, W >> LEVEL, 1) threadsPerThreadgroup:MTLSizeMake(W >> LEVEL, W >> LEVEL, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "the store kernel ran");
    CHECK(texel_at(base, LEVEL, LAYER) == 128, "the stores land in the registered layer and level");
    CHECK(texel_at(base, LEVEL, LAYER - 1) == 0 && texel_at(base, LEVEL, LAYER + 1) == 0 && texel_at(base, 0, LAYER) == 0 && texel_at(base, LEVEL + 1, LAYER) == 0, "and nowhere else");
    printf("image slots of another type: %d checks, %d failed\n", checks, fails); return fails != 0;
} }
