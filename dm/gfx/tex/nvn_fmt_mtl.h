// NVNformat → Metal pixel format + component remapping. Pure C, no dependency on Metal headers (values taken from the SDK's MTLPixelFormat.h / MTLTexture.h, iOS 26.5 SDK).
#pragma once
#include <stdint.h>

// MTLTextureSwizzle values
enum { NVN_SWZ_ZERO = 0, NVN_SWZ_ONE = 1, NVN_SWZ_R = 2, NVN_SWZ_G = 3, NVN_SWZ_B = 4, NVN_SWZ_A = 5 };

// Returns: 1 = exact match (data uploaded as is);
//          2 = substitute (precision / bit order / read mode differs: upload needs conversion or the shader must read differently; per-format handling is in the table comments in nvn_fmt_mtl.c);
//          0 = no Metal counterpart (*mtl is set to 0; handling is in the table comments, mainly expanding 3-component formats to 4 components on upload).
// swz gives, for each of the four output components r/g/b/a, which source component to take (MTLTextureSwizzleChannels order); may be NULL.
int nvn_fmt_to_mtl(int nvn_format, uint32_t *mtl_pixel_format, uint8_t swz[4]);

// Inverse of nvn_fmt_to_mtl: the NVN formats whose forward map gives Metal pixel format mtl_pixel_format (and, when swz is not NULL, also that component remapping).
// The forward map is not injective (DEPTH24 and DEPTH32F both give Depth32Float, RGBA8 and RGBX8 both give RGBA8Unorm, ...), so the result is a list: any candidate f' satisfies
// nvn_fmt_to_mtl(f') == mtl_pixel_format (with the same swizzle when swz is given), which is all a Metal pipeline key can tell apart. The candidates are ordered by preference:
// exact (kind 1) before substitute (kind 2), identity remapping before a no-alpha or r/b-swapped one, then by NVN format number; out[0] is the one a decoder takes.
// Writes at most max of them to out (may be NULL with max 0) and returns how many there are: 0 when no NVN format gives that Metal format (and always for format 0), more than 1 when
// the NVN format is ambiguous (a Vulkan backend, which can represent the NVN format exactly, should report that the choice is a guess).
int nvn_fmt_from_mtl(uint32_t mtl_pixel_format, const uint8_t swz[4], uint16_t *out, int max);
