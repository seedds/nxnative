#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t lo,hi,off; } Range;
static Range ranges[5100];static unsigned nr;
static int cmp(const void *a,const void *b){uint64_t x=((const Range*)a)->lo,y=((const Range*)b)->lo;return x<y?-1:x>y;}
static uint64_t probes[10000];static unsigned np;
static void probe(uint64_t a){probes[np++]=a;}
static uint32_t oracle(uint64_t a,const uint8_t *data){int winner=-1;for(unsigned i=0;i<nr;i++){if(ranges[i].lo>a)break;winner=(int)i;}if(winner<0||a>=ranges[winner].hi)return 0;uint32_t v;memcpy(&v,data+ranges[winner].off+a-ranges[winner].lo,4);return v;}
int main(int argc,const char **argv){@autoreleasepool{
 if(argc!=3)return 2;id<MTLDevice> dev=MTLCreateSystemDefaultDevice();id<MTLCommandQueue> queue=[dev newCommandQueue];NSError *error=nil;
 id<MTLComputePipelineState> pipelines[2];for(int i=0;i<2;i++){id<MTLLibrary> lib=[dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[i+1])] error:&error];if(!lib){fprintf(stderr,"lib %s\n",error.description.UTF8String);return 2;}pipelines[i]=[dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&error];if(!pipelines[i])return 2;}
 const NSUInteger bytes=8<<20;id<MTLBuffer> data=[dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];uint32_t *words=data.contents;for(unsigned i=0;i<bytes/4;i++)words[i]=(i*2654435761u)^0x9137a2bdu;
 // More than 4096 ranges, a 32-bit address carry, adjacent ranges with different
 // translations, host-normalized overlaps, and a whole-chunk hint range.
 ranges[nr++]=(Range){0xfffffff0ull,0x100000030ull,0};
 for(unsigned i=0;i<5000;i++){uint64_t a=0x2000000000ull+(uint64_t)i*32768;ranges[nr++]=(Range){a,a+256,4096+(uint64_t)i*256};if(i%37==0){probe(a+4);probe(a+244);probe(a+252);probe(a+256);}}
 uint64_t big=0x4000000000ull;ranges[nr++]=(Range){big,big+(4<<20),2<<20};
 uint64_t cross=0x5000000000ull;ranges[nr++]=(Range){cross,cross+16,128};ranges[nr++]=(Range){cross+16,cross+64,512};
 uint64_t overlap=0x6000000000ull;ranges[nr++]=(Range){overlap,overlap+64,1024};ranges[nr++]=(Range){overlap+12,overlap+48,2048};
 qsort(ranges,nr,sizeof(*ranges),cmp);
 // gmem_map publishes disjoint ranges: overlapping pools retain only the tail
 // past the preceding range. The cache relies on this host-side invariant.
 unsigned kept=0;uint64_t end=0;
 for(unsigned i=0;i<nr;i++){Range r=ranges[i];uint64_t lo=r.lo>end?r.lo:end;if(lo>=r.hi)continue;r.off+=lo-r.lo;r.lo=lo;ranges[kept++]=r;end=r.hi;}nr=kept;
 for(unsigned i=0;i<64;i+=4){probe(0xffffffe0ull+i);probe(cross+i);probe(overlap+i);probe(big+(2<<20)-32+i);}
 probe(0);probe(UINT64_MAX-15);probe(UINT64_MAX-7);probe(UINT64_MAX-3);
 id<MTLBuffer> input=[dev newBufferWithBytes:probes length:np*8 options:MTLResourceStorageModeShared],outputs[2];for(int i=0;i<2;i++)outputs[i]=[dev newBufferWithLength:np*16 options:MTLResourceStorageModeShared];
 unsigned failures=0,total=0;
 for(unsigned cap=8192;cap<=16384;cap*=2)for(unsigned hinted=0;hinted<2;hinted++){
  id<MTLBuffer> map=[dev newBufferWithLength:cap*32 options:MTLResourceStorageModeShared],args=[dev newBufferWithLength:8 options:MTLResourceStorageModeShared];uint64_t *rows=map.contents;memset(rows,0,map.length);for(unsigned i=1;i<cap;i++)rows[i*4]=UINT64_MAX;rows[2]=cap;
  for(unsigned i=0;i<nr;i++){rows[(i+1)*4]=ranges[i].lo;rows[(i+1)*4+1]=ranges[i].hi;rows[(i+1)*4+2]=data.gpuAddress+ranges[i].off-ranges[i].lo;
   if(hinted&&ranges[i].lo==big){for(uint64_t chunk=big>>21;chunk<(big+(4<<20))>>21;chunk++){uint32_t *h=(uint32_t*)rows+(chunk&4095)*8+6;h[0]=(uint32_t)chunk;h[1]=i+1;}}}
  *(uint64_t*)args.contents=map.gpuAddress;
  for(unsigned arm=0;arm<2;arm++){
   memset(outputs[arm].contents,0xcd,outputs[arm].length);id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];[ce setComputePipelineState:pipelines[arm]];[ce setBuffer:args offset:0 atIndex:0];[ce setBuffer:input offset:0 atIndex:1];[ce setBuffer:outputs[arm] offset:0 atIndex:2];[ce useResource:map usage:MTLResourceUsageRead];[ce useResource:data usage:MTLResourceUsageRead];[ce dispatchThreads:MTLSizeMake(np,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[ce endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GPU failure %s\n",cb.error.description.UTF8String);return 2;}
   uint32_t *got=outputs[arm].contents;for(unsigned i=0;i<np;i++)for(unsigned lane=0;lane<4;lane++){uint32_t prior=oracle(probes[i]-16ull+4*lane,data.contents);uint32_t expected=oracle(probes[i]+4*lane,data.contents)^((prior<<1)|(prior>>31));total++;if(got[i*4+lane]!=expected){if(failures++<8)fprintf(stderr,"mismatch cap%u hint%u arm%u address%llx lane%u got%x expected%x\n",cap,hinted,arm,(unsigned long long)probes[i],lane,got[i*4+lane],expected);}}
  }
  if(memcmp(outputs[0].contents,outputs[1].contents,np*16))failures++;
  printf("cap=%u hints=%u probes=%u twin_bytes=%u\n",cap,hinted,np,np*16);
 }
 printf("raw range GPU: %u word comparisons, %u failures\n",total,failures);return failures?1:0;
}}
