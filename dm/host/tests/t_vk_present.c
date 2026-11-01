// The Vulkan backend's window presenter and vsync source (dm/gfx/hostvk/vk_present.c; docs/design/vulkan-backend.md 7.2 to 7.4, step P4a) on a surface that needs no display:
// VK_EXT_headless_surface, made by this test where the host program registers front_sdl.c's window (hvk_present_set_window), with front_vk_instance_extensions asking for it.
// A frame of a known picture (an R8G8B8A8_SRGB image on the backend's device, as the guest Vulkan layer registers its swapchain images) goes through be_present_ext, and the
// presenter reads back each swapchain image as it hands it to the presentation engine (hvk_win_readback). Checked:
//   ten frames presented, FIFO, at least three images, the swapchain's format a UNORM one;
//   the picture: the layer crop's region only (the band outside it is magenta and must not show), its stored bytes unchanged (an sRGB frame through the UNORM twin), the four
//     quadrants where they belong, scaled bilinearly to a 16:9 window that it fills;
//   one to one (a window of the region's size): nearest, every byte as stored, the quadrant edges exact;
//   the lower-left origin (be_set_origin_lower_left): the picture flipped;
//   another window size (1000x1000): the swapchain made again, the picture letterboxed (black bars above and below, centred);
//   the vsync source: active once the window presenter runs, its ticks at 60 Hz (be_vsync_wait, be_vsync_wait_vsync), be_has_window, be_display_hz;
//   no validation message (the layer with synchronization validation).
//   --mailbox: the same with DM_VK_PRESENT_MODE=mailbox (MAILBOX where the surface has it)
//   --headless: DM_VK_HEADLESS=1: the device without a swapchain, the frames through the headless presenter (hvk_present_last: the crop's raw bytes), no vsync source.
// Without a Vulkan device it skips (t_vk_common.h's rule); without VK_EXT_headless_surface the window parts are reported as not run and the headless one runs
#include "../dm.h"
#include "nvn_be.h"
#include "guest_vk.h"
#include "vk_present.h"
#include "vk_device.h"
#include <math.h>
#include <stdatomic.h>

// ---------- What the backend and the NVN frontend need of the host (the GPU tests' stubs, dm/gfx/tests/vk/t_vk_harness.h) ----------
uint64_t g_t0_ns;
uint64_t now_ns(void) { return plat_now_ns(); }
uint64_t guest_ns(void) { return plat_now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
int front_screen_max_fps(void) { return 60; }
static void (*g_prep)(void *, size_t);
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
void dm_guest_write_prepare(void *addr, size_t len) { if (g_prep) g_prep(addr, len); }
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { (void)fn; }   // no texture upload here: the dirty tracker never protects a page

// ---------- The window this test gives the presenter ----------
static const char *g_exts[2]; static unsigned g_nexts;
int front_vk_instance_extensions(const char **names, unsigned *n) {
    unsigned k = 0;
    if (names && n) for (; k < *n && k < g_nexts; k++) names[k] = g_exts[k];
    if (n) *n = k;
    return (int)g_nexts;
}
static _Atomic int g_win_w = 1600, g_win_h = 900, g_surfaces;
static int pixel_size(int *w, int *h) { *w = g_win_w; *h = g_win_h; return 0; }
static int make_surface(void *instance, void **out) {
    VkHeadlessSurfaceCreateInfoEXT hi = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT, NULL, 0};
    VkSurfaceKHR s;
    if (vkCreateHeadlessSurfaceEXT((VkInstance)instance, &hi, NULL, &s)) return -1;
    g_surfaces++;
    *out = (void *)s;
    return 0;
}

// ---------- Checks ----------
static int g_total, g_fail;
static void t(const char *name, int ok) { g_total++; g_fail += !ok; printf("%-120s %s\n", name, ok ? "ok" : "FAIL"); }
static int env_set(const char *v) { const char *e = getenv(v); return e && *e; }
static int vk_required(void) { return env_set("VK_ICD_FILENAMES") || env_set("VK_DRIVER_FILES") || env_set("VK_LOADER_DRIVERS_SELECT"); }
static int validation_required(void) { const char *e = getenv("VK_INSTANCE_LAYERS"); return e && strstr(e, "VK_LAYER_KHRONOS_validation"); }
const hostvk_dev *hvk_device(void);
void hvk_shutdown(void);
int be_vsync_wait_vsync(uint64_t *seen, uint64_t timeout_ns);   // the vsync source as hle_sdk16.c's vsync event takes it (vk_present.c)
int hvk_oneoff_begin(VkCommandBuffer *cb);
int hvk_oneoff_end(VkCommandBuffer cb);
VkResult hvk_buffer_make(VkDeviceSize n, VkBufferUsageFlags usage, int host_visible, VkBuffer *buf, VkDeviceMemory *mem, void **map);
void hvk_buffer_free(VkBuffer buf, VkDeviceMemory mem);

