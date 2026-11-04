#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
/* Shared measurement routine: callers supply exact little-endian FP32 words. */
NSData *MetalConversionMeasure(NSData *input, NSString **deviceName) {
 if(!input || input.length%4 || !input.length){fprintf(stderr,"invalid input\n");return nil;}
 id<MTLDevice> d=MTLCreateSystemDefaultDevice(); if(!d){fprintf(stderr,"MTLCreateSystemDefaultDevice returned nil\n");return nil;}
 *deviceName=d.name;
 NSString *src=@"#include <metal_stdlib>\nusing namespace metal; struct V { float4 p [[position]]; }; vertex V vs(uint id [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return {float4(p[id],0,1)}; } fragment float4 fs(V in [[stage_in]], device const float *v [[buffer(0)]]) { float x=v[uint(in.p.x)]; return float4(x,x,x,1); }";
 NSError *e=nil; id<MTLLibrary> lib=[d newLibraryWithSource:src options:nil error:&e]; if(!lib){fprintf(stderr,"library: %s\n",e.localizedDescription.UTF8String);return nil;}
 MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=[lib newFunctionWithName:@"fs"];pd.colorAttachments[0].pixelFormat=MTLPixelFormatRG11B10Float;
 id<MTLRenderPipelineState> ps=[d newRenderPipelineStateWithDescriptor:pd error:&e];if(!ps){fprintf(stderr,"pipeline: %s\n",e.localizedDescription.UTF8String);return nil;}
 NSUInteger count=input.length/4, width=256, height=(count+width-1)/width; if(height!=1){fprintf(stderr,"too many cases: %lu\n",(unsigned long)count);return nil;}
 MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG11B10Float width:width height:height mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;
 id<MTLTexture> tex=[d newTextureWithDescriptor:td];id<MTLBuffer> in=[d newBufferWithLength:width*4 options:MTLResourceStorageModeShared];id<MTLBuffer> out=[d newBufferWithLength:width*4 options:MTLResourceStorageModeShared]; if(!tex||!in||!out){fprintf(stderr,"resource allocation failed\n");return nil;} memset(in.contents,0,width*4); memcpy(in.contents,input.bytes,input.length);
 id<MTLCommandQueue> q=[d newCommandQueue];id<MTLCommandBuffer> cb=[q commandBuffer]; if(!q || !cb){fprintf(stderr,"command creation failed\n");return nil;} MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=tex;rp.colorAttachments[0].loadAction=MTLLoadActionDontCare;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLRenderCommandEncoder> re=[cb renderCommandEncoderWithDescriptor:rp];if(!re){fprintf(stderr,"render encoder failed\n");return nil;} [re setRenderPipelineState:ps];[re setFragmentBuffer:in offset:0 atIndex:0];[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[re endEncoding];
 id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];if(!bl){fprintf(stderr,"blit encoder failed\n");return nil;} [bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,1,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:width*4 destinationBytesPerImage:width*4];[bl endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"command buffer: %s\n",cb.error.localizedDescription.UTF8String);return nil;}
 return [NSData dataWithBytes:out.contents length:count*4];
}
#ifndef METAL_CONVERSION_NO_MAIN
int main(int argc, const char **argv) { @autoreleasepool {
 if(argc!=3){fprintf(stderr,"usage: probe inputs.bin output.bin\n");return 2;}
 NSString *inputPath=[NSString stringWithUTF8String:argv[1]], *outputPath=[NSString stringWithUTF8String:argv[2]];
 if(!inputPath || !outputPath){fprintf(stderr,"paths must be UTF-8\n");return 2;}
 NSData *input=[NSData dataWithContentsOfFile:inputPath];
 if(!input){fprintf(stderr,"cannot read input file\n");return 1;}
 NSString *device=nil; NSData *result=MetalConversionMeasure(input,&device);
 if(!result) return 1;
 if(![result writeToFile:outputPath atomically:YES]){fprintf(stderr,"write failed\n");return 1;}
 fprintf(stdout,"%s\n",device.UTF8String); return 0;
} }
#endif
