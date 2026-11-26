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
static _Atomic int g_vk_frame_ticks;
void dm_guest_frame_tick(void) { atomic_fetch_add(&g_vk_frame_ticks, 1); }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum { CYCLES = 140, FRAMES = 3, IMAGES = 3 };   // more cycles than the present path's window-texture list holds (128)

static void one_cycle(__weak id<MTLTexture> *weak) {
    @autoreleasepool {
        VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .minImageCount = IMAGES, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
                                       .imageExtent = {64, 32}, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                       .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR};
        VkSwapchainKHR sc = NULL;
        VkResult result = h_vkCreateSwapchainKHR(&g_device, &ci, NULL, &sc);
        CHECK(result == VK_SUCCESS && sc, "swap chain creation failed: %d", result);
        if (result != VK_SUCCESS || !sc) return;
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

// Controllable virtual clock, sleep, display-link and ticket hooks for deterministic ownership/timeout tests
static uint64_t g_fake_now, g_fake_done_tk, g_fake_ready_at, g_fake_sleep_total, g_fake_wait_last_tmo;
static int g_fake_sleep_calls, g_fake_wait_calls, g_fake_link_on, g_fake_link_succeed;
static uint64_t fake_now(void) { return g_fake_now; }
static void fake_sleep(uint64_t ns) { g_fake_sleep_calls++; g_fake_sleep_total += ns; g_fake_now += ns; }
static int fake_ready(uint64_t tk) { return !tk || tk <= g_fake_done_tk || (g_fake_ready_at && g_fake_now >= g_fake_ready_at); }
static int fake_wait(uint64_t tk, uint64_t timeout_ns) {
    g_fake_wait_calls++;
    g_fake_wait_last_tmo = timeout_ns;
    if (fake_ready(tk)) return 1;
    if (g_fake_ready_at && g_fake_ready_at > g_fake_now) {
        uint64_t need = g_fake_ready_at - g_fake_now;
        if (timeout_ns == UINT64_MAX || need <= timeout_ns) { g_fake_now = g_fake_ready_at; g_fake_done_tk = tk; return 1; }
    }
    if (timeout_ns != UINT64_MAX) g_fake_now += timeout_ns;
    return 0;
}
static int fake_link_active(void) { return g_fake_link_on; }
static int fake_link_wait(int vsyncs, uint64_t timeout_ns) {
    (void)vsyncs;
    g_fake_now += timeout_ns;
    return g_fake_link_succeed;
}
static void hooks_install(void) {
    g_fake_now = 1000000000ull; g_fake_done_tk = 0; g_fake_ready_at = 0;
    g_fake_sleep_total = 0; g_fake_wait_last_tmo = 0; g_fake_sleep_calls = 0; g_fake_wait_calls = 0;
    g_fake_link_on = 0; g_fake_link_succeed = 0; g_next_acq = 0;
    g_vk_now_ns = fake_now; g_vk_sleep_ns = fake_sleep; g_vk_ticket_ready = fake_ready; g_vk_ticket_wait = fake_wait;
    g_vk_link_active = fake_link_active; g_vk_link_wait_tick = fake_link_wait;
}
static void hooks_restore(void) {
    g_next_acq = 0;
    g_vk_now_ns = vk_now_default; g_vk_sleep_ns = vk_sleep_default; g_vk_ticket_ready = vk_ticket_ready_default;
    g_vk_ticket_wait = vk_ticket_wait_default; g_vk_link_active = mtl_link_active; g_vk_link_wait_tick = mtl_link_wait_tick;
}

typedef struct { VkSwapchainKHR sc; uint32_t idx; } delayed_present_arg;
static void *delayed_present_thread(void *p) {
    delayed_present_arg *a = (delayed_present_arg *)p;
    usleep(5 * 1000);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &a->sc, .pImageIndices = &a->idx};
    h_vkQueuePresentKHR(NULL, &pi);
    return NULL;
}

