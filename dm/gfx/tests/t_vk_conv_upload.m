// Converted linear images (fmt_t.conv: R4G4, B4G4R4A4, B5G6R5, B5G5R5A1) are unpacked on the CPU before each vkQueueSubmit (conv_sync).
// The first upload must run unconditionally rather than comparing against an in-band sentinel in im->shadow (0xA5 used to skip an all-0xA5
// input on the first submit, leaving the Metal texture zeroed). Byte comparison against im->shadow activates only after the first upload.
// VK_EXT_4444_formats (A4R4G4B4, A4B4G4R4) is covered end to end: the extension and its feature struct, both formats read back from the RGBA8 texture with
// every nibble of a page checked per channel (alpha from 15:12, red from the nibble the format's name gives it), the same page optimal-tiled through
// vkCmdCopyBufferToImage (conv_row in C_COPYB2I), and the refused vkCmdCopyImageToBuffer.
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

// A 16x16 VK_EXT_4444_formats page in which every nibble of every channel differs: the packed word is A = 0xF, 11:8 = 15-x, 7:4 = y, 3:0 = x^y. Read as
// A4R4G4B4 that is red 15-x, green y, blue x^y; read as A4B4G4R4 it is blue 15-x, green y, red x^y. One page therefore tells the two formats apart, and its
// alpha (0xF in the high nibble, 0xFF expanded) tells alpha from every colour channel: a mapping that lets Metal read the pack as ABGR4Unorm puts alpha in
// red, which this page reports as red 0xFF, green y, blue x^y, alpha 15-x.
static uint16_t page4444(uint32_t x, uint32_t y) { return (uint16_t)(0xF000u | (uint32_t)(15 - x) << 8 | y << 4 | ((x ^ y) & 15)); }
static void fill_page4444(uint8_t *row, uint32_t w, uint32_t y) { for (uint32_t x = 0; x < w; x++) { uint16_t v = page4444(x, y); row[2 * x] = (uint8_t)v; row[2 * x + 1] = (uint8_t)(v >> 8); } }
// What the RGBA8 texture must hold for that page, in Vulkan's channel order. `swap_rb` is the A4B4G4R4 naming of the same packed word.
static void want_page4444(uint8_t *out, uint32_t i, uint32_t x, uint32_t y, int swap_rb) {
    uint8_t hi = (uint8_t)x4(15 - x), lo = (uint8_t)x4(x ^ y);
    out[4 * i + 0] = swap_rb ? lo : hi; out[4 * i + 1] = (uint8_t)x4(y); out[4 * i + 2] = swap_rb ? hi : lo; out[4 * i + 3] = (uint8_t)x4(15);
}
static void check_page4444(const uint8_t *got, const char *label, int swap_rb) {
    for (uint32_t y = 0, i = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++, i++) {
            uint8_t want[4]; want_page4444(want, 0, x, y, swap_rb);
            if (memcmp(got + 4 * i, want, 4) != 0) {
                fprintf(stderr, "  %s: texel (%u,%u) got %02x %02x %02x %02x, want %02x %02x %02x %02x\n", label, x, y,
                        got[4 * i], got[4 * i + 1], got[4 * i + 2], got[4 * i + 3], want[0], want[1], want[2], want[3]);
                CHECK(0, "%s", label);
                return;
            }
        }
    CHECK(1, "%s", label);
}

