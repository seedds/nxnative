// Converted linear images (fmt_t.conv: R4G4, B4G4R4A4, B5G6R5, B5G5R5A1, A4R4G4B4, A4B4G4R4) are unpacked on the CPU before each vkQueueSubmit (conv_sync).
// The first upload must run unconditionally rather than comparing against an in-band sentinel in im->shadow (0xA5 used to skip an all-0xA5
// input on the first submit, leaving the Metal texture zeroed). Byte comparison against im->shadow activates only after the first upload.
// VK_EXT_4444_formats and GPU compute buffer <-> image conversion (conv_b2i / conv_i2b) are covered end to end: extension advertisement, feature query,
// non-mutating const vkCreateDevice pNext chain, format properties (TRANSFER_SRC_BIT advertised, UNIFORM_TEXEL_BUFFER_BIT withheld), linear and optimal
// A4R4G4B4/A4B4G4R4 per-channel verification, round-trip B2I -> I2B for all six converted formats, same-submit GPU-written staging, sub-rects/strides/arrays/3D.
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

// A 16x16 VK_EXT_4444_formats page whose nibbles distinguish every channel: A = 0xF, 11:8 = 15-x, 7:4 = y, 3:0 = x^y.
// Read as A4R4G4B4: R = 15-x, G = y, B = x^y, A = 0xF. Read as A4B4G4R4: R = x^y, G = y, B = 15-x, A = 0xF.
static uint16_t page4444(uint32_t x, uint32_t y) { return (uint16_t)(0xF000u | (uint32_t)(15 - x) << 8 | y << 4 | ((x ^ y) & 15)); }
static void fill_page4444(uint8_t *row, uint32_t w, uint32_t y) {
    for (uint32_t x = 0; x < w; x++) { uint16_t v = page4444(x, y); row[2 * x] = (uint8_t)v; row[2 * x + 1] = (uint8_t)(v >> 8); }
}
static void want_page4444(uint8_t *out, uint32_t x, uint32_t y, int swap_rb) {
    uint8_t hi = (uint8_t)x4(15 - x), lo = (uint8_t)x4(x ^ y);
    out[0] = swap_rb ? lo : hi; out[1] = (uint8_t)x4(y); out[2] = swap_rb ? hi : lo; out[3] = (uint8_t)x4(15);
}
static void check_page4444(const uint8_t *got, const char *label, int swap_rb) {
    for (uint32_t y = 0, i = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++, i++) {
            uint8_t want[4]; want_page4444(want, x, y, swap_rb);
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

static void submit_cb(VkCommandBuffer cb, const char *label) {
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(h_vkQueueSubmit(&g_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "%s submitted", label);
    CHECK(h_vkQueueWaitIdle(&g_queue) == VK_SUCCESS, "%s queue idle", label);
}

static VkBuffer make_host_buf(VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *mem_out, uint8_t **ptr_out) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VkBuffer buf = NULL;
    if (h_vkCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS || !buf) return NULL;
    VkMemoryRequirements mr = {0}; h_vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = 1};
    VkDeviceMemory mem = NULL;
    if (h_vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS || !mem) { h_vkDestroyBuffer(dev, buf, NULL); return NULL; }
    if (h_vkBindBufferMemory(dev, buf, mem, 0) != VK_SUCCESS) { h_vkFreeMemory(dev, mem, NULL); h_vkDestroyBuffer(dev, buf, NULL); return NULL; }
    *mem_out = mem;
    *ptr_out = (uint8_t *)mem_buf(mem).contents + buf->off;
    return buf;
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

        // 5. VK_EXT_4444_formats: advertised, mapped to RGBA8Unorm over 2-byte guest texels, feature query true (alongside EDS),
        // vkCreateDevice does not mutate const pNext, and format properties advertise TRANSFER_SRC/DST but withhold UNIFORM_TEXEL_BUFFER.
        uint32_t next = 0;
        CHECK(h_vkEnumerateDeviceExtensionProperties(NULL, NULL, &next, NULL) == VK_SUCCESS && next > 0, "device extension count enumerated");
        VkExtensionProperties eps[64];
        uint32_t nep = next < 64 ? next : 64;
        h_vkEnumerateDeviceExtensionProperties(NULL, NULL, &nep, eps);
        int has4444 = 0;
        for (uint32_t i = 0; i < nep; i++) if (!strcmp(eps[i].extensionName, VK_EXT_4444_FORMATS_EXTENSION_NAME)) has4444 = 1;
        CHECK(has4444, "VK_EXT_4444_formats is advertised");
        const fmt_t *fa = fmt_of(VK_FORMAT_A4R4G4B4_UNORM_PACK16), *fb = fmt_of(VK_FORMAT_A4B4G4R4_UNORM_PACK16);
        CHECK(fa && fa->pf == MTLPixelFormatRGBA8Unorm && fa->bytes == 2 && fa->conv == CV_A4R4G4B4, "A4R4G4B4 maps to RGBA8 over 2-byte guest texels");
        CHECK(fb && fb->pf == MTLPixelFormatRGBA8Unorm && fb->bytes == 2 && fb->conv == CV_A4B4G4R4, "A4B4G4R4 maps to RGBA8 over 2-byte guest texels");

        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDevice4444FormatsFeaturesEXT f4444 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, .pNext = &feds};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f4444};
        h_vkGetPhysicalDeviceFeatures2(NULL, &f2);
        CHECK(f4444.formatA4R4G4B4 == VK_TRUE && f4444.formatA4B4G4R4 == VK_TRUE, "vkGetPhysicalDeviceFeatures2 answers both 4444 formats supported");
        CHECK(feds.extendedDynamicState == VK_TRUE, "vkGetPhysicalDeviceFeatures2 preserves chained extendedDynamicState query");

        static const VkPhysicalDevice4444FormatsFeaturesEXT const_fdev = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, .formatA4R4G4B4 = VK_FALSE, .formatA4B4G4R4 = VK_FALSE,
        };
        VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &const_fdev};
        VkDevice dev2 = NULL;
        CHECK(h_vkCreateDevice(NULL, &dci, NULL, &dev2) == VK_SUCCESS, "vkCreateDevice accepts a const 4444 feature struct without faulting");
        CHECK(const_fdev.formatA4R4G4B4 == VK_FALSE && const_fdev.formatA4B4G4R4 == VK_FALSE, "vkCreateDevice does not mutate const pNext structs");

        const VkFormat prop_fmts[] = {VK_FORMAT_A4R4G4B4_UNORM_PACK16, VK_FORMAT_A4B4G4R4_UNORM_PACK16, VK_FORMAT_R4G4_UNORM_PACK8};
        const VkFormatFeatureFlags want_opt = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        for (size_t i = 0; i < sizeof prop_fmts / sizeof *prop_fmts; i++) {
            VkFormatProperties fp = {0};
            h_vkGetPhysicalDeviceFormatProperties(NULL, prop_fmts[i], &fp);
            CHECK((fp.optimalTilingFeatures & want_opt) == want_opt, "format %d advertises mandatory sampled/blit/transfer features", (int)prop_fmts[i]);
            CHECK((fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) == 0, "converted format %d does not advertise UNIFORM_TEXEL_BUFFER_BIT", (int)prop_fmts[i]);
        }

        // 6. Linear A4R4G4B4 and A4B4G4R4: same packed page reads back with swapped R/B and alpha from bits 15:12.
        const VkFormat lin_fmts[2] = {VK_FORMAT_A4R4G4B4_UNORM_PACK16, VK_FORMAT_A4B4G4R4_UNORM_PACK16};
        for (int k = 0; k < 2; k++) {
            ici.format = lin_fmts[k];
            result = h_vkCreateImage(dev, &ici, NULL, &im);
            CHECK(result == VK_SUCCESS && im, "4444 linear image (%d) created", k);
            if (result != VK_SUCCESS || !im) goto cleanup;
            h_vkGetImageMemoryRequirements(dev, im, &mr);
            mai.allocationSize = mr.size;
            result = h_vkAllocateMemory(dev, &mai, NULL, &mem);
            CHECK(result == VK_SUCCESS && mem, "4444 linear memory (%d) allocated", k);
            if (result != VK_SUCCESS || !mem) goto cleanup;
            result = h_vkBindImageMemory(dev, im, mem, 0);
            CHECK(result == VK_SUCCESS && im->tex, "4444 linear image (%d) bound", k);
            if (result != VK_SUCCESS || !im->tex) goto cleanup;
            mapped = NULL;
            result = h_vkMapMemory(dev, mem, 0, im->size, 0, &mapped);
            CHECK(result == VK_SUCCESS && mapped, "4444 linear memory (%d) mapped", k);
            if (result != VK_SUCCESS || !mapped) goto cleanup;
            for (uint32_t y = 0; y < H; y++) fill_page4444((uint8_t *)mapped + (size_t)y * im->pitch, W, y);
            submit_empty();
            uint8_t got_lin[W * H * 4];
            readback_tex(im->tex, 4, W, H, got_lin);
            check_page4444(got_lin, k ? "A4B4G4R4 linear: blue 11:8, red 3:0" : "A4R4G4B4 linear: alpha 15:12, red 11:8", k);
            h_vkDestroyImage(dev, im, NULL); h_vkFreeMemory(dev, mem, NULL); im = NULL; mem = NULL;
        }

        // 7. Optimal-tiled A4R4G4B4 and A4B4G4R4: GPU compute B2I unpack (checked directly against RGBA8 texture) and I2B pack round-trip.
        VkCommandPool pool = NULL;
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
        CHECK(h_vkCreateCommandPool(dev, &pci, NULL, &pool) == VK_SUCCESS && pool, "command pool created");
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer cb = NULL;
        CHECK(h_vkAllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS && cb, "command buffer allocated");
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

        VkDeviceMemory smem = NULL, dmem = NULL, gmem = NULL;
        uint8_t *sb = NULL, *db = NULL, *gb = NULL;
        VkBuffer src = make_host_buf(dev, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &smem, &sb);
        VkBuffer dst = make_host_buf(dev, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dmem, &db);
        VkBuffer gpu_mid = make_host_buf(dev, 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &gmem, &gb);
        CHECK(src && dst && gpu_mid, "transfer test buffers created");
        if (!src || !dst || !gpu_mid) goto cleanup;

        for (int k = 0; k < 2; k++) {
            for (uint32_t y = 0; y < H; y++) fill_page4444(sb + (size_t)y * W * 2, W, y);
            VkImageCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = lin_fmts[k],
                                     .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage oim = NULL;
            result = h_vkCreateImage(dev, &oci, NULL, &oim);
            CHECK(result == VK_SUCCESS && oim && oim->tex, "optimal 4444 image (%d) created", k);
            if (result != VK_SUCCESS || !oim || !oim->tex) goto cleanup_pool;
            CHECK(oim->tex.pixelFormat == MTLPixelFormatRGBA8Unorm, "optimal 4444 (%d) backed by RGBA8Unorm", k);
            CHECK(oim->size == (VkDeviceSize)W * H * 2, "optimal 4444 (%d) memory layout stays 2 bytes/texel", k);

            memset(db, 0xEE, W * H * 2);
            VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
            h_vkBeginCommandBuffer(cb, &cbi);
            h_vkCmdCopyBufferToImage(cb, src, oim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            h_vkCmdCopyImageToBuffer(cb, oim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
            h_vkEndCommandBuffer(cb);
            submit_cb(cb, k ? "A4B4G4R4 optimal B2I+I2B" : "A4R4G4B4 optimal B2I+I2B");

            uint8_t got_opt[W * H * 4];
            readback_tex(oim->tex, 4, W, H, got_opt);
            check_page4444(got_opt, k ? "A4B4G4R4 optimal B2I unpacks per channel" : "A4R4G4B4 optimal B2I unpacks per channel", k);
            CHECK(memcmp(db, sb, W * H * 2) == 0, "optimal 4444 (%d) I2B round-trip matches packed source bytes", k);
            h_vkDestroyImage(dev, oim, NULL);
        }

        // 8. Round-trip B2I -> I2B for all four pre-existing converted formats (R4G4, B4G4R4A4, B5G6R5, B5G5R5A1).
        const VkFormat other_fmts[] = {VK_FORMAT_R4G4_UNORM_PACK8, VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_B5G5R5A1_UNORM_PACK16};
        for (size_t k = 0; k < sizeof other_fmts / sizeof *other_fmts; k++) {
            const fmt_t *f = fmt_of(other_fmts[k]);
            size_t total = (size_t)W * H * f->bytes;
            for (size_t i = 0; i < total; i++) sb[i] = (uint8_t)((i * 73u + k * 29u + 17u) & 0xFFu);
            memset(db, 0xCC, total);
            VkImageCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = other_fmts[k],
                                     .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage oim = NULL;
            CHECK(h_vkCreateImage(dev, &oci, NULL, &oim) == VK_SUCCESS && oim && oim->tex, "optimal conv format %d created", (int)other_fmts[k]);
            VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
            h_vkBeginCommandBuffer(cb, &cbi);
            h_vkCmdCopyBufferToImage(cb, src, oim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            h_vkCmdCopyImageToBuffer(cb, oim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
            h_vkEndCommandBuffer(cb);
            submit_cb(cb, "other conv format round-trip");
            CHECK(memcmp(db, sb, total) == 0, "conv format %d B2I -> I2B round-trips bit-exact", (int)other_fmts[k]);
            h_vkDestroyImage(dev, oim, NULL);
        }

        // 9. Same-submit GPU-written staging buffer (vkCmdCopyBuffer -> vkCmdCopyBufferToImage -> vkCmdCopyImageToBuffer):
        // gpu_mid starts zeroed on CPU, receives packed A4R4G4B4 bytes on GPU, then uploads to image in the same command buffer.
        {
            for (uint32_t y = 0; y < H; y++) fill_page4444(sb + (size_t)y * W * 2, W, y);
            memset(gb, 0x00, W * H * 2);
            memset(db, 0xDD, W * H * 2);
            VkImageCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                                     .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage oim = NULL;
            CHECK(h_vkCreateImage(dev, &oci, NULL, &oim) == VK_SUCCESS && oim, "GPU-staged A4R4G4B4 image created");
            VkBufferCopy bc = {.srcOffset = 0, .dstOffset = 0, .size = W * H * 2};
            VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
            h_vkBeginCommandBuffer(cb, &cbi);
            h_vkCmdCopyBuffer(cb, src, gpu_mid, 1, &bc);
            h_vkCmdCopyBufferToImage(cb, gpu_mid, oim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            h_vkCmdCopyImageToBuffer(cb, oim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
            h_vkEndCommandBuffer(cb);
            submit_cb(cb, "same-submit GPU-staged A4R4G4B4");
            CHECK(memcmp(db, sb, W * H * 2) == 0, "same-submit GPU-written staging buffer uploads in submission order");
            h_vkDestroyImage(dev, oim, NULL);
        }

        // 10. Non-zero odd/2-byte bufferOffset, padded bufferRowLength/bufferImageHeight, sub-rect imageOffset, 2D array (2 layers), and 3D volume (4x4x3),
        // plus zero-extent no-op and out-of-bounds guard.
        {
            // 2D array: 8x8, 2 layers; copy 5x4 sub-rect at (2,3) with bufferOffset=6, bufferRowLength=7, bufferImageHeight=5.
            enum { AW = 8, AH = 8, AL = 2, RW = 5, RH = 4, RL = 7, IH = 5, OFF = 6 };
            size_t bpr = RL * 2, bpi = bpr * IH;
            memset(sb, 0xAA, 4096);
            memset(db, 0x55, 4096);
            for (uint32_t l = 0; l < AL; l++)
                for (uint32_t y = 0; y < RH; y++)
                    for (uint32_t x = 0; x < RW; x++) {
                        uint16_t v = (uint16_t)((((l * 5 + 3) & 15) << 12) | (((x + l) & 15) << 8) | (((y + 2) & 15) << 4) | (((x ^ y ^ l) & 15)));
                        size_t o = OFF + l * bpi + y * bpr + x * 2;
                        sb[o] = (uint8_t)v; sb[o + 1] = (uint8_t)(v >> 8);
                    }
            VkImageCreateInfo aci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                                     .extent = {AW, AH, 1}, .mipLevels = 1, .arrayLayers = AL, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage aim = NULL;
            CHECK(h_vkCreateImage(dev, &aci, NULL, &aim) == VK_SUCCESS && aim, "2D array A4R4G4B4 image created");
            VkBufferImageCopy r = {.bufferOffset = OFF, .bufferRowLength = RL, .bufferImageHeight = IH,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, AL}, .imageOffset = {2, 3, 0}, .imageExtent = {RW, RH, 1}};
            VkBufferImageCopy r_zero = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {0, 0, 1}};
            VkBufferImageCopy r_oob = {.bufferOffset = 4090, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {RW, RH, 1}};
            h_vkBeginCommandBuffer(cb, &cbi);
            h_vkCmdCopyBufferToImage(cb, src, aim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r_zero);
            h_vkCmdCopyBufferToImage(cb, src, aim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            h_vkCmdCopyImageToBuffer(cb, aim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
            h_vkCmdCopyImageToBuffer(cb, aim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r_oob);
            h_vkEndCommandBuffer(cb);
            submit_cb(cb, "2D array sub-rect + stride round-trip");
            int ok = 1;
            for (uint32_t l = 0; l < AL && ok; l++)
                for (uint32_t y = 0; y < RH && ok; y++) {
                    size_t o = OFF + l * bpi + y * bpr;
                    if (memcmp(db + o, sb + o, RW * 2) != 0) ok = 0;
                    // Padding bytes after each row must stay untouched (0x55).
                    for (size_t p = RW * 2; p < bpr; p++) if (db[o + p] != 0x55) ok = 0;
                }
            for (size_t p = 0; p < OFF; p++) if (db[p] != 0x55) ok = 0;
            for (size_t p = 4090; p < 4096; p++) if (db[p] != 0x55) ok = 0;
            CHECK(ok, "2D array sub-rect/stride round-trip preserves exact bytes and leaves padding/OOB untouched");
            h_vkDestroyImage(dev, aim, NULL);

            // 3D volume: 4x4x3 A4B4G4R4 round-trip.
            enum { VW = 4, VH = 4, VD = 3 };
            size_t vbytes = VW * VH * VD * 2;
            for (size_t i = 0; i < vbytes; i++) sb[i] = (uint8_t)((i * 53u + 11u) & 0xFFu);
            memset(db, 0x66, vbytes);
            VkImageCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D, .format = VK_FORMAT_A4B4G4R4_UNORM_PACK16,
                                     .extent = {VW, VH, VD}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage vim = NULL;
            CHECK(h_vkCreateImage(dev, &vci, NULL, &vim) == VK_SUCCESS && vim, "3D volume A4B4G4R4 image created");
            VkBufferImageCopy vr = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {VW, VH, VD}};
            h_vkBeginCommandBuffer(cb, &cbi);
            h_vkCmdCopyBufferToImage(cb, src, vim, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &vr);
            h_vkCmdCopyImageToBuffer(cb, vim, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &vr);
            h_vkEndCommandBuffer(cb);
            submit_cb(cb, "3D volume A4B4G4R4 round-trip");
            CHECK(memcmp(db, sb, vbytes) == 0, "3D volume A4B4G4R4 B2I -> I2B round-trips bit-exact");
            h_vkDestroyImage(dev, vim, NULL);
        }

    cleanup_pool:
        h_vkFreeMemory(dev, gmem, NULL);
        h_vkFreeMemory(dev, dmem, NULL);
        h_vkFreeMemory(dev, smem, NULL);
        h_vkDestroyBuffer(dev, gpu_mid, NULL);
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
