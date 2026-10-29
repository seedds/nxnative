// BC7 images backed by ASTC 4x4 (DM_VK_BC7_ASTC=1, for a RomFS whose BC7 textures dm/tools/pkg_astc.py re-encoded, GPUs without BC: the A16): a VK_FORMAT_BC7_* image is
// an ASTC 4x4 texture of the same size and sRGB-ness, an upload of its 16-byte blocks lands as they are, and the ASTC formats themselves are unchanged
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

static VkImage image(VkDevice dev, VkFormat f) {
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {8, 8, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImage im = NULL; return h_vkCreateImage(dev, &ici, NULL, &im) == VK_SUCCESS ? im : NULL;
}

int main(void) {
    setenv("DM_VK_BC7_ASTC", "1", 1);
    VkDevice dev = &g_device;
    @autoreleasepool {
        dev_init();
        if (![g_dev supportsFamily:MTLGPUFamilyApple2]) { printf("t_vk_bc7_astc: no ASTC on this GPU, skipped\n"); return 0; }
        VkImage u = image(dev, VK_FORMAT_BC7_UNORM_BLOCK), s = image(dev, VK_FORMAT_BC7_SRGB_BLOCK), a = image(dev, VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
        CHECK(u && u->tex.pixelFormat == MTLPixelFormatASTC_4x4_LDR, "BC7 UNORM image is ASTC 4x4 LDR");
        CHECK(s && s->tex.pixelFormat == MTLPixelFormatASTC_4x4_sRGB, "BC7 sRGB image is ASTC 4x4 sRGB");
        CHECK(a && a->tex.pixelFormat == MTLPixelFormatASTC_4x4_sRGB, "ASTC 4x4 image unchanged");
        CHECK(u && u->size == 4 * 16, "8x8 BC7 image is four 16-byte blocks");
        VkFormatProperties p; h_vkGetPhysicalDeviceFormatProperties(NULL, VK_FORMAT_BC7_UNORM_BLOCK, &p);
        CHECK(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "BC7 still reported as sampled");
        // The four blocks a buffer copy puts there come back unchanged: ASTC void-extent blocks (one colour each), so the texture holds exactly these bytes
        uint8_t blocks[64];
        for (int b = 0; b < 4; b++) {
            static const uint8_t ve[8] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            memcpy(blocks + 16 * b, ve, 8);
            uint16_t rgba[4] = {(uint16_t)(0x1111 * (b + 1)), 0x8000, 0x0000, 0xFFFF}; memcpy(blocks + 16 * b + 8, rgba, 8);
        }
        id<MTLBuffer> src = [g_dev newBufferWithBytes:blocks length:sizeof blocks options:MTLResourceStorageModeShared];
        id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof blocks options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:2 * 16 sourceBytesPerImage:sizeof blocks sourceSize:MTLSizeMake(8, 8, 1)
                 toTexture:u->tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl copyFromTexture:u->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
                   toBuffer:dst destinationOffset:0 destinationBytesPerRow:2 * 16 destinationBytesPerImage:sizeof blocks];
        [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !memcmp(dst.contents, blocks, sizeof blocks), "ASTC blocks round trip through the BC7 image");
    }
    printf("t_vk_bc7_astc: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
