// The "[vk] frame" line's GPU time is the GPU time of the command buffers that completed during the frames it covers, summed because command buffers can overlap,
// so it is not the frame's wall time. Here a 256x256 colour image is cleared through the layer's submit path; once the command buffer has completed, the accumulator holds
// its time, a second submit adds to it, and the frame record resets it. This tests the layer's accounting, not Metal's clocks
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

int main(void) {
    VkDevice dev = &g_device;
    @autoreleasepool {
        dev_init();
        enum { W = 256, H = 256 };
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {W, H, 1},
                                 .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VkImage im = NULL; h_vkCreateImage(dev, &ici, NULL, &im);
        if (!im) { printf("image not created\n"); abort(); }
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        VkCommandPool cp; h_vkCreateCommandPool(dev, &cpi, NULL, &cp);
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
        VkCommandBuffer cb; h_vkAllocateCommandBuffers(dev, &cai, &cb);
        VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        h_vkBeginCommandBuffer(cb, &cbi);
        VkClearColorValue red = {.float32 = {1, 0, 0, 1}}; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        h_vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &sr);
        h_vkEndCommandBuffer(cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};

        // The completion handler adds the command buffer's GPU time, so the wait for its ticket also brings the accumulation
        atomic_store(&g_gpu_ns, 0);
        h_vkQueueSubmit(&g_queue, 1, &si, NULL);
        h_vkQueueWaitIdle(&g_queue);
        uint64_t ns = atomic_load(&g_gpu_ns);
        CHECK(ns > 0, "a completed command buffer's GPU time is accumulated");
        if (!ns) fprintf(stderr, "  nothing accumulated for the submitted clear\n");
        // A second command buffer adds its own time
        h_vkQueueSubmit(&g_queue, 1, &si, NULL);
        h_vkQueueWaitIdle(&g_queue);
        CHECK(atomic_load(&g_gpu_ns) > ns, "the times of several command buffers add up");
        frame_end();   // the frame record prints the line with the GPU time just measured, and starts the counter over
        CHECK(atomic_load(&g_gpu_ns) == 0, "the frame record resets the GPU time");

        h_vkDestroyImage(dev, im, NULL);
    }
    printf("Vulkan GPU time: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
