// NVNformat → Vulkan (docs/design/vulkan-backend.md 5.1): the Vulkan backend's counterpart of nvn_fmt_mtl.h, in the same pure-C form. Every NVN format of nvn.h (0x01-0x9C) has a row:
// the Vulkan format, its aspect, the format's own component mapping, how exact it is, and the path a texture of it takes (since phase P7 every row has one). The table does not know
// the device: which usages a format supports is asked at run time (vkGetPhysicalDeviceFormatProperties, dm/gfx/hostvk/vk_texture.c), and a row's alt or conv format is taken when
// the device lacks the first one's features.
// No Vulkan function is called here; the VkFormat values come from the vendored headers (dm/third_party/vulkan-headers), so this file compiles on every host.
#pragma once
#include <stdint.h>
#include "nvn_fmt_mtl.h"   // NVN_SWZ_ZERO .. NVN_SWZ_A: the channel values of a component mapping (NVN's and Metal's enums have the same values)

enum {   // nvn_vk_fmt.aspect: VkImageAspectFlagBits values
    NVN_VK_COLOR = 0x1, NVN_VK_DEPTH = 0x2, NVN_VK_STENCIL = 0x4,
};
enum {   // nvn_vk_fmt.cls: the numeric class a shader reads the format as (and the type of a fragment output that writes it)
    NVN_VK_FLOAT = 0,   // float, unorm, snorm, sRGB, scaled, packed float, compressed
    NVN_VK_UINT = 1, NVN_VK_SINT = 2,
    NVN_VK_DS = 3,      // depth and / or stencil
};
enum {   // nvn_vk_fmt.flags
    NVN_VK_EXPAND3 = 0x1,    // three components: alt is the four-component format of the same kind; an upload expands each texel (alpha 1, or the integer 1) where the device cannot sample vk
    NVN_VK_SRGB = 0x2,       // an sRGB encoding
    NVN_VK_BLOCK = 0x4,      // block-compressed (BC, ASTC)
    NVN_VK_SCALED = 0x8,     // integer storage read as float without normalization (*_UI2F, *_I2F): a vertex format; as a texture the device rarely samples it
    NVN_VK_ORDER = 0x10,     // a 16-bit packed format whose bit order is taken from the format's name, not checked with real data (see the table)
    NVN_VK_NEEDS_EXT = 0x20, // vk needs a format or a usage the device may lack (VK_EXT_4444_formats / maintenance5 / ASTC LDR / D24 / S8 / sampling a three-component, scaled or
                             // signed 10-bit format): alt or conv is what the device gets instead
};
enum {   // nvn_vk_fmt.path: how a texture of the format is held, uploaded and read (vk_texture.c; step P7)
    NVN_VK_PATH_NONE = 0,     // no way to hold it (no row has this since phase P7)
    NVN_VK_PATH_DIRECT = 1,   // the texels as they are: vk, or alt read through the alt mapping (nvn_fmt_vk_alt_swizzle) where the device lacks vk; an upload only deswizzles
    NVN_VK_PATH_EXPAND3 = 2,  // three components: vk where the device samples it, else alt with each texel expanded at upload on the CPU, alpha one (and read as one)
    NVN_VK_PATH_CONVERT = 3,  // vk where the device samples it, else conv, each texel converted at upload on the CPU: signed RGB10A2 unpacked, integers read as float (*_UI2F,
                              // *_I2F) stored as the floats they read as
    NVN_VK_PATH_ASTC = 4,     // vk (ASTC) where the device has textureCompressionASTC_LDR, else decoded at upload into conv (RGBA8; an sRGB format into RGBA8 sRGB; decision D1), on
                              // the GPU (dm/gfx/hostvk/shaders/astc_decode.comp), on the CPU (dm/gfx/tex/astc_decode.c) where the GPU path cannot run
    NVN_VK_PATH_DEPTH = 5,    // depth / stencil whose guest packing is not Vulkan's per-aspect layout: converted at upload, in buffer copies and in CPU reads (DEPTH24_STENCIL8:
                              // depth in the low 24 bits, stencil in the top byte; DEPTH32F_STENCIL8: eight bytes a texel; DEPTH24 into D32_SFLOAT where the device lacks X8_D24)
    NVN_VK_PATH_N
};

typedef struct {
    uint32_t vk;         // VkFormat; 0 (VK_FORMAT_UNDEFINED) when Vulkan has none
    uint32_t alt;        // the substitute when the device lacks vk's features (see flags), 0 for none; for the 32-bit scaled rows, the integer format a vertex input reads
    uint32_t conv;       // NVN_VK_PATH_CONVERT and NVN_VK_PATH_ASTC: the VkFormat an upload converts or decodes into; 0 for the other paths
    uint8_t kind;        // 1 the texels NVN gives (through the row's path), 2 a substitute whose texels may differ (an unverified bit order), 0 none: the why says what happens
    uint8_t aspect;      // NVN_VK_COLOR / DEPTH / STENCIL bits
    uint8_t cls;         // NVN_VK_FLOAT / UINT / SINT / DS
    uint8_t flags;       // NVN_VK_*
    uint8_t path;        // NVN_VK_PATH_*
    uint8_t swz[4];      // for each output component r, g, b, a, which component of vk it takes (NVN_SWZ_*): "no alpha" formats read alpha 1, the swapped 16-bit packed ones r / b
    const char *why;     // what is substituted, converted or needs a device feature, and what happens without it (every row that is not a direct exact one); NULL otherwise
} nvn_vk_fmt;

// The row of NVN format f (0x01-0x9C), *out filled; returns its kind. A format outside the list (0, 0x9D and up) gives kind 0, vk 0 and the why "not an NVN format"
int nvn_fmt_to_vk(int nvn_format, nvn_vk_fmt *out);
// The component mapping (NVN_SWZ_*) that reads NVN format f's components through its row's alt format, for a backend that takes alt; the identity when f has no alt
void nvn_fmt_vk_alt_swizzle(int nvn_format, uint8_t swz[4]);
// The NVN format name of f ("RGBA8", "DEPTH24_STENCIL8"), "?" outside the list: for red lines and tests
const char *nvn_fmt_name(int nvn_format);
// The name of a path ("direct", "expand3", "convert", "astc", "depth"; "none")
const char *nvn_fmt_vk_path_name(int path);
