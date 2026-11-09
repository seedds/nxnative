// Buffer dispatch ordering: real dependent GPU results plus fallback/capacity checks.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c,label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static void issue(NvShader *shader, id<MTLComputePipelineState> ps, id<MTLBuffer> mem, unsigned src, unsigned dst) {
    uint64_t a=(uint64_t)mem.contents;
    S.ubo[5][0]=(p_bind_buf){.addr=a+src*4,.size=4};
    S.ssbo[5][0]=(p_bind_buf){.addr=a+dst*4,.size=4};
    p_dispatch dp={.x=1,.y=1,.z=1};
    id<MTL4ComputeCommandEncoder> e=dispatch_buffer_enc(shader,&dp);
    enc_c_pso(e,ps);enc_c_args(e,mem.gpuAddress+src*4,0);enc_c_args(e,mem.gpuAddress+dst*4,1);
    enc_c_dispatch(e,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_CS_RANGES","1",1);mtl_init();
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; kernel void step(device const uint* a [[buffer(0)]],device uint* b [[buffer(1)]]) { b[0]=a[0]+1; }" options:nil error:&error];
    id<MTLFunction> fn=[lib newFunctionWithName:@"step"];
    CHECK(fn!=nil,"test kernel compiles");if(!fn)return 1;
    id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:fn error:&error];
    void *guest=NULL;
    CHECK(!posix_memalign(&guest,getpagesize(),65536),"guest allocation");if(!guest)return 1;
    memset(guest,0,65536);mtl_pool_add(guest,65536);
    void *mapped=NULL;uint64_t offset=0;
    CHECK(mtl_addr((uint64_t)guest,4096,&mapped,&offset)&&offset==0,"registered guest pool resolves");
    id<MTLBuffer> mem=(__bridge id<MTLBuffer>)mapped;
    CHECK(ps&&mem,"pipeline and memory allocated");if(!ps||!mem)return 1;res_add(mem);
    NvShader *shader=[NvShader new];shader->rf.nb=2;
    shader->rf.b[0]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0};
    shader->rf.b[1]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.rw=1};
    for(int round=0;round<64;round++) {
        uint32_t *v=mem.contents;memset(v,0,4096);v[0]=round;v[8]=100+round;
        mtl_sub_begin();uint64_t barriers=g_blit_barriers;
        issue(shader,ps,mem,0,1);issue(shader,ps,mem,8,9);
        CHECK(g_blit_barriers==barriers,"disjoint dispatches need no internal barrier");
        issue(shader,ps,mem,1,2);CHECK(g_blit_barriers==barriers+1,"read after write orders producer");
        issue(shader,ps,mem,8,1);CHECK(g_blit_barriers==barriers+2,"write after read orders reader");
        issue(shader,ps,mem,0,1);CHECK(g_blit_barriers==barriers+3,"overlapping writes ordered");
        // Fill the access ledger with disjoint reads/writes, forcing capacity flushes.
        for(unsigned i=16;i<216;i+=2)issue(shader,ps,mem,0,i);
        CHECK(g_br_n<=BW_MAX&&g_bw_n<=BW_MAX,"bounded ledger stays within capacity");
        uint64_t a=(uint64_t)mem.contents;
        enc_b_copy_buffer(blit_buf(a,a+4,a+1200,a+1204),mem,0,mem,1200,4);
        issue(shader,ps,mem,300,301);
        enc_b_copy_buffer(blit_buf(a+1204,a+1208,a+1208,a+1212),mem,1204,mem,1208,4);
        issue(shader,ps,mem,302,303);
        // An unbounded declaration forces the conservative path, with the same
        // actual kernel so its dependent result remains an independent oracle.
        shader->rf.b[1].kind=NVN_RB_GMEM;issue(shader,ps,mem,303,304);
        shader->rf.b[1].kind=NVN_RB_SBUFP;issue(shader,ps,mem,304,305);
        CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"submission completes");
        CHECK(v[301]==(uint32_t)round+1&&v[303]==(uint32_t)round+2&&v[305]==(uint32_t)round+4,"copy dispatch and unknown fallback transitions preserve GPU results");
        CHECK(!mtl_last_commit_error(),"Metal reports no execution error");
        CHECK(v[1]==(uint32_t)round+1&&v[2]==(uint32_t)round+2&&v[9]==(uint32_t)round+101,"GPU results preserve dependency order");
        for(unsigned i=16;i<216;i+=2)CHECK(v[i]==(uint32_t)round+1,"capacity flush preserves output");
    }
    csh_rng r[BW_MAX],w[BW_MAX];int nr=0,nw=0;p_dispatch dp={0};
    S.ssbo[5][0].size=0;CHECK(!cs_buffer_ranges(shader,&dp,r,&nr,w,&nw),"unknown buffer size rejected");
    S.ssbo[5][0].size=4;dp.ind=1;nr=nw=0;CHECK(!cs_buffer_ranges(shader,&dp,r,&nr,w,&nw),"indirect dispatch rejected");
    dp.ind=0;shader->rf.b[1].kind=NVN_RB_GMEM;nr=nw=0;CHECK(!cs_buffer_ranges(shader,&dp,r,&nr,w,&nw),"raw global memory rejected");
    shader->rf.b[1].kind=NVN_RB_IMG;nr=nw=0;CHECK(!cs_buffer_ranges(shader,&dp,r,&nr,w,&nw),"image access rejected");
    nr=0;CHECK(!cs_range_add(r,&nr,UINT64_MAX-2,4),"wrapped range rejected");
    nr=0;CHECK(!cs_range_add(r,&nr,0x12340000,4),"unmapped address cannot masquerade as independent dummy storage");
    mtl_pool_del(guest);mtl_drain();mem=nil;free(guest);
    printf("compute range ordering: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
