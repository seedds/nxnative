// BC textures on the CPU, for GPUs that cannot sample them. A Metal device that reports no BC support
// (MTLDevice.supportsBCTextureCompression is NO on Apple GPUs before the A19: A15 / A16 and older iPhones
// and iPads) returns nil for a texture made in a BC pixel format, so every BC texture of a game would be
// missing. There the replay layer creates such a texture in the uncompressed format below and fills it
// with the blocks decoded here. Pure C (the decoder is dm/third_party/bcdec), no Metal headers.
#pragma once
#include <stddef.h>
#include <stdint.h>

// NVN format is BC1-BC7 (0x41-0x50): returns 1, *mtl_pf = the Metal pixel format of the decoded texels
// (RGBA8Unorm, RGBA8Unorm_sRGB, R8 / RG8 Unorm or Snorm for BC4 / BC5, RGBA16Float for BC6H),
// *bpp = its bytes per texel. Not a BC format: returns 0 and leaves the outputs alone.
int nvn_bc_unpacked(uint32_t nvn_fmt, uint32_t *mtl_pf, int *bpp);

// Decodes w x h texels of one level and layer (one depth slice): src holds the blocks, `pitch` bytes per
// row of blocks; dst is the texel image, `dpitch` bytes per row (at least w * bpp). Edge blocks are decoded
// whole and only the texels inside w x h are written. Returns 0, or -1 when nvn_fmt is not a BC format.
int nvn_bc_decode(uint32_t nvn_fmt, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch);

// Whether BC textures are decoded on the CPU: when the device cannot sample them, or when `force` (the
// DM_TEST_NO_BC environment value, may be NULL) is set and not "0": the way to try the path on a Mac.
int nvn_bc_cpu_wanted(int device_has_bc, const char *force);
