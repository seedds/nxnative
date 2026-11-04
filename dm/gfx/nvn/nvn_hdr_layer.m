#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#include "nvn_hdr.h"
#include <math.h>
void dm_hdr_configure_layer(void *layer_ptr, double potential_headroom) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_ptr;
    int requested = dm_hdr_requested();
    dm_hdr_display(requested && layer && isfinite(potential_headroom) && potential_headroom > 1.0 &&
                   CAEDRMetadata.available);
    if (!requested || !layer) {
        return;
    }
    // Keep a single float drawable format across SDR/HDR transitions and display-link drawables.
    // HDR10 metadata maps this virtual 1000-nit mastering display to the physical display.
    layer.pixelFormat = MTLPixelFormatRGBA16Float;
    CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearITUR_2020);
    layer.colorspace = color_space;
    CGColorSpaceRelease(color_space);
    layer.wantsExtendedDynamicRangeContent = YES;
    if (!layer.EDRMetadata) {
        layer.EDRMetadata = [CAEDRMetadata HDR10MetadataWithMinLuminance:0
                                                            maxLuminance:DM_HDR_MASTERING_PEAK
                                                      opticalOutputScale:DM_HDR_LINEAR_UNIT];
    }
}
void dm_hdr_layer_metadata(void *layer_ptr, int hdr_content) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_ptr;
    if (!layer || layer.pixelFormat != MTLPixelFormatRGBA16Float || !dm_hdr_requested()) {
        return;
    }
    uint16_t min_luminance = 0, max_luminance = 100;
    if (hdr_content) {
        dm_hdr_luminance_range(1, &min_luminance, &max_luminance);
    }
    static __weak CAMetalLayer *last_layer;
    static uint32_t last_range;
    uint32_t range = min_luminance | (uint32_t)max_luminance << 16;
    if (last_layer == layer && last_range == range) {
        return;
    }
    layer.EDRMetadata = [CAEDRMetadata HDR10MetadataWithMinLuminance:min_luminance
                                                        maxLuminance:max_luminance
                                                  opticalOutputScale:DM_HDR_LINEAR_UNIT];
    last_layer = layer;
    last_range = range;
}
