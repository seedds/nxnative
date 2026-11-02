// The Vulkan backend's window presenter and vsync source (docs/design/vulkan-backend.md 7.2, 7.3, 7.4; step P4a), beside the headless presenter of vk_backend.c. Plain C over volk,
// Windows and Linux. vk_backend.c routes be_present / be_present_ext here while the window presenter is on, and to the headless presenter otherwise (no window, DM_VK_HEADLESS=1, or
// a window the presenter could not use: one red line says why).
//   The present (game thread): the frame's region (the layer crop, else the whole image) is copied raw into a snapshot image of the presenter, on the backend's queue, after the
//     producer's work (queue order and a full barrier; an external producer's timeline value is waited for too). The ticket is that copy's value on the presenter's timeline, so
//     be_present_wait returns once the copy is done: the game's texture is free then, as Apple's present snapshots make it (design 7.2). An sRGB frame is copied into its UNORM
//     twin, so its stored bytes reach the screen unchanged (the Switch scans the bytes out; Metal's present_raw_src).
//   The present thread: takes the snapshots in order, acquires a swapchain image, clears it black (the letterbox bars), blits the snapshot into the centred rectangle of the window
//     that keeps the frame's aspect (bilinear, nearest at one to one; the lower-left origin flipped), and presents it. DM_PRESENT_UPSCALE (design 7.4, decision D4; vk_present.c
//     "Scaling") changes the scaling: `fsr` runs AMD FidelityFX Super Resolution 1.0 (EASU then RCAS, two compute passes in the same command buffer) where the picture is shown
//     larger than the game renders it, `integer` scales by nearest neighbour at the largest whole multiple that fits; `off` (the default) is the bilinear blit.
//     The swapchain follows the window's size (recreated on VK_ERROR_OUT_OF_DATE_KHR, VK_SUBOPTIMAL_KHR, or when the front end reports another size), a UNORM format of 8 or 10
//     bits in SRGB_NONLINEAR. Its image count and present mode follow decision D12 (vk_present.c, "The image count and the present mode"): FIFO with the game's window texture
//     count, at least three, by default; DM_VK_LATENCY=low takes FIFO with two images, or MAILBOX where the surface allows no fewer than three; DM_VK_PRESENT_MODE=fifo|mailbox
//     forces the mode (a diagnostic; IMMEDIATE is not offered, design 7.3); the surface's minImageCount and maxImageCount bound the count. A window with another texture count
//     (a new window builder) re-creates the swapchain when the count changes.
//   The vsync source (design 7.3): a thread ticks at 60 Hz (be_vsync_rate), phase-locked to the screen's refreshes when the screen runs at a multiple of 60 Hz: the anchor is the time
//     a present reached the screen (VK_KHR_present_wait with VK_KHR_present_id where the device has them, else VK_GOOGLE_display_timing's actual present time where it has that),
//     else the return of an acquire that waited for the presentation engine to release an image (FIFO: at a refresh). The display link's ticks on Apple are the model:
//     be_vsync_wait / be_vsync_wait_ticks / be_vsync_wait_vsync (hle_sdk16.c's vsync event) follow mtl_link_wait_tick / _ticks / mtl_link_wait_vsync.
// The front end's window comes through hvk_present_set_window (the host program registers front_sdl.c's functions; the tests a headless surface of VK_EXT_headless_surface): the
// backend links no front end, so a binary without one (the GPU tests) stays headless.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "volk.h"

// ---------- The window (the host program, front.h) ----------
// make_surface: the window's VkSurfaceKHR on the backend's instance (front_vk_create_surface: 0, or -1 when there is none); pixel_size: the window's size in pixels now (0, or -1 when
// unknown; 0x0 while minimized). Any thread; registered before the backend comes up. The instance extensions come from front_vk_instance_extensions (vk_device.c, want_surface)
typedef int (*hvk_surface_fn)(void *vk_instance, void **vk_surface_out);
typedef int (*hvk_pixel_size_fn)(int *w, int *h);
void hvk_present_set_window(hvk_surface_fn make_surface, hvk_pixel_size_fn pixel_size);

