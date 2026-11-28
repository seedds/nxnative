// Buffer→image uploads and the staging row pitch (hle_vk.m C_COPYB2I; docs/reference/rendering.md, "Vulkan upload rows with a padded staging pitch"): a
// Switch staging buffer spaces rows of a width that is not already a multiple of 256 bytes align(bpr, 256) apart and the game still declares
// bufferRowLength = 0, so the layer believes the VkBuffer's own size when that size is exactly a padded total — layout A, apitch*(rows-1)+bpr bytes per
// slice with the last row tight, layout B, apitch*rows bytes per slice with every row padded, each times the copy's layer count (a 3D image's extent
// depth). Each case fills a synthetic staging buffer in one of those layouts and reads the image back through the layer's own image→buffer copy: a wrong
// pitch or slice stride reads another row's or slice's texels, which the pattern (x, y, slice) makes visible.
//   (a) a tight buffer: rows and total tight, which matches neither identity, so the declared pitch is kept
//   (b) layout A, the last row of the slice tight
//   (c) layout B, every row padded
//   (d) four array layers of layout A, so the slice stride is multiplied by the layer count
//   (e) a 32x32x32 volume in 262144 B = 256*32*32, layout B — the case that motivated the rule: 128-byte rows of a 32-cubed light volume, which a tight
//       read takes from the wrong place in every row but the first, so the volume shears
//   (f) a buffer larger than either padded total, which proves nothing: tight, or padded when the profile's tex_row_pad key is on
// The profile key is a per-game build-time switch, so the Makefile's check rule builds this source twice (P3_ROW_PAD 0 and 1) and (a)-(e) must pass
// both ways: the two proved layouts need no key.
#include "../../host/dm.h"
#ifndef P3_ROW_PAD
#define P3_ROW_PAD 0
#endif
#undef DM_GAME_TEX_ROW_PAD
#define DM_GAME_TEX_ROW_PAD P3_ROW_PAD
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

static VkDevice dev; static VkCommandPool pool; static VkDeviceMemory mem; static VkDeviceSize mem_top;
static VkBuffer new_buf(VkDeviceSize size) {   // host-visible memory, from the test's one allocation
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; CHECK(h_vkCreateBuffer(dev, &ci, NULL, &b) == VK_SUCCESS, "buffer of %llu bytes created", (unsigned long long)size);
    CHECK(h_vkBindBufferMemory(dev, b, mem, mem_top) == VK_SUCCESS, "buffer of %llu bytes bound", (unsigned long long)size);
    mem_top += (size + 255) & ~(VkDeviceSize)255;
    return b;
}
static uint8_t *buf_bytes(VkBuffer b) { return (uint8_t *)mem_buf(b->mem).contents + b->off; }
static VkCommandBuffer new_cb(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    h_vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void run(VkCommandBuffer cb) {   // submit and wait for the queue
    h_vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    h_vkQueueSubmit(&g_queue, 1, &si, VK_NULL_HANDLE); h_vkQueueWaitIdle(&g_queue);
}
static VkImage new_img(uint32_t w, uint32_t h, uint32_t d, VkImageType type, uint32_t layers, VkFormat fmt) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = type, .format = fmt, .extent = {w, h, d}, .mipLevels = 1,
                            .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage im = NULL; VkResult r = h_vkCreateImage(dev, &ci, NULL, &im);
    CHECK(r == VK_SUCCESS && im && im->tex, "image %ux%ux%u type %d layers %u created", w, h, d, (int)type, layers);
    return im;
}

