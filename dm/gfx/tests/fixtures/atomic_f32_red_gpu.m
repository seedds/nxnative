#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
int main(int argc,const char **argv){@autoreleasepool{
 assert(argc==2);id<MTLDevice>d=MTLCreateSystemDefaultDevice();NSError*err=nil;
 id<MTLLibrary>lib=[d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&err];assert(lib);
 id<MTLComputePipelineState>ps=[d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&err];assert(ps);
 id<MTLBuffer>c=[d newBufferWithLength:65536 options:0],map=[d newBufferWithLength:131072 options:0],log=[d newBufferWithLength:4096 options:0],v=[d newBufferWithLength:4194304 options:0],args=[d newBufferWithLength:24 options:0];
 memset(c.contents,0,c.length);memset(map.contents,0,map.length);
 uint64_t *a=args.contents;a[0]=c.gpuAddress;a[1]=map.gpuAddress;a[2]=log.gpuAddress;
 uint64_t guest=0x100000000ull;uint64_t *m=map.contents;m[4]=guest;m[5]=guest+v.length;m[6]=v.gpuAddress-guest;((uint32_t*)m)[4]=2;
 id<MTLCommandQueue>q=[d newCommandQueue];size_t checks=0,bad=0;
 uint64_t addresses[]={guest,guest+v.length-4,guest-4,guest+v.length,0,0xfffffffffffffffcul,guest+128};
 for(unsigned hint=0;hint<2;hint++)for(unsigned n=0;n<sizeof(addresses)/8;n++){
  memset(v.contents,0,v.length);memset(log.contents,0,log.length);memcpy((char*)c.contents+49*16,&addresses[n],8);
  uint64_t chunk=addresses[n]&~((1ull<<21)-1); BOOL valid_hint=hint&&chunk>=guest&&chunk<guest+v.length; unsigned hi=(unsigned)((addresses[n]>>21)&4095);((uint32_t*)map.contents)[hi*8+6]=valid_hint?(uint32_t)(addresses[n]>>21):0;((uint32_t*)map.contents)[hi*8+7]=valid_hint?1:0;
  id<MTLCommandBuffer>cb=[q commandBuffer];id<MTLComputeCommandEncoder>e=[cb computeCommandEncoder];[e setComputePipelineState:ps];[e setBuffer:args offset:0 atIndex:0];
  for(id<MTLBuffer>b in @[c,map,log,v])[e useResource:b usage:MTLResourceUsageRead|MTLResourceUsageWrite];
  [e dispatchThreads:MTLSizeMake(32,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[e endEncoding];[cb commit];[cb waitUntilCompleted];assert(!cb.error);
  BOOL valid=addresses[n]>=guest&&addresses[n]<guest+v.length;float *values=v.contents;
  for(unsigned j=0;j<v.length/4;j++){float want=valid&&j==(addresses[n]-guest)/4?48.0f:0.0f;checks++;if(values[j]!=want){bad++;if(bad<=8)printf("value hint=%u n=%u word=%u got=%f want=%f\n",hint,n,j,values[j],want);}}
  checks++;if(((uint32_t*)log.contents)[0]!=(valid?2:0)){bad++;printf("log hint=%u n=%u got=%u want=%u\n",hint,n,((uint32_t*)log.contents)[0],valid?2:0);}
  ((uint32_t*)map.contents)[hi*8+6]=((uint32_t*)map.contents)[hi*8+7]=0;
 }
 printf("translated_RED_gpu_checks=%zu failures=%zu cases=14 lanes=32 valid_and_invalid_ranges=1 hint_and_search=1 write_log=1\n",checks,bad);return bad?1:0;
}}
