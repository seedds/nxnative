// Real-GPU transfer tests with known ST 2084 luminances, gamut vectors and an unchanged SDR path.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <math.h>
#include <stdio.h>
#include "../nvn/host_msl.h"
#include "../nvn/nvn_hdr.h"
static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } } while (0)
static float pq(double nits) {
    double p = pow(nits / 10000., 2610. / 16384.);
    return pow((3424. / 4096. + (2413. / 128.) * p) / (1. + (2392. / 128.) * p), 2523. / 32.);
}
static void render(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> pso, id<MTLSamplerState> sampler,
                   const float rgba[4], int input, int edr, float out[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> src = [dev newTextureWithDescriptor:td];
    [src replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:rgba bytesPerRow:16];
    td.usage = MTLTextureUsageRenderTarget; id<MTLTexture> dst = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    float constants[12] = {0,0,1,1, .5,.5,.5,.5, input,edr,0,1000};
    [en setRenderPipelineState:pso]; [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:sampler atIndex:0];
    [en setFragmentBytes:constants length:sizeof constants atIndex:0]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status == MTLCommandBufferStatusCompleted, "GPU completion");
    [dst getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
}
int main(void) { @autoreleasepool {
    setenv("DM_HDR", "1", 1); dm_hdr_display(0); CHECK(!dm_hdr_query(), "SDR screen does not advertise HDR");
    dm_hdr_display(1); CHECK(dm_hdr_query(), "capable screen advertises HDR");
    dm_hdr_display(0); CHECK(!dm_hdr_query() && dm_hdr_advertised(), "screen loss stops advertising but queued HDR retains decoding");
    dm_hdr_luminance(1, 0, 800); uint16_t lo, hi; dm_hdr_luminance_range(1, &lo, &hi); CHECK(lo == 0 && hi == 800, "content peak metadata retained");
    dm_hdr_luminance(1, 900, 800); dm_hdr_luminance_range(1, &lo, &hi); CHECK(hi == 800, "invalid luminance range rejected");
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); CHECK(dev != nil, "Metal device"); if (!dev) return 1;
    CAMetalLayer *layer = [CAMetalLayer layer]; layer.device = dev; dm_hdr_configure_layer((__bridge void *)layer, 2);
    CHECK(layer.pixelFormat == MTLPixelFormatRGBA16Float && layer.wantsExtendedDynamicRangeContent && layer.colorspace && layer.EDRMetadata, "EDR layer configured");
    dm_hdr_configure_layer((__bridge void *)layer, 1); CHECK(!dm_hdr_query(), "non-EDR screen remains SDR");
    id<MTLCommandQueue> queue = [dev newCommandQueue]; NSError *error = nil;
    id<MTLLibrary> lib = host_msl_lib(dev, nil, "present", &error); CHECK(lib != nil, "present metallib"); if (!lib) return 1;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float; id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
    CHECK(pso != nil, "present pipeline"); if (!pso) return 1;
    id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    float result[4];
    for (int i = 0; i < 6; i++) {
        double nits[] = {0, .1, 80, 203, 1000, 10000}; float encoded = pq(nits[i]); float in[] = {encoded, encoded, encoded, 1};
        render(dev, queue, pso, sampler, in, 1, 1, result);
        double actual = result[0] * 80; printf("PQ %.2f nits -> %.3f nits\n", nits[i], actual);
        CHECK(fabs(actual - nits[i]) < fmax(.01, nits[i] * .001), "PQ absolute luminance"); CHECK(result[3] == 1, "alpha preserved");
    }
    float sdr[] = {.1,.5,.9,.75}; render(dev, queue, pso, sampler, sdr, 0, 0, result);
    uint32_t sdr_bits[4], result_bits[4]; memcpy(sdr_bits, sdr, sizeof sdr_bits); memcpy(result_bits, result, sizeof result_bits);
    CHECK(!memcmp(sdr_bits, result_bits, sizeof sdr_bits), "legacy SDR byte-for-byte pass-through");
    float white[] = {1,1,1,1}; render(dev, queue, pso, sampler, white, 0, 1, result);
    CHECK(fabs(result[0] * 80 - 100) < .001, "SDR white maps to 100 nits");
    float red[] = {1,0,0,1}; render(dev, queue, pso, sampler, red, 0, 1, result);
    CHECK(fabs(result[0] - .784254875) < 1e-5 && fabs(result[1] - .086371625) < 1e-5, "Rec.709 red transformed into Rec.2020");
    float linear[] = {12.5,12.5,12.5,1}; render(dev, queue, pso, sampler, linear, 2, 1, result);
    CHECK(fabs(result[0] - 12.5) < .001, "linear 80-nit unit preserves 1000-nit highlights");
    printf("t_present_hdr: %d checks, %d failures\n", checks, fails); return fails != 0;
} }
