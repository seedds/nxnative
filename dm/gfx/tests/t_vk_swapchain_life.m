// Swap chain images outlive their swap chain only until the presents that showed them are done: vkDestroySwapchainKHR releases them (mtl_ext_tex_release: their serial,
// the residency set, the present path's references), and their presents' pins leave nothing on the deferred-release queue, which no NVN submission drains in a Vulkan game.
// Before, every recreation stranded its images, and every present left a queue entry holding its image for the rest of the session (140 recreations: 420 of 420 alive).
// The presents go to an offscreen layer, so they are committed to the GPU and complete there before their images go.
#include "../vk/hle_vk.m"
#import <QuartzCore/CAMetalLayer.h>
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
static CAMetalLayer *g_test_layer;
void *front_metal_layer(void) { return (__bridge void *)g_test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
int dm_opmode(void) { return 1; }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum { CYCLES = 140, FRAMES = 3, IMAGES = 3 };   // more cycles than the present path's window-texture list holds (128)

static void one_cycle(__weak id<MTLTexture> *weak) {
    @autoreleasepool {
        VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .minImageCount = IMAGES, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
                                       .imageExtent = {64, 32}, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .presentMode = VK_PRESENT_MODE_FIFO_KHR};
        VkSwapchainKHR sc; h_vkCreateSwapchainKHR(&g_device, &ci, NULL, &sc);
        for (uint32_t i = 0; i < sc->n; i++) weak[i] = sc->img[i]->tex;
        for (int f = 0; f < FRAMES; f++) {
            uint32_t k; h_vkAcquireNextImageKHR(&g_device, sc, UINT64_MAX, NULL, NULL, &k);
            VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &k};
            h_vkQueuePresentKHR(NULL, &pi);
        }
        h_vkDestroySwapchainKHR(&g_device, sc, NULL);
    }
}

static int alive(__weak id<MTLTexture> *weak, int n) { int k = 0; @autoreleasepool { for (int i = 0; i < n; i++) k += weak[i] != nil; } return k; }

int main(void) {
    static __weak id<MTLTexture> weak[CYCLES][4];
    @autoreleasepool {
        dev_init();
        g_test_layer = [CAMetalLayer layer]; g_test_layer.device = (__bridge id<MTLDevice>)mtl_device(); g_test_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_test_layer.framebufferOnly = NO; g_test_layer.drawableSize = CGSizeMake(64, 32);
    }
    for (int c = 0; c < CYCLES; c++) one_cycle(weak[c]);
    int left = 0;
    for (int tries = 0; tries < 200; tries++) {   // up to 2 s for the presents' completions and the releases behind them
        left = 0; for (int c = 0; c < CYCLES; c++) left += alive(weak[c], IMAGES);
        if (!left) break;
        usleep(10 * 1000);
    }
    CHECK(left == 0, "%d of %d swap chain images are still alive after their swap chains were destroyed", left, CYCLES * IMAGES);
    printf("t_vk_swapchain_life: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
