#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#include "nvn_hdr.h"
#include <math.h>
void dm_hdr_configure_layer(void *ptr, double headroom) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ptr;
    int requested = dm_hdr_requested();
    dm_hdr_display(requested && layer && isfinite(headroom) && headroom > 1.0 && CAEDRMetadata.available);
    if (!requested || !layer) return;
    // Keep a single float drawable format across SDR/HDR transitions and display-link drawables.
    // HDR10 metadata maps this virtual 1000-nit mastering display to the physical display.
    layer.pixelFormat = MTLPixelFormatRGBA16Float;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
    layer.colorspace = cs; CGColorSpaceRelease(cs);
    layer.wantsExtendedDynamicRangeContent = YES;
    if (!layer.EDRMetadata) layer.EDRMetadata = [CAEDRMetadata HDR10MetadataWithMinLuminance:0 maxLuminance:DM_HDR_MASTERING_PEAK opticalOutputScale:DM_HDR_LINEAR_UNIT];
}
void dm_hdr_layer_metadata(void *ptr, int hdr_content) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ptr;
    if (!layer || layer.pixelFormat != MTLPixelFormatRGBA16Float || !dm_hdr_requested()) return;
    uint16_t lo = 0, hi = 100;
    if (hdr_content) dm_hdr_luminance_range(1, &lo, &hi);
    static __weak CAMetalLayer *last_layer;
    static uint32_t last_range;
    uint32_t range = lo | (uint32_t)hi << 16;
    if (last_layer == layer && last_range == range) return;
    layer.EDRMetadata = [CAEDRMetadata HDR10MetadataWithMinLuminance:lo maxLuminance:hi opticalOutputScale:DM_HDR_LINEAR_UNIT];
    last_layer = layer; last_range = range;
}
