// Cube descriptors used by image2DArray instructions must write the same faces
// and mip as the sampled cube. A cube view is not a writable array binding.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void image_case(int tid, int array, int subset, int integer, id<MTLComputePipelineState> pipeline) { @autoreleasepool {
    unsigned faces = array ? 12 : 6, first = subset ? 6 : 0, count = subset ? 6 : faces;
    tex_t texture = {.magic=MAGIC('T','E','X',0), .serial=99000+(unsigned)tid};
    texture.d = (nvn_texdesc){.target=array ? NVN_TEXTURE_TARGET_CUBEMAP_ARRAY : NVN_TEXTURE_TARGET_CUBEMAP,
        .format=NVN_FORMAT_RGBA8, .flags=NVN_TEXTURE_FLAGS_IMAGE, .samples=1, .width=8, .height=8, .depth=array ? 12 : 1, .levels=4};
    nvn_tex_layout(&texture.d, &texture.L); mtl_tex_init(&texture);
    MTLTextureDescriptor *description = tex_desc(&texture, 0);
    description.storageMode = MTLStorageModeShared;
    description.usage |= MTLTextureUsagePixelFormatView;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:description];
    CHECK(base && base.textureType == MTLTextureType2DArray && base.arrayLength == faces, "cube storage holds all physical faces");
    if (!base) return;
    res_add(base); serial_put(texture.serial, base);
    uint32_t seed[64]; for (int i = 0; i < 64; i++) seed[i] = 0x55aa33cc;
    for (unsigned level = 0; level < 4; level++) for (unsigned face = 0; face < faces; face++) {
        unsigned width = 8 >> level;
        [base replaceRegion:MTLRegionMake3D(0,0,0,width,width,1) mipmapLevel:level slice:face withBytes:seed bytesPerRow:width*4 bytesPerImage:width*width*4];
    }
    view_t view = {.set_target=1, .target=array ? NVN_TEXTURE_TARGET_CUBEMAP_ARRAY : NVN_TEXTURE_TARGET_CUBEMAP,
                   .set_levels=1, .base_level=1, .nlevels=2, .set_layers=subset, .min_layer=subset ? 1 : 0, .nlayers=subset ? 1 : 0};
    mtl_reg_image(tid, &texture, &view);
    nvn_rbind binding = {.kind=NVN_RB_IMG, .dtt=MTLTextureType2DArray, .dpf=integer ? NVN_DPF_UINT : NVN_DPF_FLOAT, .rw=1};
    uint64_t rid, again_rid; int real, again_real;
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> image = img_binding(tid, &binding, &rid, &real);
    id<MTLTexture> again = img_binding(tid, &binding, &again_rid, &again_real);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(real && image.textureType == MTLTextureType2DArray, "cube image binding matches the shader's array type");
    CHECK(image.arrayLength == count, "image binding retains every selected face");
    CHECK(image.width == 4 && image.mipmapLevelCount == 2 && image.parentRelativeLevel == 1, "image binding retains its mip range");
    CHECK(image.parentTexture == base && image.parentRelativeSlice == first, "image view uses the resident root and selected slice offset");
    CHECK((image.usage & MTLTextureUsageShaderWrite) != 0, "image view is writable without a swizzle");
    CHECK(image.pixelFormat == (integer ? MTLPixelFormatR32Uint : MTLPixelFormatRGBA8Unorm), "image binding keeps its requested pixel class");
    CHECK(rid == image.gpuResourceID._impl && again == image && again_rid == rid && again_real, "repeated image binds retain the actual view and resource ID");
    // Do not submit an invalid resource to validation in the failing baseline.
    if (!real || image.textureType != MTLTextureType2DArray || !(image.usage & MTLTextureUsageShaderWrite)) return;
    id<MTLCommandBuffer> command = q_cb(); id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:image atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(4,4,count) threadsPerThreadgroup:MTLSizeMake(4,4,1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted && !command.error, "GPU accepts the image binding");
    for (unsigned level = 0; level < 4; level++) for (unsigned face = 0; face < faces; face++) {
        uint32_t pixels[64]; unsigned width = 8 >> level;
        [base getBytes:pixels bytesPerRow:width*4 bytesPerImage:width*width*4 fromRegion:MTLRegionMake3D(0,0,0,width,width,1) mipmapLevel:level slice:face];
        int same = 1;
        for (unsigned y = 0; y < width; y++) for (unsigned x = 0; x < width; x++) {
            uint32_t actual = pixels[y*width+x];
            if (level != 1 || face < first || face >= first + count) same &= actual == 0x55aa33cc;
            else if (integer) same &= actual == (0xa0500000u | (face-first)*256u | y*16u | x);
            else { const uint8_t *rgba = (const uint8_t *)&actual; same &= abs((int)rgba[0]-(int)((face-first+1)*16)) <= 1 && abs((int)rgba[1]-(int)(x*16)) <= 1 && abs((int)rgba[2]-(int)(y*16)) <= 1 && rgba[3] == 255; }
        }
        CHECK(same, "sampled backing sees every image write, preserving other faces and mip levels");
    }
} }
int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void put_float(texture2d_array<float, access::write> t [[texture(0)]], uint3 p [[thread_position_in_grid]]) { t.write(float4(float(p.z+1)/16.0,float(p.x)/16.0,float(p.y)/16.0,1.0),p.xy,p.z); }\n"
        "kernel void put_uint(texture2d_array<uint, access::write> t [[texture(0)]], uint3 p [[thread_position_in_grid]]) { t.write(uint4(0xa0500000u | p.z*256u | p.y*16u | p.x),p.xy,p.z); }\n";
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    int tid = 700;
    for (int integer = 0; integer < 2; integer++) {
        id<MTLFunction> function = [library newFunctionWithName:integer ? @"put_uint" : @"put_float"];
        if (!function) { fprintf(stderr, "missing image writer function\n"); return 2; }
        id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        image_case(tid++, 0, 0, integer, pipeline);
        image_case(tid++, 1, 0, integer, pipeline);
        image_case(tid++, 1, 1, integer, pipeline);
    }
    printf("cube image views: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
