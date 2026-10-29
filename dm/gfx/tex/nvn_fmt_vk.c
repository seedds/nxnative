// NVNformat (nvn.h 0x01-0x9C) → Vulkan (nvn_fmt_vk.h; docs/design/vulkan-backend.md 5.1). One row per NVN format, in NVN enum order, beside the Metal table (nvn_fmt_mtl.c).
// Where the two differ:
// - Three-component formats (0x19-0x24) have Vulkan formats; optimal-tiling sampling of them is rarely supported, so each row names its four-component format as alt, which an
//   upload expands to (alpha 1) when the device cannot sample the first. A buffer texture reads the three components as they are (R32G32B32 texel buffers are common).
// - The 16-bit packed formats exist in both bit orders. RGB565 is red in its low five bits (the Metal table swaps red and blue for it, and Echoes of Wisdom's RGB565 masks drew red
//   without the swap: nvn_fmt_mtl.c, dm/gfx/tex/README.md), which is Vulkan's B5G6R5 exactly, and BGR565 is R5G6B5. The others are taken from their names, as on Metal, and stay
//   substitutes until real data checks them (NVN_VK_ORDER). Where the exact layout needs an optional format (A4B4G4R4, A1B5G5R5), alt is a mandatory one read through a swizzle.
// - Integer storage read as float (*_UI2F / *_I2F) is Vulkan's USCALED / SSCALED, a vertex format; Vulkan has none for 32-bit components, so those rows have none and name the
//   integer format as alt (the shader's shadow inputs, dmsh_spv.py, read it as an integer).
// - Depth 24 (0x33, 0x35) is X8_D24 / D24_UNORM_S8 where supported (NVIDIA, Intel), with the 32-bit float formats as alt (AMD), which changes what one unit of constant depth bias
//   means (decision D10, phase P7). Stencil 8 alone needs S8_UINT, optional; alt is D32_SFLOAT_S8_UINT, of which only the stencil is used.
// - ASTC needs textureCompressionASTC_LDR; elsewhere it is decoded at upload (decision D1, phase P7).
#include "nvn_fmt_vk.h"
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../../third_party/vulkan-headers/include/vulkan/vulkan_core.h"
#include <string.h>

enum { F = NVN_VK_FLOAT, U = NVN_VK_UINT, I = NVN_VK_SINT, DS = NVN_VK_DS };
enum { C = NVN_VK_COLOR, D = NVN_VK_DEPTH, ST = NVN_VK_STENCIL };
#define ID {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A}
#define NA {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_ONE}   // no alpha: alpha reads 1
#define SW_ARGB {NVN_SWZ_A, NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B}   // through B4G4R4A4: its "A" holds red, "R" green, "G" blue, "B" alpha
#define SW_BGRA {NVN_SWZ_B, NVN_SWZ_G, NVN_SWZ_R, NVN_SWZ_A}   // red and blue swapped
#define SW_BGR1 {NVN_SWZ_B, NVN_SWZ_G, NVN_SWZ_R, NVN_SWZ_ONE}
typedef struct { nvn_vk_fmt f; uint8_t alt_swz[4]; } row;     // alt_swz: the mapping that reads the NVN format's components through alt (identity when alt has the same layout)

// Row builders: exact (E: identity, X: no alpha), three-component (R3), and the general one (G)
#define E(vk, cls) {{vk, 0, 1, C, cls, 0, ID, NULL}, ID}
#define X(vk, cls) {{vk, 0, 1, C, cls, 0, NA, NULL}, NA}
#define EF(vk, cls, fl) {{vk, 0, 1, C, cls, fl, ID, NULL}, ID}
#define XF(vk, cls, fl) {{vk, 0, 1, C, cls, fl, NA, NULL}, NA}
#define R3(vk, alt, cls) {{vk, alt, 2, C, cls, NVN_VK_EXPAND3, ID, "three components: the device seldom samples them with optimal tiling, so an upload expands them to four (alpha 1) where it cannot"}, ID}
#define G(vk, alt, kind, asp, cls, fl, swz, why, aswz) {{vk, alt, kind, asp, cls, fl, swz, why}, aswz}

static const char *const W_ORDER = "a 16-bit packed format: bit order taken from its name (red in the low bits), not checked with real data";
static const char *const W_SCALED32 = "Vulkan has no 32-bit scaled format: bound as the integer format alt; a float vertex input reads it through the shader's shadow inputs (dmsh_spv.py), a float texture read is not converted (phase P4)";
static const char *const W_ASTC = "needs textureCompressionASTC_LDR; elsewhere decoded on the GPU at upload (decision D1, phase P7), not done yet";

