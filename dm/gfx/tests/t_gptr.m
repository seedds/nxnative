// Integration test: GPU reads pointer-site descriptors produced by encode_args.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks,failed;
#define CHECK(x) do {checks++;if(!(x)){failed++;fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);}}while(0)
int main(void) { @autoreleasepool {
 mtl_init(); uint8_t *mem,*target; size_t len=65536;
 if(posix_memalign((void**)&mem,getpagesize(),len)||posix_memalign((void**)&target,getpagesize(),len))abort();
 memset(mem,0,len);memset(target,0,len);mtl_pool_add(mem,len);mtl_pool_add(target,len);
 mtl_pool_buffer_addr(mem,len);mtl_pool_buffer_addr(target,len);
 uint64_t ptr=(uint64_t)(uintptr_t)(target+64);memcpy(mem,&ptr,8);
 NSError *err=nil;NSString *src=@"#include <metal_stdlib>\nusing namespace metal;\nstruct A{const device ulong *sites [[id(0)]];device ulong*out [[id(1)]];};kernel void probe(constant A&a[[buffer(0)]],uint i[[thread_position_in_grid]]){if(i<20)a.out[i]=a.sites[i];}";
 id<MTLLibrary> lib=[g_dev newLibraryWithSource:src options:nil error:&err];if(!lib){fprintf(stderr,"%s\n",err.description.UTF8String);return 1;}
 NvShader *sh=[NvShader new];sh->stage=5;sh->fn=[lib newFunctionWithName:@"probe"];snprintf(sh->key,sizeof sh->key,"ffff000000000123_b");
 sh->ae=[sh->fn newArgumentEncoderWithBufferIndex:0];sh->arg_len=sh->ae.encodedLength;
 NSDictionary *json=@{@"route":@"B",@"bindings":@[
 @{@"kind":@"gptr",@"arg_id":@0,@"msl_name":@"sites",@"msl_type":@"const device ulong*",@"sites":@[@[@0,@0],@[@0,@8],@[@0,@16],@[@-1,@0],@[@1,@0]]},
 @{@"kind":@"sbuf",@"reg":@2,@"arg_id":@1,@"msl_name":@"out",@"msl_type":@"device ulong*",@"rw":@YES}]};
 char why[256]={0};CHECK(nvn_refl_from_json(json,&sh->rf,why,sizeof why)==0);CHECK(sh->rf.ngptr==5);
 id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:sh->fn error:&err];if(!ps)return 1;
 for(int round=0;round<2;round++){
  state_reset();S.ssbo[5][0]=(p_bind_buf){.stage=5,.index=0,.addr=(uint64_t)(uintptr_t)mem,.size=16};
  S.ssbo[5][2]=(p_bind_buf){.stage=5,.index=2,.addr=(uint64_t)(uintptr_t)(mem+4096),.size=256};
  if(round){ptr=(uint64_t)(uintptr_t)(mem+8192);memcpy(mem,&ptr,8);}
  memset(mem+4096,0xcc,256);mtl_sub_begin();id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);args_dirty_all();
  CHECK(encode_args((id)en,sh,5,0));enc_c_pso(en,ps);enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(32,1,1));enc_c_end(en);
  CHECK(mtl_wait(mtl_sub_commit(),5000000000ull));
  void *mb=NULL;uint64_t mo=0,gpu=0;CHECK(mtl_gpu_addr(ptr,1,&mb,&mo,&gpu));
  uint64_t *got=(uint64_t*)(mem+4096);CHECK(got[0]==ptr-mo);CHECK(got[1]==ptr-mo+((__bridge id<MTLBuffer>)mb).length);CHECK(got[2]==gpu-ptr);CHECK(got[3]==0);
  for(int i=4;i<20;i++)CHECK(got[i]==0);
 }
 printf("gptr_host: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