// ---------- The frame ----------
// 320x180: the crop region (40, 20, 240, 135) holds four quadrants; the band around it is magenta
enum { FW = 320, FH = 180, CX = 40, CY = 20, CW = 240, CH = 135 };
static const uint8_t TL[4] = {200, 30, 40, 255}, TR[4] = {30, 200, 60, 255}, BL[4] = {20, 40, 210, 255}, BR[4] = {100, 100, 100, 255}, MAGENTA[4] = {255, 0, 255, 255};
static const uint8_t *frame_texel(int x, int y) {
    if (x < CX || x >= CX + CW || y < CY || y >= CY + CH) return MAGENTA;
    int right = x >= CX + CW / 2, bottom = y >= CY + 70;   // the top quadrants are 70 rows, the bottom ones 65
    return bottom ? (right ? BR : BL) : (right ? TR : TL);
}
static VkImage g_img; static VkDeviceMemory g_img_mem;
static int make_frame(const hostvk_dev *d) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_SRGB, {FW, FH, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(d->device, &ci, NULL, &g_img)) return 0;
    VkMemoryRequirements r; vkGetImageMemoryRequirements(d->device, g_img, &r);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < d->mem.memoryTypeCount && type == UINT32_MAX; i++) if (r.memoryTypeBits >> i & 1) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, type};
    if (vkAllocateMemory(d->device, &ai, NULL, &g_img_mem) || vkBindImageMemory(d->device, g_img, g_img_mem, 0)) return 0;
    VkBuffer b; VkDeviceMemory bm; void *map;
    if (hvk_buffer_make((VkDeviceSize)FW * FH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &b, &bm, &map)) return 0;
    for (int y = 0; y < FH; y++) for (int x = 0; x < FW; x++) memcpy((uint8_t *)map + ((size_t)y * FW + x) * 4, frame_texel(x, y), 4);
    VkCommandBuffer cb;
    if (!hvk_oneoff_begin(&cb)) return 0;
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier2 to_dst = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_img, whole};
    VkDependencyInfo d1 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &to_dst};
    vkCmdPipelineBarrier2(cb, &d1);
    VkBufferImageCopy c = {0, FW, FH, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {FW, FH, 1}};
    vkCmdCopyBufferToImage(cb, b, g_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    VkImageMemoryBarrier2 to_src = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, HVK_EXT_LAYOUT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, g_img, whole};
    VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &to_src};
    vkCmdPipelineBarrier2(cb, &d2);
    int ok = hvk_oneoff_end(cb);
    hvk_buffer_free(b, bm);
    return ok;
}