static const row T[0x9D] = {
    [0x01] = E(VK_FORMAT_R8_UNORM, F), E(VK_FORMAT_R8_SNORM, F), E(VK_FORMAT_R8_UINT, U), E(VK_FORMAT_R8_SINT, I),
    [0x05] = E(VK_FORMAT_R16_SFLOAT, F), E(VK_FORMAT_R16_UNORM, F), E(VK_FORMAT_R16_SNORM, F), E(VK_FORMAT_R16_UINT, U), E(VK_FORMAT_R16_SINT, I),
    [0x0A] = E(VK_FORMAT_R32_SFLOAT, F), E(VK_FORMAT_R32_UINT, U), E(VK_FORMAT_R32_SINT, I),
    [0x0D] = E(VK_FORMAT_R8G8_UNORM, F), E(VK_FORMAT_R8G8_SNORM, F), E(VK_FORMAT_R8G8_UINT, U), E(VK_FORMAT_R8G8_SINT, I),
    [0x11] = E(VK_FORMAT_R16G16_SFLOAT, F), E(VK_FORMAT_R16G16_UNORM, F), E(VK_FORMAT_R16G16_SNORM, F), E(VK_FORMAT_R16G16_UINT, U), E(VK_FORMAT_R16G16_SINT, I),
    [0x16] = E(VK_FORMAT_R32G32_SFLOAT, F), E(VK_FORMAT_R32G32_UINT, U), E(VK_FORMAT_R32G32_SINT, I),
    [0x19] = R3(VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, F), R3(VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, F),
             R3(VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT, U), R3(VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT, I),
    [0x1D] = R3(VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, F), R3(VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, F),
             R3(VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, F), R3(VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT, U),
             R3(VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT, I),
    [0x22] = R3(VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, F), R3(VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT, U),
             R3(VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT, I),
    [0x25] = E(VK_FORMAT_R8G8B8A8_UNORM, F), E(VK_FORMAT_R8G8B8A8_SNORM, F), E(VK_FORMAT_R8G8B8A8_UINT, U), E(VK_FORMAT_R8G8B8A8_SINT, I),
    [0x29] = E(VK_FORMAT_R16G16B16A16_SFLOAT, F), E(VK_FORMAT_R16G16B16A16_UNORM, F), E(VK_FORMAT_R16G16B16A16_SNORM, F), E(VK_FORMAT_R16G16B16A16_UINT, U),
             E(VK_FORMAT_R16G16B16A16_SINT, I),
    [0x2E] = E(VK_FORMAT_R32G32B32A32_SFLOAT, F), E(VK_FORMAT_R32G32B32A32_UINT, U), E(VK_FORMAT_R32G32B32A32_SINT, I),
    [0x31] = G(VK_FORMAT_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, ST, DS, NVN_VK_NEEDS_EXT, ID, "S8_UINT is optional: where the device lacks it, the stencil plane of D32_SFLOAT_S8_UINT", ID),
    [0x32] = G(VK_FORMAT_D16_UNORM, 0, 1, D, DS, 0, ID, NULL, ID),
    [0x33] = G(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, 1, D, DS, NVN_VK_NEEDS_EXT, ID,
               "X8_D24 where supported; D32_SFLOAT elsewhere, which changes what one unit of constant depth bias means (decision D10, phase P7)", ID),
    [0x34] = G(VK_FORMAT_D32_SFLOAT, 0, 1, D, DS, 0, ID, NULL, ID),
    [0x35] = G(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, D | ST, DS, NVN_VK_NEEDS_EXT, ID,
               "D24_UNORM_S8_UINT where supported (not on AMD); D32_SFLOAT_S8_UINT elsewhere, which changes what one unit of constant depth bias means (decision D10, phase P7)", ID),
    [0x36] = G(VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 1, D | ST, DS, 0, ID, NULL, ID),
    [0x37] = XF(VK_FORMAT_R8G8B8A8_SRGB, F, NVN_VK_SRGB), EF(VK_FORMAT_R8G8B8A8_SRGB, F, NVN_VK_SRGB),
    // RGBA4: red in bits 0-3 → A4B4G4R4 (VK_EXT_4444_formats, core in 1.3 with optional support); alt B4G4R4A4 (mandatory), whose "A" is bits 0-3: read as (A, R, G, B)
    [0x39] = G(VK_FORMAT_A4B4G4R4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, 2, C, F, NVN_VK_ORDER | NVN_VK_NEEDS_EXT, ID, W_ORDER,
               SW_ARGB),
    // RGB5 / RGB5A1: red in bits 0-4 → A1B5G5R5 (maintenance5, optional); alt A1R5G5B5 (mandatory), red and blue swapped
    [0x3A] = G(VK_FORMAT_A1B5G5R5_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, C, F, NVN_VK_ORDER | NVN_VK_NEEDS_EXT, NA, W_ORDER,
               SW_BGR1),
    [0x3B] = G(VK_FORMAT_A1B5G5R5_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, C, F, NVN_VK_ORDER | NVN_VK_NEEDS_EXT, ID, W_ORDER,
               SW_BGRA),
    // RGB565: red in bits 0-4 → B5G6R5 exactly (optional support); alt R5G6B5 (mandatory), red and blue swapped
    [0x3C] = G(VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, 1, C, F, NVN_VK_NEEDS_EXT, ID,
               "B5G6R5 is optional: where the device lacks it, R5G6B5 read with red and blue swapped (sampling only: an attachment needs the identity mapping)",
               SW_BGRA),
    [0x3D] = E(VK_FORMAT_A2B10G10R10_UNORM_PACK32, F), E(VK_FORMAT_A2B10G10R10_UINT_PACK32, U), E(VK_FORMAT_B10G11R11_UFLOAT_PACK32, F), E(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, F),
    // DXT / RGTC / BPTC = BC1-7; BC1_RGB reads alpha 1 itself (the texel a punch-through block marks transparent reads black with alpha 1, as GL's RGB_S3TC_DXT1 does)
    [0x41] = EF(VK_FORMAT_BC1_RGB_UNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC2_UNORM_BLOCK, F, NVN_VK_BLOCK),
             EF(VK_FORMAT_BC3_UNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC1_RGB_SRGB_BLOCK, F, NVN_VK_BLOCK | NVN_VK_SRGB),
             EF(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, F, NVN_VK_BLOCK | NVN_VK_SRGB), EF(VK_FORMAT_BC2_SRGB_BLOCK, F, NVN_VK_BLOCK | NVN_VK_SRGB),
             EF(VK_FORMAT_BC3_SRGB_BLOCK, F, NVN_VK_BLOCK | NVN_VK_SRGB),
    [0x49] = EF(VK_FORMAT_BC4_UNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC4_SNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC5_UNORM_BLOCK, F, NVN_VK_BLOCK),
             EF(VK_FORMAT_BC5_SNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC7_UNORM_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC7_SRGB_BLOCK, F, NVN_VK_BLOCK | NVN_VK_SRGB),
             EF(VK_FORMAT_BC6H_SFLOAT_BLOCK, F, NVN_VK_BLOCK), EF(VK_FORMAT_BC6H_UFLOAT_BLOCK, F, NVN_VK_BLOCK),
    [0x51] = EF(VK_FORMAT_R8_USCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R8_SSCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R16_USCALED, F, NVN_VK_SCALED),
             EF(VK_FORMAT_R16_SSCALED, F, NVN_VK_SCALED),
             G(0, VK_FORMAT_R32_UINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID), G(0, VK_FORMAT_R32_SINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID),
    [0x57] = EF(VK_FORMAT_R8G8_USCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R8G8_SSCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R16G16_USCALED, F, NVN_VK_SCALED),
             EF(VK_FORMAT_R16G16_SSCALED, F, NVN_VK_SCALED),
             G(0, VK_FORMAT_R32G32_UINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID), G(0, VK_FORMAT_R32G32_SINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID),
    [0x5D] = EF(VK_FORMAT_R8G8B8_USCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R8G8B8_SSCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R16G16B16_USCALED, F, NVN_VK_SCALED),
             EF(VK_FORMAT_R16G16B16_SSCALED, F, NVN_VK_SCALED),
             G(0, VK_FORMAT_R32G32B32_UINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID), G(0, VK_FORMAT_R32G32B32_SINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID),
    [0x63] = EF(VK_FORMAT_R8G8B8A8_USCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R8G8B8A8_SSCALED, F, NVN_VK_SCALED), EF(VK_FORMAT_R16G16B16A16_USCALED, F, NVN_VK_SCALED),
             EF(VK_FORMAT_R16G16B16A16_SSCALED, F, NVN_VK_SCALED),
             G(0, VK_FORMAT_R32G32B32A32_UINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID), G(0, VK_FORMAT_R32G32B32A32_SINT, 0, C, F, NVN_VK_SCALED, ID, W_SCALED32, ID),
    [0x69] = G(VK_FORMAT_A2B10G10R10_SNORM_PACK32, 0, 1, C, F, NVN_VK_NEEDS_EXT, ID, "optional support: where the device lacks it the texture is not created (an unpack to RGBA16_SNORM at upload is phase P7)", ID),
             G(VK_FORMAT_A2B10G10R10_SINT_PACK32, 0, 1, C, I, NVN_VK_NEEDS_EXT, ID, "optional support: where the device lacks it the texture is not created (an unpack to RGBA16_SINT at upload is phase P7)", ID),
             EF(VK_FORMAT_A2B10G10R10_USCALED_PACK32, F, NVN_VK_SCALED), EF(VK_FORMAT_A2B10G10R10_SSCALED_PACK32, F, NVN_VK_SCALED),
    [0x6D] = X(VK_FORMAT_R8G8B8A8_UNORM, F), X(VK_FORMAT_R8G8B8A8_SNORM, F), X(VK_FORMAT_R8G8B8A8_UINT, U), X(VK_FORMAT_R8G8B8A8_SINT, I),
    [0x71] = X(VK_FORMAT_R16G16B16A16_SFLOAT, F), X(VK_FORMAT_R16G16B16A16_UNORM, F), X(VK_FORMAT_R16G16B16A16_SNORM, F), X(VK_FORMAT_R16G16B16A16_UINT, U),
             X(VK_FORMAT_R16G16B16A16_SINT, I),
    [0x76] = X(VK_FORMAT_R32G32B32A32_SFLOAT, F), X(VK_FORMAT_R32G32B32A32_UINT, U), X(VK_FORMAT_R32G32B32A32_SINT, I),
    // 0x79-0x94 ASTC: computed below (NVN's block order is Vulkan's)
    [0x95] = E(VK_FORMAT_R5G6B5_UNORM_PACK16, F),   // BGR565: red in bits 11-15, the mirror of RGB565
    [0x96] = G(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, 2, C, F, NVN_VK_ORDER, NA, W_ORDER, NA), G(VK_FORMAT_A1R5G5B5_UNORM_PACK16, 0, 2, C, F, NVN_VK_ORDER, ID, W_ORDER, ID),
             G(VK_FORMAT_R5G5B5A1_UNORM_PACK16, 0, 2, C, F, NVN_VK_ORDER, ID, W_ORDER, ID),
    [0x99] = X(VK_FORMAT_B8G8R8A8_UNORM, F), E(VK_FORMAT_B8G8R8A8_UNORM, F), XF(VK_FORMAT_B8G8R8A8_SRGB, F, NVN_VK_SRGB), EF(VK_FORMAT_B8G8R8A8_SRGB, F, NVN_VK_SRGB),
};