// One upload of RGBA8 texels, and a texel-by-texel readback. `src_pitch` and `src_slice` are the staging layout the copy must find: the rows and slices
// are filled tight for the pitch the layer ought to choose, so a read at the wrong pitch or slice stride lands on another row's texels or on the 0x7F
// poison in the padding.
static void upload_case(const char *label, uint32_t w, uint32_t h, uint32_t d, VkImageType type, uint32_t layers, VkDeviceSize bufsize,
                        uint32_t src_pitch, VkDeviceSize src_slice) {
    int is3d = type == VK_IMAGE_TYPE_3D;
    uint32_t slices = is3d ? d : layers, rows = h;
    VkImage im = new_img(w, h, d, type, layers, is3d ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);
    if (!im || !im->tex) return;

    VkBuffer src = new_buf(bufsize);
    uint8_t *p = buf_bytes(src); memset(p, 0x7F, (size_t)bufsize);   // padding poison, and the bytes past any proved total
    for (uint32_t s = 0; s < slices; s++)
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *q = p + s * src_slice + (VkDeviceSize)y * src_pitch + (VkDeviceSize)x * 4;
                q[0] = (uint8_t)x; q[1] = (uint8_t)y; q[2] = (uint8_t)s; q[3] = 0xA5;
            }

    VkBufferImageCopy r = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, is3d ? 1u : layers}, .imageExtent = {w, h, d}};
    VkCommandBuffer cb = new_cb(); h_vkCmdCopyBufferToImage(cb, src, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r); run(cb);

    VkDeviceSize tight_slice = (VkDeviceSize)w * h * 4;
    VkBuffer back = new_buf(tight_slice * slices);
    for (uint32_t s = 0; s < slices; s++) {   // one layer (or one depth slice) at a time, tight rows, placed at its own offset
        VkBufferImageCopy rr = {.bufferOffset = s * tight_slice, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, is3d ? 0u : s, 1},
                                .imageOffset = {0, 0, is3d ? (int32_t)s : 0}, .imageExtent = {w, h, 1}};
        cb = new_cb(); h_vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back, 1, &rr); run(cb);
    }

    const uint8_t *got = buf_bytes(back);
    long bad = -1;
    for (uint32_t s = 0; s < slices && bad < 0; s++)
        for (uint32_t y = 0; y < rows && bad < 0; y++)
            for (uint32_t x = 0; x < w && bad < 0; x++) {
                const uint8_t *q = got + s * tight_slice + (VkDeviceSize)(y * w + x) * 4;
                if (q[0] != (uint8_t)x || q[1] != (uint8_t)y || q[2] != (uint8_t)s || q[3] != 0xA5) bad = (long)((s * rows + y) * w + x);
            }
    checks++;
    if (bad < 0) return;
    fails++;
    const uint8_t *q = got + (size_t)bad * 4;
    fprintf(stderr, "FAIL: %s: texel %ld (x %ld, y %ld, slice %ld) got %02x %02x %02x %02x, want %02x %02x %02x a5\n", label, bad, bad % w,
            (bad / w) % rows, bad / (long)(w * rows), q[0], q[1], q[2], q[3], (unsigned)(bad % w), (unsigned)((bad / w) % rows), (unsigned)(bad / (long)(w * rows)));
}

int main(void) {
    dev = &g_device;
    @autoreleasepool {
        dev_init();
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; h_vkCreateCommandPool(dev, &pci, NULL, &pool);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = 1 << 21, .memoryTypeIndex = 1};
        CHECK(h_vkAllocateMemory(dev, &mai, NULL, &mem) == VK_SUCCESS && mem, "2 MB of host-visible device memory allocated");

        // 100 wide RGBA8: bpr 400, apitch 512, eight rows. Layout A is 512*7+400 = 3984 B a slice, layout B 512*8 = 4096 B, a tight slice 3200 B.
        enum { W = 100, H = 8, LAYERS = 4, APITCH = 512, BPR = 400, NEED_A = APITCH * (H - 1) + BPR, FULL_B = APITCH * H, TIGHT = BPR * H };

        // (a) a tight buffer: the total proves neither layout, so the rows are read at the declared 400 bytes
        upload_case("(a) tight buffer, tight total: tight rows", W, H, 1, VK_IMAGE_TYPE_2D, 1, TIGHT, BPR, TIGHT);
        // (b) layout A: the buffer is exactly one padded slice with a tight last row
        upload_case("(b) layout A (apitch*(rows-1)+bpr): padded rows, last row tight", W, H, 1, VK_IMAGE_TYPE_2D, 1, NEED_A, APITCH, NEED_A);
        // (c) layout B: the buffer is exactly one fully padded slice
        upload_case("(c) layout B (apitch*rows): every row padded", W, H, 1, VK_IMAGE_TYPE_2D, 1, FULL_B, APITCH, FULL_B);
        // (d) layout A times four array layers: the slice stride is the padded slice, not the tight one
        upload_case("(d) layout A, four array layers: slice stride x4", W, H, 1, VK_IMAGE_TYPE_2D, LAYERS, NEED_A * LAYERS, APITCH, NEED_A);
        // (e) 32x32x32 BGRA8, 128-byte rows: 256*32*32 = 262144 B, layout B times the volume's 32 depth slices
        upload_case("(e) 32x32x32 volume in 262144 B = 256*32*32: layout B x32", 32, 32, 32, VK_IMAGE_TYPE_3D, 1, 262144, 256, 256 * 32);
        // (f) 65536 B, larger than either padded total (3984, 4096) and than a tight copy (3200): proves nothing. The key, when the build has it on,
        // reads it padded anyway; without it the declared pitch is kept.
        upload_case(P3_ROW_PAD ? "(f) oversized buffer, tex_row_pad on: padded rows" : "(f) oversized buffer: tight rows", W, H, 1, VK_IMAGE_TYPE_2D, 1,
                    65536, P3_ROW_PAD ? APITCH : BPR, P3_ROW_PAD ? NEED_A : TIGHT);

        h_vkFreeMemory(dev, mem, NULL);
    }
    printf("Vulkan buffer→image staging pitch (tex_row_pad %s): %d checks, %d failed\n", P3_ROW_PAD ? "on" : "off", checks, fails);
    return fails != 0;
}
