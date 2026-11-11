// Actual Metal execution of camera-only motion: identity and signed camera translations.
// This does not validate object motion or a game's depth convention.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <simd/simd.h>
#include <math.h>
typedef struct { simd_float4 inverse[4], previous[4]; } Params;
int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }
    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:@"../msl/camera_motion.metal" encoding:NSUTF8StringEncoding error:&error];
    id<MTLLibrary> library = source ? [dev newLibraryWithSource:source options:nil error:&error] : nil;
    id<MTLComputePipelineState> pipelines[2];
    NSString *entries[2]={@"camera_motion",@"camera_motion_composed"};
    for(int mode=0;mode<2;mode++){
        id<MTLFunction> fn=[library newFunctionWithName:entries[mode]];
        pipelines[mode]=fn?[dev newComputePipelineStateWithFunction:fn error:&error]:nil;
        if(!pipelines[mode]){fprintf(stderr,"%s: %s\n",entries[mode].UTF8String,error.description.UTF8String);return 2;}
    }
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatRG16Float; td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    id<MTLTexture> output = [dev newTextureWithDescriptor:td];
    id<MTLBuffer> readback = [dev newBufferWithLength:64*64*4 options:MTLResourceStorageModeShared];
    const float near = .1f, far = 100.f, distance = 2.f;
    float a = -(far+near)/(far-near), b = -2*far*near/(far-near);
    Params p = {0};
    float az = .5f*(a-1), bz = .5f*b; // GL [-1,1] projection converted to the depth texture's Metal [0,1]
    p.inverse[0] = (simd_float4){1,0,0,0}; p.inverse[1] = (simd_float4){0,1/1.7f,0,0};
    p.inverse[2] = (simd_float4){0,0,0,-1}; p.inverse[3] = (simd_float4){0,0,1/bz,az/bz};
    p.previous[0] = (simd_float4){1,0,0,0}; p.previous[1] = (simd_float4){0,1.7f,0,0};
    p.previous[2] = (simd_float4){0,0,az,bz}; p.previous[3] = (simd_float4){0,0,-1,0};
    int failed = 0;
    for (int iteration = 0; iteration < 12; iteration++) {
        int mode=iteration/6,trial=iteration%6;
        float shift = trial == 1 ? .1f : trial == 2 ? -.1f : 0;
        float vertical = trial == 3 ? .1f : 0;
        float jitter = trial == 4 ? .03f : 0;
        float depth_a = trial == 5 ? -1-az : az, depth_b = trial == 5 ? -bz : bz;
        p.inverse[0][3] = jitter;
        p.inverse[3] = (simd_float4){0,0,1/depth_b,depth_a/depth_b};
        p.previous[0][3] = shift; p.previous[1][3] = vertical*1.7f;
        p.previous[2] = (simd_float4){0,0,depth_a,depth_b};

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = -depth_a + depth_b/distance;
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipelines[mode]]; [enc setTexture:depth atIndex:0]; [enc setTexture:output atIndex:1];
        float composed[16];
        for(int row=0;row<4;row++)for(int column=0;column<4;column++){
            double v=0;for(int k=0;k<4;k++)v+=(double)p.previous[row][k]*p.inverse[k][column];
            composed[4*row+column]=(float)v;
        }
        if(mode)[enc setBytes:composed length:sizeof composed atIndex:0];
        else [enc setBytes:&p length:sizeof p atIndex:0];
        [enc dispatchThreads:MTLSizeMake(64,64,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [enc endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,64,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:64*256];
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); return 3; }
        const __fp16 *values = readback.contents; double worst = 0;
        float expected = shift * 32.f / distance + jitter*32, expected_y = -vertical*1.7f*32/distance;
        for (int i = 0; i < 64*64; i++) {
            if (!isfinite(values[i*2]) || !isfinite(values[i*2+1])) { failed++; break; }
            worst = fmax(worst, fabs(values[i*2]-expected)); worst = fmax(worst, fabs(values[i*2+1]-expected_y));
        }
        if (worst > .003) failed++;
        printf("camera motion mode %d trial %d: expected (%+g,%+g) px, max error %.6f\n", mode, trial, expected, expected_y, worst);
    }
    return failed ? 1 : 0;
} }
