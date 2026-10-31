// The guest Vulkan layer on the Vulkan backend (guest_vk.c; docs/design/vulkan-backend.md 8.1, step P9a): what it shares with the backend's present path (vk_backend.c) and
// with its tests (dm/gfx/tests/vk/t_vk_guest.c, t_vk_spirv_fix.c). Plain C over volk, Windows and Linux.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "volk.h"

// ---------- External producers on this backend (nvn_be_ext.h's be_ext_tex / be_present_ext; vk_backend.c) ----------
// What a producer hands be_ext_tex here (the Metal backend takes an id<MTLTexture>): a 2D colour image of one level and one layer, 4 bytes a texel, made on the backend's device
// with TRANSFER_SRC usage. The backend copies the description; the image stays the producer's, which drops it (hvk_ext_tex_drop) before destroying it
typedef struct { VkImage image; VkFormat format; uint32_t width, height; } hvk_ext_image;
// The layout an external image is in when it is presented, and the presenter leaves it in: the producer's frame ends in it (the guest layer turns the game's PRESENT_SRC_KHR,
// which names presentable images of a real swapchain, into it). The headless presenter copies from it; the window presenter of phase P4 may sample it instead
#define HVK_EXT_LAYOUT VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
// be_present_ext's ev on this backend: a timeline VkSemaphore of the backend's device (as void *), which the present waits for ev_val on before it reads the image (NULL: no wait).
// Its ticket is the present's own timeline value; be_present_wait(ticket) returns once the present no longer reads the image
void hvk_ext_tex_drop(uint64_t serial);   // the serial no longer names an image (after be_present_wait of its last ticket)
// The picture the presenter would show last: no window yet (phase P4), so the headless presenter copies each presented frame's region (the layer crop, else the whole image)
// into a buffer. Waits for that copy, then gives its size and format and up to n bytes of its texels (rows of 4-byte texels, tightly packed). 0 when nothing was presented
typedef struct { uint64_t serial, ticket, frames; uint32_t x, y, width, height; VkFormat format; } hvk_present_info;
int hvk_present_last(void *texels, size_t n, hvk_present_info *info);

// ---------- SPIR-V checks and fix-ups (guest_vk.c; design 8.1) ----------
// The structural checks a module passes before it reaches the driver (the parts of spirv-val the fix-ups rely on): header (magic, version 1.0 to 1.6, id bound, schema),
// instruction framing, the logical layout's section order, one memory model, an entry point, the Shader capability, and the ids the fix-ups read below the bound.
// NULL when the module may be handed to the driver, else why not (a static string)
const char *guest_spirv_check(const uint32_t *w, size_t n);
enum {
    GUEST_SPIRV_STRIDES = 1,   // ArrayStride decorations on struct members (OpMemberDecorate) moved onto the members' array types (OpDecorate), where SPIR-V puts them
    GUEST_SPIRV_DUAL = 2,      // a fragment output at Location 1 without an Index becomes Location 0, Index 1: the second source of dual-source blending
};
typedef struct {
    int strides_moved;     // array types given the ArrayStride a member decoration carried
    int strides_dropped;   // member ArrayStride decorations removed (every one: moved, already on the type, or on a member that is no array)
    int stride_conflicts;  // a member decoration whose stride differs from the one its array type gets (the first one is kept)
    int dual_rewritten;    // outputs moved from Location 1 to Location 0 Index 1 (0 or 1)
} guest_spirv_report;
// The fix-ups of `what` (GUEST_SPIRV_*) on a module that passed guest_spirv_check. Returns the module to hand the driver: w itself when nothing changed (*out_n = n), else a new
// malloc'd one the caller frees; NULL when out of memory. Changes only what the report counts; ids, the bound and every other instruction stay as they are
uint32_t *guest_spirv_fix(const uint32_t *w, size_t n, unsigned what, size_t *out_n, guest_spirv_report *r);
// 1 for a module whose fragment entry point writes an output at Location 1 without an Index and none at Location 0 Index 1: a pipeline that blends with the second source
// (a SRC1 factor) takes its GUEST_SPIRV_DUAL form, as NVIDIA's hardware feeds output 1 to source 1 (rendering.md, "Dual-source blending from output location 1")
int guest_spirv_dual_candidate(const uint32_t *w, size_t n);