static const char *const NAMES[0x9D] = {
    [0x01] = "R8", "R8SN", "R8UI", "R8I", "R16F", "R16", "R16SN", "R16UI", "R16I", "R32F", "R32UI", "R32I", "RG8", "RG8SN", "RG8UI", "RG8I", "RG16F", "RG16", "RG16SN",
    "RG16UI", "RG16I", "RG32F", "RG32UI", "RG32I", "RGB8", "RGB8SN", "RGB8UI", "RGB8I", "RGB16F", "RGB16", "RGB16SN", "RGB16UI", "RGB16I", "RGB32F", "RGB32UI", "RGB32I",
    "RGBA8", "RGBA8SN", "RGBA8UI", "RGBA8I", "RGBA16F", "RGBA16", "RGBA16SN", "RGBA16UI", "RGBA16I", "RGBA32F", "RGBA32UI", "RGBA32I", "STENCIL8", "DEPTH16", "DEPTH24",
    "DEPTH32F", "DEPTH24_STENCIL8", "DEPTH32F_STENCIL8", "RGBX8_SRGB", "RGBA8_SRGB", "RGBA4", "RGB5", "RGB5A1", "RGB565", "RGB10A2", "RGB10A2UI", "R11G11B10F", "RGB9E5F",
    "RGB_DXT1", "RGBA_DXT1", "RGBA_DXT3", "RGBA_DXT5", "RGB_DXT1_SRGB", "RGBA_DXT1_SRGB", "RGBA_DXT3_SRGB", "RGBA_DXT5_SRGB", "RGTC1_UNORM", "RGTC1_SNORM", "RGTC2_UNORM",
    "RGTC2_SNORM", "BPTC_UNORM", "BPTC_UNORM_SRGB", "BPTC_SFLOAT", "BPTC_UFLOAT", "R8_UI2F", "R8_I2F", "R16_UI2F", "R16_I2F", "R32_UI2F", "R32_I2F", "RG8_UI2F", "RG8_I2F",
    "RG16_UI2F", "RG16_I2F", "RG32_UI2F", "RG32_I2F", "RGB8_UI2F", "RGB8_I2F", "RGB16_UI2F", "RGB16_I2F", "RGB32_UI2F", "RGB32_I2F", "RGBA8_UI2F", "RGBA8_I2F",
    "RGBA16_UI2F", "RGBA16_I2F", "RGBA32_UI2F", "RGBA32_I2F", "RGB10A2SN", "RGB10A2I", "RGB10A2_UI2F", "RGB10A2_I2F", "RGBX8", "RGBX8SN", "RGBX8UI", "RGBX8I", "RGBX16F",
    "RGBX16", "RGBX16SN", "RGBX16UI", "RGBX16I", "RGBX32F", "RGBX32UI", "RGBX32I", "RGBA_ASTC_4x4", "RGBA_ASTC_5x4", "RGBA_ASTC_5x5", "RGBA_ASTC_6x5", "RGBA_ASTC_6x6",
    "RGBA_ASTC_8x5", "RGBA_ASTC_8x6", "RGBA_ASTC_8x8", "RGBA_ASTC_10x5", "RGBA_ASTC_10x6", "RGBA_ASTC_10x8", "RGBA_ASTC_10x10", "RGBA_ASTC_12x10", "RGBA_ASTC_12x12",
    "RGBA_ASTC_4x4_SRGB", "RGBA_ASTC_5x4_SRGB", "RGBA_ASTC_5x5_SRGB", "RGBA_ASTC_6x5_SRGB", "RGBA_ASTC_6x6_SRGB", "RGBA_ASTC_8x5_SRGB", "RGBA_ASTC_8x6_SRGB",
    "RGBA_ASTC_8x8_SRGB", "RGBA_ASTC_10x5_SRGB", "RGBA_ASTC_10x6_SRGB", "RGBA_ASTC_10x8_SRGB", "RGBA_ASTC_10x10_SRGB", "RGBA_ASTC_12x10_SRGB", "RGBA_ASTC_12x12_SRGB",
    "BGR565", "BGR5", "BGR5A1", "A1BGR5", "BGRX8", "BGRA8", "BGRX8_SRGB", "BGRA8_SRGB",
};

