#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=3)return 2;const unsigned n=1024,cap=4096;const uint64_t guest=0x2000000000ull;id<MTLDevice>d=MTLCreateSystemDefaultDevice();id<MTLCommandQueue>q=[d newCommandQueue];NSError*err=nil;
 id<MTLBuffer>data=[d newBufferWithLength:n*16 options:0],map=[d newBufferWithLength:cap*32 options:0],args=[d newBufferWithLength:8 options:0],out=[d newBufferWithLength:n*16 options:0];
 uint64_t*rows=map.contents;memset(rows,0,map.length);for(unsigned i=1;i<cap;i++)rows[i*4]=UINT64_MAX;rows[2]=cap;rows[4]=guest;rows[5]=guest+n*16;rows[6]=data.gpuAddress-guest;*(uint64_t*)args.contents=map.gpuAddress;
 unsigned bad=0,checks=0;
 for(int arm=0;arm<2;arm++){
  uint32_t*p=data.contents;for(unsigned i=0;i<n*4;i++)p[i]=0x13572468u^i;
  id<MTLLibrary>lib=[d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[arm+1])] error:&err];if(!lib){fprintf(stderr,"%s\n",err.description.UTF8String);return 2;}
  id<MTLComputePipelineState>ps=[d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&err];if(!ps)return 2;
  id<MTLCommandBuffer>cb=[q commandBuffer];id<MTLComputeCommandEncoder>e=[cb computeCommandEncoder];[e setComputePipelineState:ps];[e setBuffer:args offset:0 atIndex:0];[e setBuffer:out offset:0 atIndex:2];[e setBuffer:data offset:0 atIndex:3];[e useResource:map usage:MTLResourceUsageRead];[e useResource:data usage:MTLResourceUsageRead|MTLResourceUsageWrite];[e dispatchThreads:MTLSizeMake(n,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[e endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted)return 3;
  uint32_t*v=out.contents;for(unsigned i=0;i<n;i++){uint32_t old=0x13572468u^(i*4),new=old^0xdeadbeefu;checks+=3;if(v[i*4]!=old||v[i*4+1]!=new||p[i*4]!=new)bad++;}
 }
 printf("raw range intervening-write GPU: %u checks, %u failed\n",checks,bad);return bad?1:0;
}}