// ---------- Reading the window's picture ----------
static uint8_t *g_shot; static size_t g_shot_n;
static hvk_win_info g_info;
static int bgra;   // the swapchain's byte order
static int shot(void) {
    if (!hvk_win_last(NULL, 0, &g_info)) return 0;
    size_t n = (size_t)g_info.width * g_info.height * 4;
    if (n > g_shot_n) { free(g_shot); g_shot = malloc(n); g_shot_n = n; }
    if (!g_shot || !hvk_win_last(g_shot, n, &g_info)) return 0;
    bgra = g_info.format == VK_FORMAT_B8G8R8A8_UNORM;
    const char *dump = getenv("T_VK_PRESENT_DUMP");   // a folder: each picture read back as <n>.ppm, to look at
    if (dump && *dump) {
        static int k; char path[1024]; snprintf(path, sizeof path, "%s/%d.ppm", dump, k++);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", g_info.width, g_info.height);
            for (size_t i = 0; i < (size_t)g_info.width * g_info.height; i++) { const uint8_t *p = g_shot + 4 * i; uint8_t rgb[3] = {bgra ? p[2] : p[0], p[1], bgra ? p[0] : p[2]}; fwrite(rgb, 1, 3, f); }
            fclose(f);
        }
    }
    return 1;
}
static void px(int x, int y, uint8_t out[4]) {   // RGBA at window pixel (x, y)
    const uint8_t *p = g_shot + ((size_t)y * g_info.width + x) * 4;
    out[0] = bgra ? p[2] : p[0]; out[1] = p[1]; out[2] = bgra ? p[0] : p[2]; out[3] = p[3];
}
static int same(const uint8_t a[4], const uint8_t b[4], int tol) { for (int i = 0; i < 3; i++) if (abs((int)a[i] - (int)b[i]) > tol) return 0; return 1; }
static int at_is(int x, int y, const uint8_t *want, int tol, char *why, size_t n) {
    uint8_t got[4]; px(x, y, got);
    int ok = same(got, want, tol);
    if (!ok && why) snprintf(why + strlen(why), n - strlen(why), " (%d,%d)=%u,%u,%u want %u,%u,%u", x, y, got[0], got[1], got[2], want[0], want[1], want[2]);
    return ok;
}
// The picture's region of the window (info.dst) shows the crop scaled: the source texel at the centre of each sampled window pixel, away from the quadrant edges
static int picture_ok(int flipped, char *why, size_t n) {
    int ok = 1;
    const int32_t *r = g_info.dst;
    for (int j = 0; j < 9; j++) for (int i = 0; i < 9; i++) {
        double u = (i + 0.5) / 9, v = (j + 0.5) / 9;   // normalized in the crop
        int sx = CX + (int)(u * CW), sy = CY + (int)(v * CH);
        if (abs(sx - (CX + CW / 2)) < 6 || abs(sy - (CY + 70)) < 6) continue;   // near a quadrant edge: filtered
        int wx = r[0] + (int)(u * r[2]), wy = r[1] + (int)((flipped ? 1 - v : v) * r[3]);
        if (!at_is(wx, wy, frame_texel(sx, sy), 2, why, n)) ok = 0;
    }
    return ok;
}
static long count_near(const uint8_t *c, int tol) {   // window pixels of colour c
    long k = 0; uint8_t p[4];
    for (uint32_t y = 0; y < g_info.height; y++) for (uint32_t x = 0; x < g_info.width; x++) { px((int)x, (int)y, p); k += same(p, c, tol); }
    return k;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *phase = argc > 1 ? argv[1] : "";
    int headless = !strcmp(phase, "--headless"), mailbox = !strcmp(phase, "--mailbox");
    if (*phase && !headless && !mailbox) { fprintf(stderr, "usage: t_vk_present [--headless | --mailbox]\n"); return 2; }
    if (!getenv("DM_VK_VALIDATION")) setenv("DM_VK_VALIDATION", "1", 1);
    setenv("DM_VK_PSO_CACHE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1);
    if (headless) setenv("DM_VK_HEADLESS", "1", 1);
    if (mailbox) setenv("DM_VK_PRESENT_MODE", "mailbox", 1);
    // The loader's instance extensions: VK_EXT_headless_surface gives a surface without a display
    int surf_ext = 0;
    if (volkInitialize() == VK_SUCCESS) {
        uint32_t n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
        if (e && vkEnumerateInstanceExtensionProperties(NULL, &n, e) >= 0) for (uint32_t i = 0; i < n; i++) surf_ext |= !strcmp(e[i].extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
        free(e);
    }
    printf("loader: %s %s\n", VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, surf_ext ? "present" : "absent");
    int window = surf_ext;
    if (window) { g_exts[0] = VK_KHR_SURFACE_EXTENSION_NAME; g_exts[1] = VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME; g_nexts = 2; hvk_present_set_window(make_surface, pixel_size); }
    else printf("note: the loader has no %s: the window presenter's swapchain parts are not run, only the headless presenter\n", VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    be_init();
    const hostvk_dev *d = hvk_device();
    if (!d) {
        if (!vk_required()) { printf("note: no Vulkan device: skipped\n== t_vk_present: 0 checks, 0 failed (skipped: no Vulkan device) ==\n"); return 0; }
        t("the Vulkan backend has a device (VK_ICD_FILENAMES / VK_DRIVER_FILES / VK_LOADER_DRIVERS_SELECT names a driver)", 0);
        printf("== t_vk_present: %d checks, %d failed ==\n", g_total, g_fail); return 1;
    }
    printf("device '%s' (%s), validation %s; present wait %u, display timing %u\n", d->props.deviceName, d->driver_name, d->validation ? "on" : "off", d->x.present_wait, d->x.display_timing);
    int win = window && !headless;
    t(win ? "the device was made for the window: VK_KHR_swapchain enabled" : "the device was made without a swapchain (no window, or DM_VK_HEADLESS=1)", d->swapchain == win);
    t("the frame: an R8G8B8A8_SRGB image of a known picture on the backend's device", make_frame(d));
    hvk_ext_image ei = {g_img, VK_FORMAT_R8G8B8A8_SRGB, FW, FH};
    uint64_t serial = be_ext_tex(&ei);
    const int crop[4] = {CX, CY, CW, CH};
    char why[2048], name[2600];
    if (win) {
        hvk_win_readback(1);
        uint64_t tk = 0; int ok = 1;
        for (int i = 0; i < 10; i++) { tk = be_present_ext(serial, 1.0 / 60, NULL, 0, crop); ok &= tk != 0; be_present_wait(tk); }
        t("ten frames presented through be_present_ext, each with a ticket be_present_wait returns on", ok);
        t("the frames go to the window presenter, which made the surface once", hvk_win_on() && be_has_window() && g_surfaces == 1);
        int got = shot();
        snprintf(name, sizeof name, "the newest frame read back from the swapchain image: %ux%u, %u images, format %d, mode %d, %llu presented, %llu dropped", g_info.width, g_info.height,
                 g_info.images, g_info.format, g_info.mode, (unsigned long long)g_info.presents, (unsigned long long)g_info.dropped);
        t(name, got && g_info.width == 1600 && g_info.height == 900 && g_info.presents >= 10 && g_info.dropped == 0);
        t("at least three swapchain images, in a UNORM format (the stored bytes reach the screen unchanged)", g_info.images >= 3 && (g_info.format == VK_FORMAT_B8G8R8A8_UNORM || g_info.format == VK_FORMAT_R8G8B8A8_UNORM));
        t(mailbox ? "DM_VK_PRESENT_MODE=mailbox: MAILBOX" : "FIFO by default (decision D12)", g_info.mode == (mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR));
        snprintf(name, sizeof name, "the crop's %dx%d region fills the 16:9 window (%d,%d %dx%d), scaled bilinearly", CW, CH, g_info.dst[0], g_info.dst[1], g_info.dst[2], g_info.dst[3]);
        t(name, got && g_info.dst[0] == 0 && g_info.dst[1] == 0 && g_info.dst[2] == 1600 && g_info.dst[3] == 900 && g_info.linear && g_info.src_w == CW && g_info.src_h == CH);
        why[0] = 0;
        t("the four quadrants where they belong, with the frame's stored sRGB bytes (no decode)", got && picture_ok(0, why, sizeof why));
        if (why[0]) printf("     %s\n", why);
        long mag = got ? count_near(MAGENTA, 40) : -1;
        snprintf(name, sizeof name, "nothing outside the crop shows: %ld magenta pixels", mag);
        t(name, mag == 0);
        // The lower-left origin: stored bottom row first, shown upright
        be_set_origin_lower_left(1);
        tk = be_present_ext(serial, 1.0 / 60, NULL, 0, crop); be_present_wait(tk);
        be_set_origin_lower_left(0);
        got = shot(); why[0] = 0;
        t("the lower-left origin: the picture flipped (the bottom quadrants on top)", got && g_info.flip && picture_ok(1, why, sizeof why));
        if (why[0]) printf("     %s\n", why);
        // One to one: nearest, every byte as stored, the edges exact
        g_win_w = CW; g_win_h = CH;
        tk = be_present_ext(serial, 1.0 / 60, NULL, 0, crop); be_present_wait(tk);
        got = shot(); why[0] = 0;
        int exact = got && g_info.width == CW && g_info.height == CH && !g_info.linear;
        for (int y = 0; exact && y < CH; y++) for (int x = 0; x < CW; x++) if (!at_is(x, y, frame_texel(CX + x, CY + y), 0, why, sizeof why)) { exact = 0; break; }
        snprintf(name, sizeof name, "a window of the region's size (%dx%d): the swapchain made again (%llu so far), nearest, every texel exact", CW, CH, (unsigned long long)g_info.swapchains);
        t(name, exact && g_info.swapchains >= 2);
        if (why[0]) printf("     %s\n", why);
        // Another aspect: letterboxed
        g_win_w = 1000; g_win_h = 1000;
        tk = be_present_ext(serial, 1.0 / 60, NULL, 0, crop); be_present_wait(tk);
        got = shot(); why[0] = 0;
        const uint8_t black[4] = {0, 0, 0, 255};
        int box = got && g_info.width == 1000 && g_info.height == 1000 && g_info.dst[0] == 0 && g_info.dst[2] == 1000 && abs(g_info.dst[3] - 563) <= 1 && abs(g_info.dst[1] - 218) <= 1;
        box &= at_is(500, 5, black, 0, why, sizeof why) && at_is(500, g_info.dst[1] - 2, black, 0, why, sizeof why) && at_is(500, 995, black, 0, why, sizeof why)
               && at_is(500, g_info.dst[1] + g_info.dst[3] + 1, black, 0, why, sizeof why);
        box &= picture_ok(0, why, sizeof why);
        snprintf(name, sizeof name, "a 1000x1000 window: the picture letterboxed at %d,%d %dx%d, black above and below", g_info.dst[0], g_info.dst[1], g_info.dst[2], g_info.dst[3]);
        t(name, box);
        if (why[0]) printf("     %s\n", why);
        // The vsync source
        t("the vsync source runs (be_vsync_active), at 60 Hz (be_vsync_rate), with the screen's 60 Hz (be_display_hz)", be_vsync_active() && be_vsync_rate() == 60 && be_display_hz() == 60);
        uint64_t t0 = plat_now_ns(); int ticks = 0;
        for (int i = 0; i < 12; i++) ticks += be_vsync_wait(1, 100000000ull);
        double ms = (plat_now_ns() - t0) / 1e6;
        snprintf(name, sizeof name, "12 waits for one vsync each: %d ticks in %.1f ms (%.2f ms a tick; 16.67 at 60 Hz)", ticks, ms, ms / 12);
        t(name, ticks == 12 && ms / 12 > 12 && ms / 12 < 30);
        uint64_t seen = 0; int ev = 0; t0 = plat_now_ns();
        for (int i = 0; i < 6; i++) ev += be_vsync_wait_vsync(&seen, 100000000ull);
        ms = (plat_now_ns() - t0) / 1e6;
        snprintf(name, sizeof name, "hle_sdk16.c's vsync event source (be_vsync_wait_vsync): %d of 6 ticks in %.1f ms", ev, ms);
        t(name, ev == 6 && ms > 60 && ms < 200);
        printf("vsync anchor: %s; presented time of present 1: %.6f s (0: not known without present wait or display timing)\n",
               g_info.timing == 3 ? "present wait" : g_info.timing == 2 ? "display timing" : g_info.timing == 1 ? "an acquire that waited (FIFO)" : "none (a free-running 60 Hz clock)",
               be_presented_time(1));
        be_present_flush();
    } else {
        // The headless presenter: the crop's raw bytes in the host-visible buffer
        uint64_t tk = be_present_ext(serial, 1.0 / 60, NULL, 0, crop);
        be_present_wait(tk);
        static uint8_t pic[CW * CH * 4]; hvk_present_info pi;
        int got = hvk_present_last(pic, sizeof pic, &pi), exact = got && pi.width == CW && pi.height == CH;
        for (int y = 0; exact && y < CH; y++) for (int x = 0; x < CW; x++) if (memcmp(pic + ((size_t)y * CW + x) * 4, frame_texel(CX + x, CY + y), 4)) { exact = 0; break; }
        t("the headless presenter: the crop's region, every byte as stored", tk && exact);
        t("no window and no vsync source: be_has_window 0, be_vsync_active 0, be_vsync_wait returns 0 at once", !be_has_window() && !be_vsync_active() && !be_vsync_wait(1, 50000000ull));
        if (!window) printf("note: not run here: the swapchain, the scaling, the letterbox, the flip in the window, the vsync source (no %s)\n", VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    }
    hvk_ext_tex_drop(serial);
    vkDestroyImage(d->device, g_img, NULL); vkFreeMemory(d->device, g_img_mem, NULL);
    const hostvk_dev *dd = hvk_device();
    hvk_shutdown();
    if (dd && dd->validation) { snprintf(name, sizeof name, "no validation message: %u errors, %u warnings", (unsigned)dd->val_errors, (unsigned)dd->val_warnings); t(name, dd->val_errors == 0 && dd->val_warnings == 0); }
    else if (validation_required()) t("validation layer on", 0);
    else printf("note: VK_LAYER_KHRONOS_validation is not installed: validation not checked\n");
    printf("== t_vk_present%s%s: %d checks, %d failed ==\n", *phase ? " " : "", phase, g_total, g_fail);
    return g_fail != 0;
}