const char *nvn_fmt_name(int f) { return f > 0 && f < (int)(sizeof NAMES / sizeof *NAMES) && NAMES[f] ? NAMES[f] : "?"; }

int nvn_fmt_to_vk(int f, nvn_vk_fmt *out) {
    nvn_vk_fmt r = {0};
    if (f >= 0x79 && f <= 0x94) {   // ASTC: 14 block sizes LDR, then 14 sRGB; Vulkan alternates UNORM / SRGB per block size in the same block order
        int k = (f - 0x79) % 14, srgb = f >= 0x87;
        r = (nvn_vk_fmt){(uint32_t)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + srgb), 0, 1, C, F, (uint8_t)(NVN_VK_BLOCK | NVN_VK_NEEDS_EXT | (srgb ? NVN_VK_SRGB : 0)), ID, W_ASTC};
    } else if (f > 0 && f < (int)(sizeof T / sizeof *T) && (T[f].f.vk || T[f].f.alt)) r = T[f].f;
    else r.why = "not an NVN format";
    if (out) *out = r;
    return r.kind;
}

void nvn_fmt_vk_alt_swizzle(int f, uint8_t swz[4]) {
    static const uint8_t id[4] = ID;
    memcpy(swz, f > 0 && f < (int)(sizeof T / sizeof *T) && T[f].f.alt ? T[f].alt_swz : id, 4);
}
