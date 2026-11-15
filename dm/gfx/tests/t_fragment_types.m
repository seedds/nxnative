#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
#include <stdint.h>
#include <stdio.h>
int main(int argc,char **argv) { @autoreleasepool {
 if(argc!=2 && !(argc==3 && !strcmp(argv[2],"--source"))){fprintf(stderr,"usage: t_fragment_types <fixture directory> [--source]\n");return 2;}
 int source_only=argc==3;
 mtl_init(); id<MTLDevice> dev=g_dev; NSError *e=nil;
 g_msl_dir=@(argv[1]);
 id<MTLLibrary> lib=source_only ? [dev newLibraryWithSource:[NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingPathComponent:@"unit.metal"] encoding:NSUTF8StringEncoding error:&e] options:nil error:&e]
                              : lib_from_file([g_msl_dir stringByAppendingPathComponent:@"base.metallib"],&e);
 if(!lib){fprintf(stderr,"load: %s\n",e.localizedDescription.UTF8String);return 1;}
 NvShader *fs=[NvShader new]; fs->fn=fn4_note([lib newFunctionWithName:@"main0"],lib,@"main0",nil);
 fs->rf.color_known=1;fs->rf.color_mask=7;
 fs->typed_outputs=@{@"entry":@"main0_t",@"alpha":@"main0_t_a",@"mask":@7,@"uint":@2,@"sint":@4,@"library":@"typed.metallib"};
 if(fs->fn.functionConstantsDictionary.count || fs->typed_lib_state){fprintf(stderr,"base unexpectedly specialized/eager loaded\n");return 1;}
 // Source mode checks the same specialization/render path when the standalone
 // compiler is unavailable. The offline mode still checks split/lazy loading.
 if(source_only){fs->typed_lib=lib;fs->typed_lib_state=1;}
 id<MTLCommandQueue> q=[dev newCommandQueue]; uint32_t expected[3]={0x3e800000,0xbf000000,0x3f800000};int checked=0;
 for (int scenario=0;scenario<64;scenario++) { @autoreleasepool {
  int test=scenario%32, wide=scenario>=32;
  unsigned a=test==27?3:test>=30?6:7; int mode=test>=27?5:test, types[3];
  for(int i=0;i<3;i++){types[i]=mode%3;mode/=3;}
  pso_key k={0};k.samples=1;k.chmask=0xfff;
  for(int i=0;i<3;i++)if(a>>i&1)k.cfmt[i]=types[i]==1?MTLPixelFormatR32Uint:types[i]==2?MTLPixelFormatR32Sint:MTLPixelFormatR32Float;
  if(wide)k.cfmt[0]=types[0]==1?MTLPixelFormatRGBA32Uint:types[0]==2?MTLPixelFormatRGBA32Sint:MTLPixelFormatRGBA32Float;
  if(!(a&1))k.cfmt[0]=0;
  // Alpha variants test the raw float alpha even when output 0 is absent.
  if(test>=28)k.atest=1;
  MTLRenderPipelineDescriptor *pd=pso_desc(&k,nil,fs,nil,0);pd.vertexFunction=fn4_note([lib newFunctionWithName:@"v"],lib,@"v",nil);
  id<MTLFunction> picked=pd.fragmentFunction;
  if(pso_desc(&k,nil,fs,nil,0).fragmentFunction!=picked){fprintf(stderr,"variant not cached\n");return 1;}
  id<MTLTexture> tex[3]={nil,nil,nil};MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
  for(int i=0;i<3;i++)if(a>>i&1){MTLPixelFormat pf=(MTLPixelFormat)k.cfmt[i];pd.colorAttachments[i].pixelFormat=pf;
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:2 height:2 mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;tex[i]=[dev newTextureWithDescriptor:td];rp.colorAttachments[i].texture=tex[i];rp.colorAttachments[i].clearColor=MTLClearColorMake(0,0,0,0);rp.colorAttachments[i].loadAction=MTLLoadActionClear;rp.colorAttachments[i].storeAction=MTLStoreActionStore;}
  id<MTLRenderPipelineState> ps=pso_build(pd,!k.border,&e);if(!ps){fprintf(stderr,"pipeline %d: %s\n",scenario,e.localizedDescription.UTF8String);return 1;}
  id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];[enc setRenderPipelineState:ps]; struct {uint32_t func;float ref;} alpha={test%2?5:2,0.5f}; if(k.atest)[enc setFragmentBytes:&alpha length:sizeof alpha atIndex:DMSH_ALPHA_BUFFER];[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];[cb commit];[cb waitUntilCompleted];
  if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GPU failed\n");return 1;}
  for(int i=0;i<3;i++)if(tex[i]){
   uint32_t pixels[16],rgba[4]={0x3e800000,0xbf000000,0x3f800000,0x3e800000};int channels=wide&&i==0?4:1;
   [tex[i] getBytes:pixels bytesPerRow:8*channels fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
   for(int j=0;j<4*channels;j++){
    uint32_t want=test>=28&&test%2?0:channels==4?rgba[j%4]:expected[i];
    if(pixels[j]!=want){fprintf(stderr,"bits scenario%d target%d word%d: %x expected%x\n",scenario,i,j,pixels[j],want);return 1;}
   }
  }
  checked++;
 } }
 printf("host typed outputs: %d format combinations preserve exact bits; alpha and variant cache verified\n",checked);return 0;
} }