// ---------- The backend's side (vk_backend.c) ----------
int hvk_win_wanted(void);       // a window is registered, it is a Vulkan window and DM_VK_HEADLESS does not refuse it: the device is made with a surface in mind (hostvk_opts.want_surface)
void hvk_win_device_up(int with_surface);   // the device came up, with VK_KHR_swapchain or (with_surface 0) without: headless then
int hvk_win_ready(void);        // presents go to the window presenter: it is on, or it will start at this present (its surface is made then); 0: the headless presenter
// One present: image (in layout, on the backend's device; w x h; format fmt) region crop (x, y, w, h; NULL or an invalid one: the whole image), flipped vertically when flip,
// after the timeline value wait_val of wait (VK_NULL_HANDLE: none). Returns the ticket (be_present_wait), 0 when the frame is not shown (logged once)
uint64_t hvk_win_present(VkImage image, VkFormat fmt, VkImageLayout layout, uint32_t w, uint32_t h, const int crop[4], int flip, VkSemaphore wait, uint64_t wait_val);
void hvk_win_present_wait(uint64_t ticket);   // the present of ticket no longer reads its image
void hvk_win_flush(void);                     // every present so far no longer reads its image
void hvk_win_shutdown(void);                  // the threads stopped, every object released, the surface destroyed (before hostvk_destroy)
// The game's window registered a texture (decision D12): be_xv_window_tex (nvnWindowInitialize hands the backend each texture of the window builder's SetTextures) and be_ext_tex
// (a guest layer's swapchain or window surface image) call it with a key of their own. The keys registered after a present form the next window's set; its size, at that
// window's first present, is the texture count the swapchain's image count follows. Any thread, before or after the device came up
void hvk_win_texture(uint64_t key);
int hvk_win_on(void);                         // the window presenter shows frames (be_has_window)
int hvk_win_display_hz(void);                 // the screen's refresh rate while it is on (the front end's screen mode, else the refresh duration, else 60); 0 off
double hvk_win_presented_time(uint64_t n);    // be_presented_time: seconds on plat_now_ns's clock when present n (1 = the first) reached the screen, -1 dropped, 0 not known
uint32_t hvk_win_qos(void);                   // the present thread's QoS class (be_present_qos)
// The vsync source (be_vsync_*, nvn_be.h / nvn_be_ext.h): the contract of the Metal display link's functions
int hvk_vsync_active(void);
int hvk_vsync_rate(void);
int hvk_vsync_stalled(void);
int hvk_vsync_wait(int vsyncs, uint64_t timeout_ns);
int hvk_vsync_wait_ticks(int ticks, uint64_t timeout_ns);
int hvk_vsync_wait_vsync(uint64_t *seen, uint64_t timeout_ns);

// ---------- Test hooks (dm/host/tests/t_vk_present.c) ----------
// With readback on, each window present also copies the swapchain image it presents into a host-visible buffer (before the image goes to the presentation engine); hvk_win_last
// waits until the present thread has shown every frame presented so far and its submission is done, then gives up to n bytes of the newest picture (rows of 4-byte texels,
// tightly packed, in the swapchain's format) and what was presented. Not while frames are being presented on another thread
typedef struct {
    uint64_t presents, ticket, swapchains, dropped;   // frames presented, the newest's ticket, swapchains made so far, frames not shown (minimized, out of date twice)
    uint32_t width, height, images;                   // the swapchain's extent and image count
    VkFormat format; VkPresentModeKHR mode;
    int32_t dst[4];                                   // the picture's rectangle in the window: x, y, w, h (the rest is the black letterbox)
    uint32_t src_w, src_h; int flip, linear;          // the region shown, flipped, filtered bilinearly
    int timing;                                       // the vsync source's anchor: 0 none yet, 1 an acquire that waited, 2 VK_GOOGLE_display_timing, 3 VK_KHR_present_wait
    int upscale;                                      // how the picture was scaled: 0 the blit, 1 FSR 1 with RCAS writing the swapchain image, 2 FSR 1 through a second
                                                      // intermediate image blitted one to one, 3 integer (nearest at a whole multiple)
    uint32_t textures;                                // decision D12: the game's window texture count the swapchain was made for (0: none registered)
    uint32_t asked, min_images, max_images;           // the image count asked for (the rule's), the surface's minImageCount and maxImageCount (0: no maximum)
    int low;                                          // DM_VK_LATENCY=low
    uint64_t shown_n;                                 // the present number (1 = the first) of the frame shown last
} hvk_win_info;
void hvk_win_readback(int on);
int hvk_win_last(void *texels, size_t n, hvk_win_info *info);
// The upscaler's settings as DM_PRESENT_UPSCALE and DM_PRESENT_SHARPEN would give them (NULL: unset), from the next frame shown on; no_direct 1 keeps FSR's RCAS off the
// swapchain image (the second intermediate image, as on a surface without storage). After the first present; the swapchain is made again for its usage
void hvk_win_upscale(const char *mode, const char *sharpen, int no_direct);
// DM_VK_LATENCY and DM_VK_PRESENT_MODE as the switches would give them (NULL: unset), from the next frame shown on: the mode is picked again and the swapchain made again
void hvk_win_latency(const char *latency, const char *present_mode);
// Decision D12's rule (vk_present.c, "The image count and the present mode") over a surface's bounds, for the tests to run on surfaces other than the one they have: the image
// count and the present mode for a window of `textures` textures (0: none registered) on a surface allowing min_images..max_images (max 0: no maximum) that offers MAILBOX or not,
// with DM_VK_LATENCY=low (low 1) and DM_VK_PRESENT_MODE (forced: -1 unset, else VK_PRESENT_MODE_FIFO_KHR or VK_PRESENT_MODE_MAILBOX_KHR)
void hvk_win_swap_rule(uint32_t textures, uint32_t min_images, uint32_t max_images, int has_mailbox, int low, int forced, uint32_t *images, VkPresentModeKHR *mode);