static void submit_empty(void) {
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    CHECK(h_vkQueueSubmit(&g_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "vkQueueSubmit succeeded");
    CHECK(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS, "vkQueueWaitIdle succeeded");
}

static void readback_tex(id<MTLTexture> tex, uint32_t bpp, uint32_t w, uint32_t h, uint8_t *out_bytes) {
    size_t row = (size_t)w * bpp, total = row * h;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:total options:MTLResourceStorageModeShared];
    CHECK(buf != nil, "readback buffer allocated");
    if (!buf) return;
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
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
        VkDeviceMemory mem = NULL;
        VkResult result = h_vkCreateImage(dev, &ici, NULL, &im);
        CHECK(result == VK_SUCCESS && im, "R4G4 linear image created");
        if (result != VK_SUCCESS || !im) goto cleanup;
        VkMemoryRequirements mr = {0};
        h_vkGetImageMemoryRequirements(dev, im, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = 1};
        result = h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(result == VK_SUCCESS && mem, "R4G4 memory allocated");
        if (result != VK_SUCCESS || !mem) goto cleanup;
        result = h_vkBindImageMemory(dev, im, mem, 0);
        CHECK(result == VK_SUCCESS && im->tex, "R4G4 image bound");
        if (result != VK_SUCCESS || !im->tex) goto cleanup;
        CHECK(im->conv_synced == 0, "R4G4 starts unsynced before first submit");

        void *mapped = NULL;
        result = h_vkMapMemory(dev, mem, 0, im->size, 0, &mapped);
        CHECK(result == VK_SUCCESS && mapped, "R4G4 memory mapped");
        if (result != VK_SUCCESS || !mapped) goto cleanup;
        memset(mapped, 0xA5, im->size);

        submit_empty();
        CHECK(im->conv_synced == 1, "R4G4 marked synced after first submit");
        uint8_t got2[W * H * 2] = {0};
        readback_tex(im->tex, 2, W, H, got2);
        const uint8_t want_a5[2] = {0xAA, 0x55};
        check_all_texels(got2, 2, want_a5, "all-0xA5 R4G4 texture uploads on first submit");

        // 2. After the first upload, unchanged guest memory skips re-upload (overwrite Metal texture directly with a sentinel marker).
        uint8_t marker[W * H * 2];
        for (uint32_t i = 0; i < W * H; i++) { marker[2 * i] = 0x11; marker[2 * i + 1] = 0x22; }
        [im->tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:marker bytesPerRow:W * 2];
        submit_empty();
        readback_tex(im->tex, 2, W, H, got2);
        const uint8_t want_marker[2] = {0x11, 0x22};
        check_all_texels(got2, 2, want_marker, "unchanged R4G4 memory skips upload on subsequent submit");

        // 3. Mutating guest memory triggers a fresh upload (0xA4 -> AA 44).
        memset(mapped, 0xA4, im->size);
        submit_empty();
        readback_tex(im->tex, 2, W, H, got2);
        const uint8_t want_a4[2] = {0xAA, 0x44};
        check_all_texels(got2, 2, want_a4, "modified R4G4 memory (0xA4) re-uploads on submit");

        h_vkDestroyImage(dev, im, NULL);
        h_vkFreeMemory(dev, mem, NULL);
        im = NULL;
        mem = NULL;

        // 4. Also verify a 16-bit converted format (B4G4R4A4_UNORM_PACK16) filled with 0xA5 bytes uploads on first submit.
        ici.format = VK_FORMAT_B4G4R4A4_UNORM_PACK16;
        result = h_vkCreateImage(dev, &ici, NULL, &im);
        CHECK(result == VK_SUCCESS && im, "B4G4R4A4 linear image created");
        if (result != VK_SUCCESS || !im) goto cleanup;
        h_vkGetImageMemoryRequirements(dev, im, &mr);
        mai.allocationSize = mr.size;
        result = h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(result == VK_SUCCESS && mem, "B4G4R4A4 memory allocated");
        if (result != VK_SUCCESS || !mem) goto cleanup;
        result = h_vkBindImageMemory(dev, im, mem, 0);
        CHECK(result == VK_SUCCESS && im->tex, "B4G4R4A4 image bound");
        if (result != VK_SUCCESS || !im->tex) goto cleanup;
        mapped = NULL;
        result = h_vkMapMemory(dev, mem, 0, im->size, 0, &mapped);
        CHECK(result == VK_SUCCESS && mapped, "B4G4R4A4 memory mapped");
        if (result != VK_SUCCESS || !mapped) goto cleanup;
        memset(mapped, 0xA5, im->size);

        submit_empty();
        uint8_t got4[W * H * 4] = {0};
        readback_tex(im->tex, 4, W, H, got4);
        const uint8_t want_b4[4] = {0xAA, 0x55, 0xAA, 0x55};
        check_all_texels(got4, 4, want_b4, "all-0xA5 B4G4R4A4 texture uploads on first submit");

        h_vkDestroyImage(dev, im, NULL);
        h_vkFreeMemory(dev, mem, NULL);
        im = NULL;
        mem = NULL;

        // 5. VK_EXT_4444_formats: advertised, both formats mapped to an RGBA8 texture over 2-byte guest texels, and its feature struct answered true both in
        // the features query and at device creation (STAR WARS Battlefront Classic Collection's REI renderer only names A4R4G4B4 when the extension is listed).
        uint32_t next = 0;
        CHECK(h_vkEnumerateDeviceExtensionProperties(NULL, NULL, &next, NULL) == VK_SUCCESS && next > 0, "device extension count enumerated");
        VkExtensionProperties eps[64];
        uint32_t nep = next < 64 ? next : 64;
        h_vkEnumerateDeviceExtensionProperties(NULL, NULL, &nep, eps);
        int has4444 = 0;
        for (uint32_t i = 0; i < nep; i++) if (!strcmp(eps[i].extensionName, VK_EXT_4444_FORMATS_EXTENSION_NAME)) has4444 = 1;
        CHECK(has4444, "VK_EXT_4444_formats is advertised");
        const fmt_t *fa = fmt_of(VK_FORMAT_A4R4G4B4_UNORM_PACK16), *fb = fmt_of(VK_FORMAT_A4B4G4R4_UNORM_PACK16);
        CHECK(fa && fa->pf == MTLPixelFormatRGBA8Unorm && fa->bytes == 2 && fa->conv == CV_A4R4G4B4, "A4R4G4B4 is an RGBA8 texture over 2-byte guest texels");
        CHECK(fb && fb->pf == MTLPixelFormatRGBA8Unorm && fb->bytes == 2 && fb->conv == CV_A4B4G4R4, "A4B4G4R4 is an RGBA8 texture over 2-byte guest texels");
        VkPhysicalDevice4444FormatsFeaturesEXT f4444 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f4444};
        h_vkGetPhysicalDeviceFeatures2(NULL, &f2);
        CHECK(f4444.formatA4R4G4B4 == VK_TRUE && f4444.formatA4B4G4R4 == VK_TRUE, "vkGetPhysicalDeviceFeatures2 answers both 4444 formats supported");
        VkPhysicalDevice4444FormatsFeaturesEXT fdev = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT};
        VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &fdev};
        VkDevice dev2 = NULL;
        CHECK(h_vkCreateDevice(NULL, &dci, NULL, &dev2) == VK_SUCCESS, "vkCreateDevice with the 4444 feature struct succeeds");
        CHECK(fdev.formatA4R4G4B4 == VK_TRUE && fdev.formatA4B4G4R4 == VK_TRUE, "vkCreateDevice writes the 4444 feature booleans back true");

        // 6. A4R4G4B4 linear, a page whose every nibble differs: alpha from the high nibble, red 11:8, green 7:4, blue 3:0.
        ici.format = VK_FORMAT_A4R4G4B4_UNORM_PACK16;
        result = h_vkCreateImage(dev, &ici, NULL, &im);
        CHECK(result == VK_SUCCESS && im, "A4R4G4B4 linear image created");
        if (result != VK_SUCCESS || !im) goto cleanup;
        h_vkGetImageMemoryRequirements(dev, im, &mr);
        mai.allocationSize = mr.size;
        result = h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(result == VK_SUCCESS && mem, "A4R4G4B4 memory allocated");
        if (result != VK_SUCCESS || !mem) goto cleanup;
        result = h_vkBindImageMemory(dev, im, mem, 0);
        CHECK(result == VK_SUCCESS && im->tex, "A4R4G4B4 image bound");
        if (result != VK_SUCCESS || !im->tex) goto cleanup;
        mapped = NULL;
        result = h_vkMapMemory(dev, mem, 0, im->size, 0, &mapped);
        CHECK(result == VK_SUCCESS && mapped, "A4R4G4B4 memory mapped");
        if (result != VK_SUCCESS || !mapped) goto cleanup;
        for (uint32_t y = 0; y < H; y++) fill_page4444((uint8_t *)mapped + (size_t)y * im->pitch, W, y);

        submit_empty();
        uint8_t got6[W * H * 4];
        readback_tex(im->tex, 4, W, H, got6);
        check_page4444(got6, "A4R4G4B4 linear: alpha from the high nibble, red from 11:8, unswapped", 0);

        h_vkDestroyImage(dev, im, NULL);
        h_vkFreeMemory(dev, mem, NULL);
        im = NULL;
        mem = NULL;

        // 7. A4B4G4R4 linear, the same packed page: the format's own naming puts blue at 11:8 and red at 3:0.
        ici.format = VK_FORMAT_A4B4G4R4_UNORM_PACK16;
        result = h_vkCreateImage(dev, &ici, NULL, &im);
        CHECK(result == VK_SUCCESS && im, "A4B4G4R4 linear image created");
        if (result != VK_SUCCESS || !im) goto cleanup;
        h_vkGetImageMemoryRequirements(dev, im, &mr);
        mai.allocationSize = mr.size;
        result = h_vkAllocateMemory(dev, &mai, NULL, &mem);
        CHECK(result == VK_SUCCESS && mem, "A4B4G4R4 memory allocated");
        if (result != VK_SUCCESS || !mem) goto cleanup;
        result = h_vkBindImageMemory(dev, im, mem, 0);
        CHECK(result == VK_SUCCESS && im->tex, "A4B4G4R4 image bound");
        if (result != VK_SUCCESS || !im->tex) goto cleanup;
        mapped = NULL;
        result = h_vkMapMemory(dev, mem, 0, im->size, 0, &mapped);
        CHECK(result == VK_SUCCESS && mapped, "A4B4G4R4 memory mapped");
        if (result != VK_SUCCESS || !mapped) goto cleanup;
        for (uint32_t y = 0; y < H; y++) fill_page4444((uint8_t *)mapped + (size_t)y * im->pitch, W, y);

        submit_empty();
        uint8_t got7[W * H * 4];
        readback_tex(im->tex, 4, W, H, got7);
        check_page4444(got7, "A4B4G4R4 linear: blue from 11:8, red from 3:0", 1);

        h_vkDestroyImage(dev, im, NULL);
        h_vkFreeMemory(dev, mem, NULL);
        im = NULL;
        mem = NULL;

        // 8. The same page optimal-tiled and uploaded through vkCmdCopyBufferToImage: the C_COPYB2I branch expands the staged rows into the RGBA8 texture
        // (conv_row). An optimal-tiled converted image used to be refused at creation.
        VkCommandPool pool = NULL;
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
        CHECK(h_vkCreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS && pool, "command pool created");
        VkBufferCreateInfo sbci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 2, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VkBuffer src = NULL;
        CHECK(h_vkCreateBuffer(dev, &sbci, NULL, &src) == VK_SUCCESS && src, "staging buffer created");
        VkMemoryRequirements bmr = {0};
        h_vkGetBufferMemoryRequirements(dev, src, &bmr);
        VkDeviceMemory bmem = NULL;
        VkMemoryAllocateInfo bmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size, .memoryTypeIndex = 1};
        CHECK(h_vkAllocateMemory(dev, &bmai, NULL, &bmem) == VK_SUCCESS && bmem, "staging memory allocated");
        CHECK(h_vkBindBufferMemory(dev, src, bmem, 0) == VK_SUCCESS, "staging buffer bound");
        uint8_t *sb = (uint8_t *)mem_buf(bmem).contents + src->off;
        for (uint32_t y = 0; y < H; y++) fill_page4444(sb + (size_t)y * W * 2, W, y);

        VkImageCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                                 .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkImage oim = NULL;
        result = h_vkCreateImage(dev, &oci, NULL, &oim);
        CHECK(result == VK_SUCCESS && oim && oim->tex, "A4R4G4B4 optimal-tiled image created (a converted format is no longer linear-only)");
        if (result != VK_SUCCESS || !oim || !oim->tex) goto cleanup;
        CHECK(oim->tex.pixelFormat == MTLPixelFormatRGBA8Unorm, "its Metal texture is RGBA8");
        CHECK(oim->size == (VkDeviceSize)W * H * 2, "its subresource layout still follows the packed format (2 bytes per texel)");

        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
        VkCommandBuffer cb = NULL;
        CHECK(h_vkAllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS && cb, "command buffer allocated");
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        h_vkBeginCommandBuffer(cb, &cbi);
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
        h_vkCmdCopyBufferToImage(cb, src, oim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        h_vkEndCommandBuffer(cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        CHECK(h_vkQueueSubmit(&g_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "staged upload submitted");
        CHECK(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS, "staged upload queue idle");
        uint8_t got8[W * H * 4];
        readback_tex(oim->tex, 4, W, H, got8);
        check_page4444(got8, "an optimal-tiled A4R4G4B4 upload expands the staged nibbles through vkCmdCopyBufferToImage", 0);

        // 9. vkCmdCopyImageToBuffer of a converted format: reported, and not one byte of the 2-byte row written (it used to write 4-byte texels into it).
        VkBufferCreateInfo dbci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 2, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer dst = NULL;
        CHECK(h_vkCreateBuffer(dev, &dbci, NULL, &dst) == VK_SUCCESS && dst, "readback buffer created");
        VkMemoryRequirements dmr = {0};
        h_vkGetBufferMemoryRequirements(dev, dst, &dmr);
        VkDeviceMemory dmem = NULL;
        VkMemoryAllocateInfo dmai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = dmr.size, .memoryTypeIndex = 1};
        CHECK(h_vkAllocateMemory(dev, &dmai, NULL, &dmem) == VK_SUCCESS && dmem, "readback memory allocated");
        CHECK(h_vkBindBufferMemory(dev, dst, dmem, 0) == VK_SUCCESS, "readback buffer bound");
        uint8_t *db = (uint8_t *)mem_buf(dmem).contents + dst->off;
        memset(db, 0xEE, W * H * 2);
        h_vkBeginCommandBuffer(cb, &cbi);
        h_vkCmdCopyImageToBuffer(cb, oim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
        h_vkEndCommandBuffer(cb);
        CHECK(h_vkQueueSubmit(&g_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "refused readback submitted");
        CHECK(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS, "refused readback queue idle");
        int untouched = 1;
        for (uint32_t i = 0; i < W * H * 2; i++) if (db[i] != 0xEE) untouched = 0;
        CHECK(untouched, "readback of a converted format writes nothing into the packed row");

        h_vkDestroyImage(dev, oim, NULL);
        h_vkFreeMemory(dev, dmem, NULL);
        h_vkFreeMemory(dev, bmem, NULL);
        h_vkDestroyBuffer(dev, dst, NULL);
        h_vkDestroyBuffer(dev, src, NULL);
        h_vkDestroyCommandPool(dev, pool, NULL);

    cleanup:
        h_vkDestroyImage(dev, im, NULL);
        h_vkFreeMemory(dev, mem, NULL);
    }
    printf("Vulkan converted texture upload: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
