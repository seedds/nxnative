// Real-GPU transfer tests with known ST 2084 luminances, gamut vectors and an unchanged SDR path.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/host_msl.h"
#include "../nvn/nvn_hdr.h"
static int fails, checks;
#define CHECK(c, msg)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s\n", msg);                                                                                  \
        }                                                                                                              \
    } while (0)
static float pq(double nits) {
    double p = pow(nits / 10000., 2610. / 16384.);
    return pow((3424. / 4096. + (2413. / 128.) * p) / (1. + (2392. / 128.) * p), 2523. / 32.);
}
static void render(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline,
                   id<MTLSamplerState> sampler, const float rgba[4], int input, int edr, float out[4]) {
    MTLTextureDescriptor *texture_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    texture_desc.usage = MTLTextureUsageShaderRead;
    texture_desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> src = [device newTextureWithDescriptor:texture_desc];
    [src replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:rgba bytesPerRow:16];
    texture_desc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> dst = [device newTextureWithDescriptor:texture_desc];
    MTLRenderPassDescriptor *pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    pass_desc.colorAttachments[0].texture = dst;
    pass_desc.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass_desc];
    float constants[12] = {0, 0, 1, 1, .5, .5, .5, .5, input, edr, 0, 1000};
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:src atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];
    [encoder setFragmentBytes:constants length:sizeof constants atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    CHECK(command_buffer.status == MTLCommandBufferStatusCompleted, "GPU completion");
    [dst getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
}
int main(void) {
    @autoreleasepool {
        unsetenv("DM_HDR");
        dm_hdr_display(1);
        CHECK(!dm_hdr_query() && !dm_hdr_advertised(), "HDR requires explicit opt-in");
        setenv("DM_HDR", "0", 1);
        CHECK(!dm_hdr_query(), "explicit SDR opt-out");
        setenv("DM_HDR", "1", 1);
        dm_hdr_display(0);
        CHECK(!dm_hdr_query(), "SDR screen does not advertise HDR");
        dm_hdr_display(1);
        CHECK(dm_hdr_query(), "capable screen advertises HDR");
        dm_hdr_display(0);
        CHECK(!dm_hdr_query() && dm_hdr_advertised(), "screen loss stops advertising but queued HDR retains decoding");
        dm_hdr_luminance(1, 0, 800);
        uint16_t min_luminance, max_luminance;
        dm_hdr_luminance_range(1, &min_luminance, &max_luminance);
        CHECK(min_luminance == 0 && max_luminance == 800, "content peak metadata retained");
        dm_hdr_luminance(1, 900, 800);
        dm_hdr_luminance_range(1, &min_luminance, &max_luminance);
        CHECK(max_luminance == 800, "invalid luminance range rejected");
        dm_hdr_luminance(1, 0, 0);
        dm_hdr_luminance_range(1, &min_luminance, &max_luminance);
        CHECK(max_luminance == 800, "zero peak is rejected");
        dm_hdr_luminance(0, 1, 203);
        dm_hdr_luminance_range(0, &min_luminance, &max_luminance);
        CHECK(min_luminance == 1 && max_luminance == 203, "standard luminance range is independent");
        dm_hdr_gamut(1);
        dm_hdr_gamut(7);
        CHECK(dm_hdr_content_gamut() == 1, "invalid gamut does not replace metadata");
        dm_hdr_gamut(-1);
        CHECK(dm_hdr_content_gamut() == -1, "gamut metadata can be cleared");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        CHECK(device != nil, "Metal device");
        if (!device) {
            return 1;
        }
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = device;
        dm_hdr_configure_layer((__bridge void *)layer, 2);
        CHECK(layer.pixelFormat == MTLPixelFormatRGBA16Float && layer.wantsExtendedDynamicRangeContent &&
                  layer.colorspace && layer.EDRMetadata,
              "EDR layer configured");
        dm_hdr_configure_layer((__bridge void *)layer, 1);
        CHECK(!dm_hdr_query(), "non-EDR screen remains SDR");
        dm_hdr_configure_layer((__bridge void *)layer, NAN);
        CHECK(!dm_hdr_query(), "unknown display headroom does not advertise HDR");
        dm_hdr_configure_layer(NULL, 2);
        CHECK(!dm_hdr_query(), "missing layer does not advertise HDR");
        dm_hdr_layer_metadata((__bridge void *)layer, 1);
        CAEDRMetadata *metadata = layer.EDRMetadata;
        dm_hdr_layer_metadata((__bridge void *)layer, 1);
        CHECK(layer.EDRMetadata == metadata, "unchanged luminance reuses layer metadata");
        dm_hdr_luminance(1, 0, 1000);
        dm_hdr_layer_metadata((__bridge void *)layer, 1);
        CHECK(layer.EDRMetadata != metadata, "changed luminance replaces layer metadata");
        metadata = layer.EDRMetadata;
        dm_hdr_layer_metadata((__bridge void *)layer, 0);
        CHECK(layer.EDRMetadata != metadata, "SDR frames replace HDR mastering metadata");
        CAMetalLayer *sdr_layer = [CAMetalLayer layer];
        setenv("DM_HDR", "0", 1);
        dm_hdr_configure_layer((__bridge void *)sdr_layer, 2);
        CHECK(sdr_layer.pixelFormat == MTLPixelFormatBGRA8Unorm && !sdr_layer.wantsExtendedDynamicRangeContent,
              "opt-out leaves the SDR layer unchanged");
        setenv("DM_HDR", "1", 1);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> library = host_msl_lib(device, nil, "present", &error);
        CHECK(library != nil, "present metallib");
        if (!library) {
            return 1;
        }
        MTLRenderPipelineDescriptor *pipeline_desc = [MTLRenderPipelineDescriptor new];
        pipeline_desc.vertexFunction = [library newFunctionWithName:@"vs"];
        pipeline_desc.fragmentFunction = [library newFunctionWithName:@"fs"];
        pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
        CHECK(pipeline != nil, "present pipeline");
        if (!pipeline) {
            return 1;
        }
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        float result[4];
        for (int i = 0; i < 6; i++) {
            double nits[] = {0, .1, 80, 203, 1000, 10000};
            float encoded = pq(nits[i]);
            float in[] = {encoded, encoded, encoded, 1};
            render(device, queue, pipeline, sampler, in, 1, 1, result);
            double actual = result[0] * 80;
            printf("PQ %.2f nits -> %.3f nits\n", nits[i], actual);
            CHECK(fabs(actual - nits[i]) < fmax(.01, nits[i] * .001), "PQ absolute luminance");
            CHECK(result[3] == 1, "alpha preserved");
        }
        float sdr[] = {.1, .5, .9, .75};
        render(device, queue, pipeline, sampler, sdr, 0, 0, result);
        uint32_t sdr_bits[4], result_bits[4];
        memcpy(sdr_bits, sdr, sizeof sdr_bits);
        memcpy(result_bits, result, sizeof result_bits);
        CHECK(!memcmp(sdr_bits, result_bits, sizeof sdr_bits), "legacy SDR byte-for-byte pass-through");
        float white[] = {1, 1, 1, 1};
        render(device, queue, pipeline, sampler, white, 0, 1, result);
        CHECK(fabs(result[0] * 80 - 100) < .001, "SDR white maps to 100 nits");
        float red[] = {1, 0, 0, 1};
        render(device, queue, pipeline, sampler, red, 0, 1, result);
        CHECK(fabs(result[0] - .784254875) < 1e-5 && fabs(result[1] - .086371625) < 1e-5,
              "Rec.709 red transformed into Rec.2020");
        float linear[] = {12.5, 12.5, 12.5, 1};
        render(device, queue, pipeline, sampler, linear, 2, 1, result);
        CHECK(fabs(result[0] - 12.5) < .001, "linear 80-nit unit preserves 1000-nit highlights");
        // The SDR capture curve preserves ordering and maps the declared 1000-nit peak to white.
        const double capture_nits[] = {0, .1, 80, 203, 500, 1000};
        float previous = -1;
        for (unsigned i = 0; i < sizeof capture_nits / sizeof *capture_nits; i++) {
            float encoded = pq(capture_nits[i]);
            float input[] = {encoded, encoded, encoded, .5};
            render(device, queue, pipeline, sampler, input, 1, 0, result);
            CHECK(isfinite(result[0]) && result[0] >= 0 && result[0] <= 1.001,
                  "SDR capture has finite display-range luminance");
            CHECK(result[0] > previous, "SDR capture retains highlight ordering");
            CHECK(fabs(result[0] - result[1]) < 1e-4 && fabs(result[0] - result[2]) < 1e-4,
                  "neutral HDR luminances remain neutral in SDR");
            CHECK(result[3] == .5, "SDR capture preserves alpha");
            previous = result[0];
        }
        CHECK(fabs(result[0] - 1) < .001, "SDR capture maps the mastering peak to white");
        printf("t_present_hdr: %d checks, %d failures\n", checks, fails);
        return fails != 0;
    }
}