static void test_ownership_and_timeouts(void) {
    @autoreleasepool {
        VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .minImageCount = IMAGES, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
                                       .imageExtent = {64, 32}, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                       .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR};
        VkSwapchainKHR sc = NULL;
        CHECK(h_vkCreateSwapchainKHR(&g_device, &ci, NULL, &sc) == VK_SUCCESS && sc && sc->n == 3, "create 3-image swapchain");
        VkFence f = NULL;
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK(h_vkCreateFence(&g_device, &fci, NULL, &f) == VK_SUCCESS && f, "create unsignaled fence");

        hooks_install();
        // 1. Hold image 0 while repeatedly acquiring and presenting the other two: held index 0 is never returned
        uint32_t held0 = 99;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 0, NULL, f, &held0) == VK_SUCCESS && held0 == 0, "zero-timeout acquires ready image 0 without sleeping");
        CHECK(g_fake_sleep_calls == 0 && g_fake_wait_calls == 0 && atomic_load(&f->val) == 0, "zero-timeout ready acquire did not sleep and signaled fence");
        int seen_held = 0, seen1 = 0, seen2 = 0;
        for (int iter = 0; iter < 12; iter++) {
            uint32_t k = 99;
            VkResult r = h_vkAcquireNextImageKHR(&g_device, sc, UINT64_MAX, NULL, NULL, &k);
            CHECK(r == VK_SUCCESS && k < sc->n, "blocking acquire succeeds while image 0 is held");
            if (k == held0) seen_held++;
            if (k == 1) seen1++;
            if (k == 2) seen2++;
            VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &k};
            h_vkQueuePresentKHR(NULL, &pi);
            g_fake_done_tk = sc->ticket[k];
        }
        CHECK(seen_held == 0 && seen1 > 0 && seen2 > 0, "held image 0 was never returned while acquiring/presenting the other two (%d, %d, %d)", seen_held, seen1, seen2);
        for (uint32_t j = 0; j < sc->n; j++) if (sc->ticket[j]) mtl_present_wait(sc->ticket[j]);

        // 2. Zero-timeout with pending tickets on all unheld images: returns VK_NOT_READY without sleeping, leaving fence and *idx untouched
        sc->ticket[1] = g_fake_done_tk + 10;
        sc->ticket[2] = g_fake_done_tk + 11;
        g_fake_sleep_calls = 0; g_fake_wait_calls = 0;
        atomic_store(&f->val, FENCE_NONE);
        uint32_t sentinel = 0xdeadbeefu;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 0, NULL, f, &sentinel) == VK_NOT_READY, "zero-timeout with pending tickets returns VK_NOT_READY");
        CHECK(g_fake_sleep_calls == 0 && g_fake_wait_calls == 0 && sentinel == 0xdeadbeefu && atomic_load(&f->val) == FENCE_NONE,
              "failed zero-timeout acquire never sleeps and leaves fence and output index untouched");

        // 3. Zero-timeout when all images are held: returns VK_NOT_READY immediately
        sc->held[1] = 1; sc->held[2] = 1;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 0, NULL, f, &sentinel) == VK_NOT_READY, "zero-timeout when all images held returns VK_NOT_READY");
        CHECK(g_fake_sleep_calls == 0 && sentinel == 0xdeadbeefu && atomic_load(&f->val) == FENCE_NONE, "all-held zero-timeout leaves fence and index untouched");
        sc->held[1] = 0; sc->held[2] = 0;

        // 4. Finite timeout expiring during pacing: respects single timeout budget and does not touch fence or *idx
        uint64_t t0 = g_fake_now;
        g_next_acq = t0 + 16000000ull;   // 16 ms until next frame
        g_fake_sleep_calls = 0; g_fake_sleep_total = 0; g_fake_wait_calls = 0;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 5000000ull, NULL, f, &sentinel) == VK_TIMEOUT, "finite timeout expires during pacing");
        CHECK(g_fake_now - t0 == 5000000ull && g_fake_wait_calls == 0 && sentinel == 0xdeadbeefu && atomic_load(&f->val) == FENCE_NONE,
              "pacing-bounded timeout consumed exactly one budget and left fence/index untouched");
        CHECK(g_next_acq == t0 + 16000000ull, "timed-out acquire did not advance g_next_acq");

        // 5. Display-link timeout at finite deadline does NOT fall through to nanosleep
        g_fake_link_on = 1; g_fake_link_succeed = 0; g_fake_sleep_calls = 0;
        t0 = g_fake_now;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 4000000ull, NULL, f, &sentinel) == VK_TIMEOUT, "display-link finite timeout returns VK_TIMEOUT");
        CHECK(g_fake_now - t0 == 4000000ull && g_fake_sleep_calls == 0 && sentinel == 0xdeadbeefu && atomic_load(&f->val) == FENCE_NONE,
              "display-link timeout did not fall through to nanosleep");
        g_fake_link_on = 0;

        // 6. Single monotonic deadline shared across pacing (3 ms) + pending ticket wait (remaining 7 ms -> expires at 10 ms total)
        t0 = g_fake_now;
        g_next_acq = t0 + 3000000ull;
        g_fake_ready_at = t0 + 20000000ull;
        g_fake_sleep_calls = 0; g_fake_wait_calls = 0; g_fake_wait_last_tmo = 0;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 10000000ull, NULL, f, &sentinel) == VK_TIMEOUT, "shared budget expires during pending ticket wait");
        CHECK(g_fake_wait_calls == 1 && g_fake_wait_last_tmo == 7000000ull && g_fake_now - t0 == 10000000ull,
              "ticket wait received only remaining 7 ms of 10 ms budget (got %llu, elapsed %llu)",
              (unsigned long long)g_fake_wait_last_tmo, (unsigned long long)(g_fake_now - t0));
        CHECK(sentinel == 0xdeadbeefu && atomic_load(&f->val) == FENCE_NONE, "ticket timeout left fence and index untouched");

        // 7. Pending ticket becomes ready before deadline: picks earliest ticket (index 1) and succeeds
        t0 = g_fake_now;
        g_next_acq = t0 + 2000000ull;
        g_fake_ready_at = t0 + 6000000ull;
        uint32_t got = 99;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 10000000ull, NULL, f, &got) == VK_SUCCESS && got == 1,
              "pending ticket becoming ready within budget returns earliest unheld image (got %u)", got);
        CHECK(g_fake_now - t0 == 6000000ull && sc->held[1] == 1 && atomic_load(&f->val) == 0, "eventual success marked image held and signaled fence");

        // 8. Real clock: all images held -> finite timeout expires cleanly, then concurrent present wakes waiting acquire
        hooks_restore();
        sc->ticket[0] = sc->ticket[1] = sc->ticket[2] = 0;
        sc->held[0] = sc->held[1] = sc->held[2] = 1;
        atomic_store(&f->val, FENCE_NONE);
        sentinel = 0xdeadbeefu;
        uint64_t w0 = now_ns();
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 5000000ull, NULL, f, &sentinel) == VK_TIMEOUT, "all-held real-clock finite wait returns VK_TIMEOUT");
        uint64_t elapsed = now_ns() - w0;
        CHECK(elapsed >= 4000000ull && elapsed < 200000000ull && sentinel == 0xdeadbeefu && atomic_load(&f->val) == FENCE_NONE,
              "all-held real-clock wait respected finite bound (%llu ns) and left fence/index untouched", (unsigned long long)elapsed);

        delayed_present_arg arg = {.sc = sc, .idx = 0};
        pthread_t pt;
        CHECK(!pthread_create(&pt, NULL, delayed_present_thread, &arg), "spawn delayed present thread");
        uint32_t woken = 99;
        CHECK(h_vkAcquireNextImageKHR(&g_device, sc, 500000000ull, NULL, f, &woken) == VK_SUCCESS && woken == 0,
              "concurrent present wakes waiting acquire and returns released index 0 (got %u)", woken);
        pthread_join(pt, NULL);
        CHECK(sc->held[0] == 1 && atomic_load(&f->val) == 0, "woken acquire holds image 0 and signals fence");

        h_vkDestroyFence(&g_device, f, NULL);
        h_vkDestroySwapchainKHR(&g_device, sc, NULL);
    }
}

int main(void) {
    static __weak id<MTLTexture> weak[CYCLES][4];
    @autoreleasepool {
        dev_init();
        g_test_layer = [CAMetalLayer layer]; g_test_layer.device = (__bridge id<MTLDevice>)mtl_device(); g_test_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_test_layer.framebufferOnly = NO; g_test_layer.drawableSize = CGSizeMake(64, 32);
    }
    test_ownership_and_timeouts();
    int ticks_before = atomic_load(&g_vk_frame_ticks);
    for (int c = 0; c < CYCLES; c++) one_cycle(weak[c]);
    CHECK(atomic_load(&g_vk_frame_ticks) == ticks_before + CYCLES * FRAMES,
          "each Vulkan vkQueuePresentKHR drives dm_guest_frame_tick once");
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
