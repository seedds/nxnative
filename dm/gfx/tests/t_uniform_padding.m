// A short uniform binding keeps the GPU-produced prefix at its command-stream
// position, and supplies zeros beyond its bound size.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void*p){(void)p;return NULL;}
module_t *g_mod;int g_nmod;
void svc_thread_dump(void){}
uint64_t now_ns(void){return bp_now();}
static int checks,failed;
#define CHECK(c,label) do{checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
static uint64_t pad(NvShader*sh,p_bind_buf*u,id<MTLBuffer>__strong*b){
 return ub_padded(sh,5,0,u,32,(const uint8_t*)(uintptr_t)u->addr,b);
}
static void verify(id<MTLBuffer>b,uint64_t gpu,uint32_t first,uint32_t rest){
 const uint32_t*w=(const uint32_t*)((const uint8_t*)b.contents+gpu-b.gpuAddress);
 CHECK(w[0]==first,"padding preserves the producer's first word");
 CHECK(w[1]==rest && w[2]==rest,"padding preserves the remaining bound words");
 for(int i=3;i<8;i++)CHECK(w[i]==0,"an out-of-bounds fixed read returns zero");
}

static void emit_padding(uint8_t*stream,size_t*n,unsigned op,const void*data,size_t bytes){
 size_t length=(8+bytes+7)&~7ul;op_hdr h={op,(uint32_t)length};
 memcpy(stream+*n,&h,8);if(bytes)memcpy(stream+*n+8,data,bytes);*n+=length;
}
static void render_padding(uint8_t*mem){
 NSError*error=nil;
 NSString*source=@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 v(uint i [[vertex_id]]) {float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,0,1);}\nstruct A {constant float *u [[id(0)]];};\nfragment float4 f(constant A&a [[buffer(0)]]) {return float4(a.u[0],a.u[1],a.u[3],1);}\n";
 id<MTLLibrary>lib=[g_dev newLibraryWithSource:source options:nil error:&error];
 CHECK(lib!=nil,"bare fixed-read shader compiles");if(!lib)return;
 prog_t handle={.serial=997703};NvProg*pr=[NvProg new];
 pr->sh[0]=[NvShader new];pr->sh[0]->stage=0;pr->sh[0]->fn=[lib newFunctionWithName:@"v"];
 pr->sh[1]=[NvShader new];pr->sh[1]->stage=1;pr->sh[1]->fn=[lib newFunctionWithName:@"f"];
 pr->sh[1]->ae=[pr->sh[1]->fn newArgumentEncoderWithBufferIndex:0];
 pr->sh[1]->rf.color_known=1;pr->sh[1]->rf.color_mask=1;
 pr->sh[1]->rf.nb=1;pr->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0,.aid=0,.fixw=8};
 serial_put(handle.serial,pr);
 tex_t target={.magic=MAGIC('T','E','X',0),.serial=997704,.off=997704ll<<20};
 target.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA32F,.width=8,.height=8,.depth=1,.levels=1,.samples=1};nvn_tex_layout(&target.d,&target.L);
 uint8_t stream[2048]={0};size_t n=0;
 p_set_rt rt={.n=1};rt.color[0]=&target;emit_padding(stream,&n,OP_SET_RT,&rt,sizeof rt);
 p_bind_program bind={&handle,3};emit_padding(stream,&n,OP_BIND_PROGRAM,&bind,sizeof bind);
 p_bind_buf u={.stage=1,.index=0,.addr=(uint64_t)mem+0x200,.size=12};emit_padding(stream,&n,OP_BIND_UBO,&u,sizeof u);
 p_draw draw={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1};emit_padding(stream,&n,OP_DRAW,&draw,sizeof draw);emit_padding(stream,&n,OP_END,NULL,0);
 float values[3]={.25f,.5f,.75f};memcpy(mem+0x100,values,sizeof values);
 state_reset();memset(&R,0,sizeof R);mtl_sub_begin();
 mtl_sub_copy((uint64_t)mem+0x100,u.addr,12);mtl_exec(stream);
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"real draw survives a padding copy ending its render encoder");
 id<MTLTexture>image=tex_mtl(&target);id<MTLBuffer>out=[g_dev newBufferWithLength:8*256 options:MTLResourceStorageModeShared];
 id<MTLCommandBuffer>cb=[g_q commandBuffer];id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
 [e copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:out.length];[e endEncoding];[cb commit];[cb waitUntilCompleted];
 CHECK(cb.error==nil,"rendered padded binding reads back without a GPU error");
 int correct=1;for(int y=0;y<8;y++)for(int x=0;x<8;x++){const float*f=(const float*)((const uint8_t*)out.contents+y*256)+4*x;correct&=f[0]==.25f && f[1]==.5f && f[2]==0 && f[3]==1;}
 CHECK(correct,"every pixel reads the GPU-produced prefix and a zero beyond its binding");
}
int main(void){@autoreleasepool{
 mtl_init();uint8_t*mem=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
 CHECK(mem!=MAP_FAILED,"source memory allocated");if(mem==MAP_FAILED)return 1;
 memset(mem,0x11,16384);mtl_pool_add(mem,16384);
 NvShader*sh=[NvShader new];snprintf(sh->key,sizeof sh->key,"padding_gpu");
 p_bind_buf u={.addr=(uint64_t)mem,.size=12};id<MTLBuffer>a,b,again;
 mtl_sub_begin();mtl_sub_fill(u.addr,u.size,0x77777777);
 uint64_t ga=pad(sh,&u,&a),gr=pad(sh,&u,&again);
 CHECK(ga==gr && a==again,"an encoder retry reuses the ordered padded binding");
 mtl_sub_fill(u.addr,u.size,0x99999999);uint64_t gb=pad(sh,&u,&b);
 CHECK(ga!=gb,"a later producer gets its own padded snapshot");
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"both ordered snapshots complete");
 verify(a,ga,0x77777777,0x77777777);verify(b,gb,0x99999999,0x99999999);
 // A producer on an earlier submission is deliberately not yet on the GPU.
 id<MTLSharedEvent>gate=[g_dev newSharedEvent];[g_q4 waitForEvent:gate value:1];
 mtl_sub_begin();mtl_sub_fill(u.addr,u.size,0x55555555);uint64_t producer=mtl_sub_commit();
 mtl_sub_begin();uint32_t latest=0xaaaaaaaa;mtl_sub_update(&latest,u.addr,4);
 id<MTLBuffer>c;uint64_t gc=pad(sh,&u,&c);
 CHECK(g_done<producer,"padding returns while its previous producer is still gated");
 gate.signaledValue=1;
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"prior producer and partial update complete without a CPU wait");
 verify(c,gc,latest,0x55555555);
 // A six-byte binding still permits only its first complete 32-bit word.
 mtl_sub_begin();p_bind_buf odd={.addr=(uint64_t)mem+0x100,.size=6};
 id<MTLBuffer>tail;uint64_t gt=pad(sh,&odd,&tail);
 CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"partial-word padding completes");
 const uint32_t*tw=(const uint32_t*)((const uint8_t*)tail.contents+gt-tail.gpuAddress);
 CHECK(tw[0]==0x11111111 && tw[1]==0,"a partially bound final word stays zero like the shader's former size guard");
 render_padding(mem);
 fprintf(stderr,"t_uniform_padding: %d checks, %d failures\n",checks,failed);
 return failed?1:0;
}}
