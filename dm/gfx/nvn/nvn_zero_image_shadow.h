#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
// Shared by the renderer and readback probe. Only these uncompressed formats
// encode a numeric four-component zero as all-zero 32-bit words.
static NSUInteger nvn_zero_image_bytes(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR32Float: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRG11B10Float: return 4;
    case MTLPixelFormatRG32Float: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: return 8;
    case MTLPixelFormatRGBA32Float: case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: return 16;
    default: return 0;
    }
}
// ABI: width, height, layers, words/pixel, words/row, words/image.
static int nvn_zero_image_layout(id<MTLTexture> texture, uint32_t layout[6], NSUInteger *bytes) {
    NSUInteger pixel = nvn_zero_image_bytes(texture.pixelFormat), w = texture.width, h = texture.height, n = texture.arrayLength;
    if (!texture || texture.textureType != MTLTextureType2DArray || texture.sampleCount != 1 ||
        !pixel || !w || !h || !n || w > UINT32_MAX || h > UINT32_MAX || n > UINT32_MAX ||
        w > (NSUIntegerMax - 255) / pixel) return 0;
    NSUInteger row = (w * pixel + 255) & ~(NSUInteger)255;
    if (row > NSUIntegerMax / h) return 0;
    NSUInteger image = row * h;
    if (image > NSUIntegerMax / n || image / 4 > UINT32_MAX || image * n / 4 > UINT32_MAX) return 0;
    layout[0] = (uint32_t)w; layout[1] = (uint32_t)h; layout[2] = (uint32_t)n;
    layout[3] = (uint32_t)(pixel / 4); layout[4] = (uint32_t)(row / 4); layout[5] = (uint32_t)(image / 4);
    *bytes = image * n;
    return 1;
}
static void nvn_zero_image_copy(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture,
        id<MTLBuffer> buffer, const uint32_t layout[6], BOOL to_texture) {
    NSUInteger row = (NSUInteger)layout[4] * 4, image = (NSUInteger)layout[5] * 4;
    for (NSUInteger layer = 0; layer < layout[2]; layer++) {
        if (to_texture)
            [blit copyFromBuffer:buffer sourceOffset:layer * image sourceBytesPerRow:row sourceBytesPerImage:image
                sourceSize:MTLSizeMake(layout[0],layout[1],1) toTexture:texture destinationSlice:layer destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        else
            [blit copyFromTexture:texture sourceSlice:layer sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(layout[0],layout[1],1) toBuffer:buffer destinationOffset:layer * image destinationBytesPerRow:row destinationBytesPerImage:image];
    }
}
