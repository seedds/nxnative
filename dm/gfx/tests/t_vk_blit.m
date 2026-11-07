// vkCmdBlitImage on the GPU (hle_vk.m blit_image): layer 1 (green) of a two-layer 4x4 array whose layer 0 is red, blitted into a 2D image. The same-size region is a copy,
// the scaled one (to 8x8) a drawn quad: both must give green. The drawn path once bound the whole array to the shader's texture2d and read layer 0 (red), and Metal's
// validation stopped at the texture type. A multisampled source is refused before anything is encoded (the destination keeps its contents)
#include "../vk/hle_vk.m"
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static VkImage image(uint32_t w, uint32_t h, uint32_t layers, VkSampleCountFlagBits samples, VkImageUsageFlags usage) {
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1},
                             .mipLevels = 1, .arrayLayers = layers, .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VkImage im = NULL; CHECK(h_vkCreateImage(&g_device, &ici, NULL, &im) == VK_SUCCESS && im && im->tex, "image created");
    return im;
}
static void fill(VkImage im, uint32_t layer, uint32_t rgba) {   // through a shared buffer: the image's texture is private
    uint32_t n = im->w * im->h, px[64]; for (uint32_t i = 0; i < n; i++) px[i] = rgba;
    id<MTLBuffer> b = [g_dev newBufferWithBytes:px length:n * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:im->w * 4 sourceBytesPerImage:n * 4 sourceSize:MTLSizeMake(im->w, im->h, 1)
             toTexture:im->tex destinationSlice:layer destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static int all(VkImage im, uint32_t rgba) {   // every texel of layer 0 is rgba
    uint32_t n = im->w * im->h;
    id<MTLBuffer> b = [g_dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:im->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(im->w, im->h, 1)
               toBuffer:b destinationOffset:0 destinationBytesPerRow:im->w * 4 destinationBytesPerImage:n * 4];
    [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint32_t *p = b.contents; int ok = 1;
    for (uint32_t i = 0; i < n; i++) if (p[i] != rgba) { if (ok) fprintf(stderr, "  texel %u: %08x, want %08x\n", i, p[i], rgba); ok = 0; }
    return ok;
}
static void run_blit(VkImage src, uint32_t layer, VkImage dst, VkFilter filter) {   // what the replay does with one recorded C_BLIT
    c_blit *x = calloc(1, sizeof *x + sizeof(VkImageBlit));
    x->src = src; x->dst = dst; x->filter = filter; x->n = 1;
    x->r[0] = (VkImageBlit){{VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1}, {{0, 0, 0}, {(int32_t)src->w, (int32_t)src->h, 1}},
                            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)dst->w, (int32_t)dst->h, 1}}};
    rs_t s = {0}; s.cb = [g_q commandBuffer];
    blit_image(&s, x, C_BLIT);
    end_blit(&s); [s.cb commit]; [s.cb waitUntilCompleted];
    CHECK(s.cb.status == MTLCommandBufferStatusCompleted, "the blit's command buffer completed");
    free(x);
}

int main(void) {
    @autoreleasepool {
        dev_init();
        const uint32_t red = 0xff0000ff, green = 0xff00ff00, blue = 0xffff0000;   // RGBA8 in memory order R, G, B, A
        VkImageUsageFlags su = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, du = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImage arr = image(4, 4, 2, VK_SAMPLE_COUNT_1_BIT, su);
        CHECK(arr->tex.textureType == MTLTextureType2DArray, "the source is a 2D array texture");
        fill(arr, 0, red); fill(arr, 1, green);

        VkImage same = image(4, 4, 1, VK_SAMPLE_COUNT_1_BIT, du);
        run_blit(arr, 1, same, VK_FILTER_NEAREST);
        CHECK(all(same, green), "same-size blit of layer 1 copies green");

        VkImage big = image(8, 8, 1, VK_SAMPLE_COUNT_1_BIT, du);
        run_blit(arr, 1, big, VK_FILTER_LINEAR);
        CHECK(all(big, green), "scaled blit of layer 1 samples green, not layer 0's red");
        run_blit(arr, 0, big, VK_FILTER_NEAREST);
        CHECK(all(big, red), "scaled blit of layer 0 samples red");

        VkImage plain = image(4, 4, 1, VK_SAMPLE_COUNT_1_BIT, su);
        fill(plain, 0, green);
        run_blit(plain, 0, big, VK_FILTER_LINEAR);
        CHECK(all(big, green), "scaled blit of a plain 2D image");

        VkImage ms = image(4, 4, 1, VK_SAMPLE_COUNT_4_BIT, su | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkImage keep = image(8, 8, 1, VK_SAMPLE_COUNT_1_BIT, du);
        fill(keep, 0, blue);
        run_blit(ms, 0, keep, VK_FILTER_LINEAR);
        CHECK(all(keep, blue), "a multisampled source is refused: the destination keeps its contents");
    }
    printf("t_vk_blit: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
