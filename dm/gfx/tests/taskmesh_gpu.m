// Native object/mesh regression, fed only generated Ampere fixtures.
// Check task payload + first, child count/index, sparse attributes, vertex 32,
// MAP counter/indices, early guest return and both clip-depth entries.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../nvn/dmsh_abi.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 7) {
            return 2;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device || ![device supportsFamily:MTLGPUFamilyApple7]) {
            return 77;
        }
        NSError *error = nil;
        id<MTLLibrary> libs[3];
        for (int i = 0; i < 3; i++) {
            libs[i] = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[i + 1])] error:&error];
            if (!libs[i]) {
                fprintf(stderr, "%s\n", error.description.UTF8String);
                return 1;
            }
        }
        NSUInteger threads = strtoul(argv[4], NULL, 10), payload = strtoul(argv[5], NULL, 10);
        int primitive = atoi(argv[6]);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:32
                                                                                     height:32
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        unsigned cases = 0;
        for (int direct = 0; direct < 2; direct++) {
            for (int zero = 0; zero < 2; zero++) {
                MTLMeshRenderPipelineDescriptor *pd = [MTLMeshRenderPipelineDescriptor new];
                if (!direct) {
                    pd.objectFunction = [libs[0] newFunctionWithName:@"main0"];
                }
                NSString *entry =
                    direct ? (zero ? @"main0_direct_z" : @"main0_direct") : (zero ? @"main0_z" : @"main0");
                pd.meshFunction = [libs[direct ? 2 : 1] newFunctionWithName:entry];
                pd.fragmentFunction = [libs[direct ? 2 : 1] newFunctionWithName:@"pixel"];
                pd.colorAttachments[0].pixelFormat = td.pixelFormat;
                if (!direct) {
                    pd.maxTotalThreadsPerObjectThreadgroup = threads;
                }
                pd.maxTotalThreadsPerMeshThreadgroup = threads;
                pd.maxTotalThreadgroupsPerMeshGrid = 2;
                if (!direct) {
                    pd.payloadMemoryLength = payload;
                }
                id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithMeshDescriptor:pd
                                                                                               options:0
                                                                                            reflection:nil
                                                                                                 error:&error];
                if (!pipeline) {
                    fprintf(stderr, "threads=%lu direct=%d zero=%d: %s\n", (unsigned long)threads, direct, zero,
                            error.description.UTF8String);
                    return 1;
                }
                for (uint32_t first = 7; first <= 8; first++) {
                    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
                    pass.colorAttachments[0].texture = target;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                    id<MTLCommandBuffer> command = [queue commandBuffer];
                    id<MTLRenderCommandEncoder> enc = [command renderCommandEncoderWithDescriptor:pass];
                    [enc setRenderPipelineState:pipeline];
                    [enc setFrontFacingWinding:MTLWindingCounterClockwise];
                    [enc setCullMode:MTLCullModeBack];
                    if (direct) {
                        [enc setMeshBytes:&first length:sizeof first atIndex:DMSH_MESH_FIRST_SLOT];
                    } else {
                        [enc setObjectBytes:&first length:sizeof first atIndex:DMSH_MESH_FIRST_SLOT];
                    }
                    [enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
                        threadsPerObjectThreadgroup:MTLSizeMake(threads, 1, 1)
                          threadsPerMeshThreadgroup:MTLSizeMake(threads, 1, 1)];
                    [enc endEncoding];
                    [command commit];
                    [command waitUntilCompleted];
                    if (command.error) {
                        fprintf(stderr, "%s\n", command.error.description.UTF8String);
                        return 1;
                    }
                    uint8_t pixels[32][32][4];
                    [target getBytes:pixels bytesPerRow:32 * 4 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0];
                    // Triangle centres and an otherwise unused top-centre sentinel.
                    const int points[3][2] = {{8, 16}, {24, 16}, {16, 1}};
                    for (int point = 0; point < 3; point++) {
                        int visible = !zero && (point == 0 || (point == 1 && !direct));
                        const uint8_t *p = pixels[points[point][1]][points[point][0]];
                        int expected[4] = {first == 7 ? 255 : 0, 128, 191, 255};
                        if (primitive) {
                            expected[0] = 64;
                            expected[1] = 191;
                            expected[2] = first == 7 ? 255 : 0;
                        }
                        for (int c = 0; c < 4; c++) {
                            if (abs((int)p[c] - (visible ? expected[c] : 0)) > 1) {
                                fprintf(stderr,
                                        "threads=%lu primitive=%d direct=%d zero=%d first=%u point=%d channel=%d: "
                                        "got=%u expected=%d\n",
                                        (unsigned long)threads, primitive, direct, zero, first, point, c, p[c],
                                        visible ? expected[c] : 0);
                                return 1;
                            }
                        }
                    }
                    cases++;
                }
            }
        }
        printf("task/mesh %lu threads, primitive=%d: %u native pixel cases passed\n", (unsigned long)threads, primitive,
               cases);
        return 0;
    }
}
