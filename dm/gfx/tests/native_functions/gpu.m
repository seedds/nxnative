#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static uint32_t bits(float x) { uint32_t word; memcpy(&word,&x,4); return word; }
static float number(uint32_t word) { float x; memcpy(&x,&word,4); return x; }

static id<MTLAccelerationStructure> build(id<MTLDevice> device,id<MTLCommandQueue> queue,
                                         MTLAccelerationStructureDescriptor *descriptor) {
    MTLAccelerationStructureSizes sizes=[device accelerationStructureSizesWithDescriptor:descriptor];
    id<MTLAccelerationStructure> result=[device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    id<MTLBuffer> scratch=[device newBufferWithLength:MAX((NSUInteger)256,sizes.buildScratchBufferSize) options:MTLResourceStorageModePrivate];
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLAccelerationStructureCommandEncoder> encoder=[command accelerationStructureCommandEncoder];
    [encoder buildAccelerationStructure:result descriptor:descriptor scratchBuffer:scratch scratchBufferOffset:0];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if(command.error){fprintf(stderr,"%s\n",command.error.description.UTF8String);exit(2);}
    return result;
}

int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2)return 2;
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    if(!device || !device.supportsRaytracing)return 77;
    id<MTLCommandQueue> queue=[device newCommandQueue];
    const float vertices[]={-1,-1,1, 1,-1,1, -1,1,1};
    id<MTLBuffer> vertex=[device newBufferWithBytes:vertices length:sizeof vertices options:MTLResourceStorageModeShared];
    MTLAccelerationStructureTriangleGeometryDescriptor *geometry=[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    geometry.vertexBuffer=vertex;geometry.vertexStride=12;geometry.vertexFormat=MTLAttributeFormatFloat3;
    geometry.triangleCount=1;geometry.opaque=YES;
    MTLPrimitiveAccelerationStructureDescriptor *bottom=[MTLPrimitiveAccelerationStructureDescriptor descriptor];
    bottom.geometryDescriptors=@[geometry];
    id<MTLAccelerationStructure> blas=build(device,queue,bottom);
    const float bounds[]={-1,-1,1,1,1,2};
    id<MTLBuffer> boxes=[device newBufferWithBytes:bounds length:sizeof bounds options:MTLResourceStorageModeShared];
    MTLAccelerationStructureBoundingBoxGeometryDescriptor *box=[MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
    box.boundingBoxBuffer=boxes;box.boundingBoxStride=24;box.boundingBoxCount=1;box.opaque=YES;
    MTLPrimitiveAccelerationStructureDescriptor *procedural=[MTLPrimitiveAccelerationStructureDescriptor descriptor];
    procedural.geometryDescriptors=@[box];
    id<MTLAccelerationStructure> aabb=build(device,queue,procedural);
    MTLAccelerationStructureUserIDInstanceDescriptor instances[3]={0};
    // Column-major 4x3, deliberately different IDs and a nonuniform transform.
    const float matrices[3][12]={{1,0,0,0,1,0,0,0,1,0,0,0},{2,0,0,0,4,0,0,0,1,4,0,0},{1,0,0,0,1,0,0,0,1,10,0,0}};
    for(unsigned i=0;i<3;i++) {
        memcpy(&instances[i].transformationMatrix,matrices[i],sizeof matrices[i]);
        instances[i].mask=255;instances[i].userID=i?0x76543:0x12345;
        instances[i].accelerationStructureIndex=i==2?1:0;
    }
    id<MTLBuffer> input=[device newBufferWithBytes:instances length:sizeof instances options:MTLResourceStorageModeShared];
    MTLInstanceAccelerationStructureDescriptor *top=[MTLInstanceAccelerationStructureDescriptor descriptor];
    top.instanceDescriptorBuffer=input;top.instanceDescriptorStride=sizeof instances[0];top.instanceCount=3;
    top.instanceDescriptorType=MTLAccelerationStructureInstanceDescriptorTypeUserID;top.instancedAccelerationStructures=@[blas,aabb];
    id<MTLAccelerationStructure> tlas=build(device,queue,top);
    unsigned failures=0;
    for(NSString *kind in @[@"state",@"trace"]) for(NSString *mode in @[@"optimized",@"debug"]) {
        NSString *stem=[NSString stringWithFormat:@"%@-%@",kind,mode];
        NSString *path=[@(argv[1]) stringByAppendingPathComponent:[stem stringByAppendingString:@".metal"]];
        NSError *error=nil;MTLCompileOptions *options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion4_0;
        NSString *source=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
        id<MTLLibrary> library=[device newLibraryWithSource:source options:options error:&error];
        id<MTLFunction> function=[library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
        if(!pipeline){fprintf(stderr,"%s: %s\n",stem.UTF8String,error.localizedDescription.UTF8String);return 2;}
        BOOL trace=[kind isEqualToString:@"trace"];
        enum { GUARD=16, CAPACITY=1088 };
        unsigned rays=trace?4:2,stride=trace?64:352,words=GUARD*2+rays*stride;
        uint32_t initial[CAPACITY],expected[CAPACITY];
        for(unsigned i=0;i<words;i++)initial[i]=0xc1000000u+i*0x103u;
        if(trace) {
            const float raysIn[4][8]={{-.5f,-.5f,0,.01f,0,0,1,10},{3,-2,0,.01f,0,0,1,10},{20,0,0,.01f,0,0,1,10},{10,0,0,.01f,0,0,1,10}};
            for(unsigned i=0;i<rays;i++)for(unsigned j=0;j<8;j++)initial[GUARD+i*stride+j]=bits(raysIn[i][j]);
        }else for(unsigned i=0;i<rays;i++)initial[GUARD+i*stride]=i?200:100;
        memcpy(expected,initial,words*4);
        for(unsigned i=0;i<rays;i++) {
            unsigned base=GUARD+i*stride;
            if(trace) {
                uint32_t *hit=expected+base+16;memset(hit,0,22*4);
                if(i==3)hit[21]=1; // Procedural candidates are diagnosed, not silently accepted/missed.
                if(i<2) {
                    hit[0]=1;hit[1]=bits(1);hit[4]=i;hit[5]=i?0x76543:0x12345;
                    hit[6]=0;hit[7]=bits(.25f);hit[8]=bits(.25f);
                    const float inverse[2][12]={{1,0,0,0,0,1,0,0,0,0,1,0},{.5f,0,0,-2,0,.25f,0,0,0,0,1,0}};
                    for(unsigned j=0;j<12;j++)hit[9+j]=bits(inverse[i][j]);
                }
            }else {
                expected[base+7]=initial[base]+13;expected[base+3]=expected[base+7]+19;
                expected[base+268]=initial[base+268]+23;expected[base+262]=1;
                expected[base+344]=i?1:0;
                if(!i){expected[base+339]=expected[base+3];expected[base+11]=0x1234;}
            }
        }
        id<MTLBuffer> data=[device newBufferWithBytes:initial length:words*4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command=[queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];[encoder setBuffer:data offset:GUARD*4 atIndex:1];
        if(trace){[encoder setAccelerationStructure:tlas atBufferIndex:2];[encoder useResource:blas usage:MTLResourceUsageRead];[encoder useResource:aabb usage:MTLResourceUsageRead];}
        [encoder dispatchThreadgroups:MTLSizeMake(rays,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(command.error){fprintf(stderr,"%s\n",command.error.description.UTF8String);return 2;}
        const uint32_t *got=data.contents;unsigned bad=0;
        for(unsigned i=0;i<words;i++) {
            BOOL floating=NO;
            if(trace && i>=GUARD && i<words-GUARD) {
                unsigned field=(i-GUARD)%stride;
                floating=field==17 || field==23 || field==24 || (field>=25 && field<=36);
            }
            BOOL equal=floating?(isfinite(number(got[i])) && fabsf(number(got[i])-number(expected[i]))<1e-6f):got[i]==expected[i];
            if(!equal){fprintf(stderr,"%s word%u: got%#x expected%#x\n",stem.UTF8String,i,got[i],expected[i]);bad++;}
        }
        failures+=bad;printf("%s: %u words, %u mismatches\n",stem.UTF8String,words,bad);
    }
    printf("device: %s\n",device.name.UTF8String);return failures!=0;
} }
