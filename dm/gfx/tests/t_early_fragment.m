// A generated early-test shader must not update storage for depth-rejected pixels.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, char **argv) { @autoreleasepool {
    if (argc!=2) return 2;
    id<MTLDevice> dev=MTLCreateSystemDefaultDevice(); if (!dev) { fprintf(stderr,"no Metal device\n"); return 2; }
    NSError *error=nil; id<MTLLibrary> lib=[dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    if (!lib) { fprintf(stderr,"%s\n",error.description.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"vs"]; pd.fragmentFunction=[lib newFunctionWithName:@"main0"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm; pd.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> ps=[dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!ps) { fprintf(stderr,"%s\n",error.description.UTF8String); return 2; }
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO]; td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> color=[dev newTextureWithDescriptor:td]; td.pixelFormat=MTLPixelFormatDepth32Float; id<MTLTexture> depth=[dev newTextureWithDescriptor:td];
    id<MTLBuffer> counter=[dev newBufferWithLength:4 options:MTLResourceStorageModeShared]; id<MTLCommandQueue> queue=[dev newCommandQueue]; int failed=0;
    for (int accept=0;accept<2;accept++) {
        *(uint32_t *)counter.contents=0;
        MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new]; ds.depthCompareFunction=accept?MTLCompareFunctionAlways:MTLCompareFunctionNever;
        MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor new]; rp.colorAttachments[0].texture=color; rp.colorAttachments[0].loadAction=MTLLoadActionClear; rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        rp.depthAttachment.texture=depth; rp.depthAttachment.loadAction=MTLLoadActionClear; rp.depthAttachment.clearDepth=1; rp.depthAttachment.storeAction=MTLStoreActionDontCare;
        id<MTLCommandBuffer> cb=[queue commandBuffer]; id<MTLRenderCommandEncoder> en=[cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setDepthStencilState:[dev newDepthStencilStateWithDescriptor:ds]]; [en setFragmentBuffer:counter offset:0 atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint32_t expected=accept?4:0, got=*(uint32_t *)counter.contents;
        if (cb.error || got!=expected) { failed++; fprintf(stderr,"early depth %s: expected %u writes got %u (%s)\n",accept?"accept":"reject",expected,got,cb.error.description.UTF8String); }
    }
    printf("early fragment storage-side-effect ordering: 2 checks, %d failed\n",failed); return failed!=0;
} }
