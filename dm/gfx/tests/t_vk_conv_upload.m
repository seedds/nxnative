// Converted linear images (fmt_t.conv: R4G4, B4G4R4A4, B5G6R5, B5G5R5A1) are unpacked on the CPU before each vkQueueSubmit (conv_sync).
// The first upload must run unconditionally rather than comparing against an in-band sentinel in im->shadow (0xA5 used to skip an all-0xA5
// input on the first submit, leaving the Metal texture zeroed). Byte comparison against im->shadow activates only after the first upload.
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
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum { W = 16, H = 16 };

static void submit_empty(void) {
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    CHECK(h_vkQueueSubmit(&g_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "vkQueueSubmit succeeded");
    CHECK(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS, "vkQueueWaitIdle succeeded");
}

static void readback_tex(id<MTLTexture> tex, uint32_t bpp, uint8_t *out_bytes) {
    size_t row = (size_t)W * bpp, total = row * H;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:total options:MTLResourceStorageModeShared];
    CHECK(buf != nil, "readback buffer allocated");
    if (!buf) return;
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:total];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "readback blit completed");
    memcpy(out_bytes, buf.contents, total);
}

static void check_all_texels(const uint8_t *got, uint32_t bpp, const uint8_t *want, const char *label) {
    int ok = 1;
    for (uint32_t i = 0; i < W * H; i++) {
        if (memcmp(got + (size_t)i * bpp, want, bpp) != 0) {
            ok = 0;
            fprintf(stderr, "  %s: texel %u got", label, i);
            for (uint32_t j = 0; j < bpp; j++) fprintf(stderr, " %02x", got[(size_t)i * bpp + j]);
            fprintf(stderr, ", want");
            for (uint32_t j = 0; j < bpp; j++) fprintf(stderr, " %02x", want[j]);
            fputc('\n', stderr);
            break;
        }
    }
    CHECK(ok, "%s", label);
}

int main(void) {
    VkDevice dev = &g_device;
    @autoreleasepool {
        dev_init();

        // 1. R4G4_UNORM_PACK8 filled with 0xA5 (the former shadow sentinel): first submit must upload AA 55, not leave 00 00.
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R4G4_UNORM_PACK8,
            .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        };
        VkImage im = NULL;
        CHECK(h_vkCreateImage(dev, &ici, NULL, &im) == VK_SUCCESS && im, "R4G4 linear image created");
        VkMemoryRequirements mr = {0};
        h_vkGetImageMemoryRequirements(dev, im, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = 1};
        VkDeviceMemory mem = NULL;
        CHECK(h_vkAllocateMemory(dev, &mai, NULL, &mem) == VK_SUCCESS && mem, "R4G4 memory allocated");
        CHECK(h_vkBindImageMemory(dev, im, mem, 0) == VK_SUCCESS && im->tex, "R4G4 image bound");
        CHECK(im->conv_synced == 0, "R4G4 starts unsynced before first submit");

        void *mapped = NULL;
        CHECK(h_vkMapMemory(dev, mem, 0, im->size, 0, &mapped) == VK_SUCCESS && mapped, "R4G4 memory mapped");
        memset(mapped, 0xA5, im->size);

        submit_empty();
        CHECK(im->conv_synced == 1, "R4G4 marked synced after first submit");
        uint8_t got2[W * H * 2] = {0};
        readback_tex(im->tex, 2, got2);
        const uint8_t want_a5[2] = {0xAA, 0x55};
        check_all_texels(got2, 2, want_a5, "all-0xA5 R4G4 texture uploads on first submit");

        // 2. After the first upload, unchanged guest memory skips re-upload (overwrite Metal texture directly with a sentinel marker).
        uint8_t marker[W * H * 2];
        for (uint32_t i = 0; i < W * H; i++) { marker[2 * i] = 0x11; marker[2 * i + 1] = 0x22; }
        [im->tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:marker bytesPerRow:W * 2];
        submit_empty();
        readback_tex(im->tex, 2, got2);
        const uint8_t want_marker[2] = {0x11, 0x22};
        check_all_texels(got2, 2, want_marker, "unchanged R4G4 memory skips upload on subsequent submit");

        // 3. Mutating guest memory triggers a fresh upload (0xA4 -> AA 44).
        memset(mapped, 0xA4, im->size);
        submit_empty();
        readback_tex(im->tex, 2, got2);
        const uint8_t want_a4[2] = {0xAA, 0x44};
        check_all_texels(got2, 2, want_a4, "modified R4G4 memory (0xA4) re-uploads on submit");

        h_vkDestroyImage(dev, im, NULL);
        h_vkFreeMemory(dev, mem, NULL);

        // 4. Also verify a 16-bit converted format (B4G4R4A4_UNORM_PACK16) filled with 0xA5 bytes uploads on first submit.
        ici.format = VK_FORMAT_B4G4R4A4_UNORM_PACK16;
        VkImage im16 = NULL;
        CHECK(h_vkCreateImage(dev, &ici, NULL, &im16) == VK_SUCCESS && im16, "B4G4R4A4 linear image created");
        h_vkGetImageMemoryRequirements(dev, im16, &mr);
        mai.allocationSize = mr.size;
        VkDeviceMemory mem16 = NULL;
        CHECK(h_vkAllocateMemory(dev, &mai, NULL, &mem16) == VK_SUCCESS && mem16, "B4G4R4A4 memory allocated");
        CHECK(h_vkBindImageMemory(dev, im16, mem16, 0) == VK_SUCCESS && im16->tex, "B4G4R4A4 image bound");
        void *mapped16 = NULL;
        CHECK(h_vkMapMemory(dev, mem16, 0, im16->size, 0, &mapped16) == VK_SUCCESS && mapped16, "B4G4R4A4 memory mapped");
        memset(mapped16, 0xA5, im16->size);

        submit_empty();
        uint8_t got4[W * H * 4] = {0};
        readback_tex(im16->tex, 4, got4);
        const uint8_t want_b4[4] = {0xAA, 0x55, 0xAA, 0x55};
        check_all_texels(got4, 4, want_b4, "all-0xA5 B4G4R4A4 texture uploads on first submit");

        h_vkDestroyImage(dev, im16, NULL);
        h_vkFreeMemory(dev, mem16, NULL);
    }
    printf("Vulkan converted texture upload: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
