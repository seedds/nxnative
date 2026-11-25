// Real public recording, compact expansion, queue replay and Metal argument
// encoding. Ray-tracing resource stage8 must stay apart from compute5/task6.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)

static NVNcommandBuffer commands;
static const int public_stages[] = {8, 5, 6, 7}, host_stages[] = {6, 5, 3, 4};
static uint8_t *memory;
static uint64_t address(int stage, int slot) { return (uint64_t)(uintptr_t)(memory + stage * 4096 + slot * 256); }
static uint64_t handle(int stage, int slot) { return (uint64_t)(17 + stage * 11 + slot) | (uint64_t)(3 + stage) << 20; }

static void apply(NVNcommandHandle handle_) {
    size_t n; uint8_t *records = nvn_rec_linear(handle_, &n);
    for (size_t offset = 0; offset < n;) {
        op_hdr header; memcpy(&header, records + offset, 8);
        if (header.op == OP_END) break;
        CHECK(header.size >= 8 && header.size <= n - offset, "expanded record is bounded");
        CHECK(state_op(header.op, records + offset + 8), "resource record replays through production state_op");
        offset += header.size;
    }
    free(records);
}

static void bind_all(int stage) {
    NVNshaderStage s = (NVNshaderStage)stage;
    h_nvnCommandBufferBindUniformBuffer(&commands,s,0,address(stage,0),32);
    NVNbufferRange aligned[] = {{address(stage,1),32},{address(stage,2),64}};
    h_nvnCommandBufferBindUniformBuffers(&commands,s,1,2,aligned);
    NVNbufferRange unaligned[] = {{address(stage,3)+1,33},{address(stage,4)+3,65}};
    h_nvnCommandBufferBindUniformBuffers(&commands,s,3,2,unaligned);
    // Record-only oversized ranges force canonical fallback. No GPU read or
    // submission uses them; they distinguish normalization from truncation.
    NVNbufferRange wide[] = {{address(stage,5),(size_t)UINT32_MAX+1},{address(stage,6),(size_t)UINT32_MAX+2}};
    h_nvnCommandBufferBindUniformBuffers(&commands,s,5,2,wide);
    h_nvnCommandBufferBindUniformBuffers(&commands,s,7,1,wide);
    h_nvnCommandBufferBindStorageBuffer(&commands,s,0,address(stage,0),32);
    h_nvnCommandBufferBindStorageBuffers(&commands,s,1,2,aligned);
    h_nvnCommandBufferBindStorageBuffers(&commands,s,3,2,wide);
    h_nvnCommandBufferBindTexture(&commands,s,0,handle(stage,0));
    NVNtextureHandle textures[] = {handle(stage,1),handle(stage,2)};
    h_nvnCommandBufferBindTextures(&commands,s,1,2,textures);
    NVNseparateTextureHandle separate[] = {{handle(stage,3)},{handle(stage,4)}};
    h_nvnCommandBufferBindSeparateTexture(&commands,s,5,separate[0]);
    h_nvnCommandBufferBindSeparateTextures(&commands,s,6,2,separate);
    NVNseparateSamplerHandle samplers[] = {{handle(stage,5)},{handle(stage,6)}};
    h_nvnCommandBufferBindSeparateSampler(&commands,s,0,samplers[0]);
    h_nvnCommandBufferBindSeparateSamplers(&commands,s,1,2,samplers);
    NVNimageHandle images[] = {handle(stage,7),handle(stage,8)};
    h_nvnCommandBufferBindImage(&commands,s,0,images[0]);
    h_nvnCommandBufferBindImages(&commands,s,1,2,images);
}

static void verify_all(void) {
    for (unsigned i = 0; i < sizeof public_stages / sizeof *public_stages; i++) {
        int p=public_stages[i], s=host_stages[i];
        CHECK(S.ubo[s][0].addr==address(p,0) && S.ubo[s][1].addr==address(p,1) && S.ubo[s][3].addr==address(p,3)+1,
              "single/aligned/unaligned plural uniforms stay on their own normalized stage");
        CHECK(S.ubo[s][5].addr==address(p,5) && S.ubo[s][5].size==(uint64_t)UINT32_MAX+1 &&
              S.ubo[s][6].size==(uint64_t)UINT32_MAX+2 && S.ubo[s][7].addr==address(p,5),
              "plural canonical fallback and one-range fallback normalize only once");
        CHECK((S.ubo_set[s]&255)==255, "uniform bound-slot bits include the distinct resource stage");
        CHECK(S.ssbo[s][0].addr==address(p,0) && S.ssbo[s][2].addr==address(p,2) && S.ssbo[s][3].size==(uint64_t)UINT32_MAX+1,
              "single/plural compact and fallback storage bindings stay separate");
        CHECK(S.tex[s][0]==handle(p,0) && S.tex[s][2]==handle(p,2), "combined texture single/plural bindings stay separate");
        CHECK(S.tex[s][NVN_SEPTEX_SLOT0+5]==handle(p,3) && S.tex[s][NVN_SEPTEX_SLOT0+7]==handle(p,4), "separate texture single/plural bindings stay separate");
        CHECK(S.samp[s][0]==handle(p,5) && S.samp[s][2]==handle(p,6), "sampler single/plural bindings stay separate");
        CHECK(S.img[s][0]==handle(p,7) && S.img[s][2]==handle(p,8), "image single/plural bindings stay separate");
        CHECK(bound_tex(s,0)==handle(p,0) && bound_img(s,0)==handle(p,7), "resource helpers accept every normalized stage");
    }
}

static void recording(int compact) {
    g_rec_compact=compact;
    mtl_queue_activate(780001);
    h_nvnCommandBufferBeginRecording(&commands);
    for (unsigned i=0;i<sizeof public_stages/sizeof *public_stages;i++) bind_all(public_stages[i]);
    uint64_t deferred=handle(8,10);
    h_nvnCommandBufferBindTexturesDeferred(&commands,(NVNshaderStage)8,10,1,&deferred);
    NVNcommandHandle list=h_nvnCommandBufferEndRecording(&commands);
    deferred=handle(8,11);
    apply(list); verify_all();
    CHECK(S.tex[6][10]==deferred, "deferred ray-tracing texture reads the execution-time handle");
    uint64_t rt= S.ubo[6][0].addr, cs=S.ubo[5][0].addr, task=S.ubo[3][0].addr;
    mtl_queue_activate(780002);
    CHECK(!S.ubo[6][0].addr && !S.ubo[5][0].addr && !S.tex[6][0] && !S.img[6][0], "fresh queue has empty resource tables including ray tracing");
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferBindUniformBuffer(&commands,(NVNshaderStage)8,0,address(8,12),16);
    h_nvnCommandBufferBindTexture(&commands,(NVNshaderStage)8,0,handle(8,12));
    h_nvnCommandBufferBindImage(&commands,(NVNshaderStage)8,0,handle(8,13));
    h_nvnCommandBufferBindSeparateSampler(&commands,(NVNshaderStage)8,0,(NVNseparateSamplerHandle){handle(8,14)});
    apply(h_nvnCommandBufferEndRecording(&commands));
    CHECK(S.ubo[6][0].addr==address(8,12) && !S.ubo[5][0].addr && !S.ubo[3][0].addr, "second queue ray-tracing writes do not populate compute or task");
    mtl_queue_activate(780001);
    CHECK(S.ubo[6][0].addr==rt && S.ubo[5][0].addr==cs && S.ubo[3][0].addr==task, "queue restore preserves distinct RT/compute/task uniforms");
    verify_all();
    mtl_queue_activate(780002);
    CHECK(S.ubo[6][0].addr==address(8,12) && S.tex[6][0]==handle(8,12) && S.img[6][0]==handle(8,13) && S.samp[6][0]==handle(8,14),
          "second queue restores its own RT buffers/images/textures/samplers");
    mtl_queue_forget(780001); mtl_queue_forget(780002);
}

static void generations(void) {
    state_reset();
    for (int stage=0;stage<NVN_RESOURCE_STAGES;stage++) g_args_wrote[stage]=g_args_seq;
    uint64_t cs=g_args_ugen[5], task=g_args_ugen[3], rt=g_args_ugen[6];
    p_bind_buf b={6,0,address(8,0),32}; state_op(OP_BIND_UBO,&b);
    CHECK(g_args_ugen[6]>rt && g_args_ugen[5]==cs && g_args_ugen[3]==task, "RT uniform dirtiness has a separate generation");
    for (int stage=0;stage<NVN_RESOURCE_STAGES;stage++) g_args_wrote[stage]=g_args_seq;
    cs=g_args_ugen[5]; task=g_args_ugen[3]; rt=g_args_ugen[6];
    ubd_dirty(address(8,0),address(8,0)+16);
    CHECK(g_args_ugen[6]>rt && g_args_ugen[5]==cs && g_args_ugen[3]==task, "deferred inline uniform writes invalidate RT arguments");
    for (int stage=0;stage<NVN_RESOURCE_STAGES;stage++) g_args_wrote[stage]=g_args_seq;
    args_dirty_all();
    for (int stage=0;stage<NVN_RESOURCE_STAGES;stage++) CHECK(args_changed(stage), "pool/queue invalidation includes all seven resource stages");
    CHECK(stage_of(@"raytracing")==6 && stage_of(@"compute")==5 && stage_of(@"task")==3 && stage_of(@"mesh")==4, "stage parser distinguishes resource stage from raster shader slots");
}

static void gpu_arguments(void) {
    // A normal synthetic compute kernel isolates the host's resource ABI. This
    // is not an RT implementation or a ray-query rendering test.
    state_reset();
    uint32_t *rtin=(uint32_t *)(memory+65536), *rtout=rtin+1024, *csin=rtout+1024, *csout=csin+1024;
    rtin[0]=17; csin[0]=29; rtout[0]=csout[0]=0;
    p_bind_buf binds[]={{6,0,(uint64_t)rtin,16},{5,0,(uint64_t)csin,16}};
    for (int i=0;i<2;i++) state_op(OP_BIND_UBO,&binds[i]);
    binds[0].addr=(uint64_t)rtout; binds[1].addr=(uint64_t)csout;
    for (int i=0;i<2;i++) state_op(OP_BIND_SSBO,&binds[i]);
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; struct A { device uint*out [[id(0)]]; constant uint*in [[id(1)]]; }; kernel void probe(constant A&a [[buffer(0)]]) { a.out[0]=a.in[0]; }" options:nil error:&error];
    CHECK(lib!=nil,"resource ABI probe compiles"); if (!lib) return;
    NvShader *shader=[NvShader new]; shader->stage=NVN_RT_RESOURCE_STAGE; strcpy(shader->key,"0123456789abcdef_b"); shader->fn=[lib newFunctionWithName:@"probe"];
    shader->ae=[shader->fn newArgumentEncoderWithBufferIndex:0]; shader->arg_len=shader->ae.encodedLength; shader->rf.nb=2;
    shader->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0}; shader->rf.b[1]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0,.aid=1};
    id<MTLComputePipelineState> pipeline=[g_dev newComputePipelineStateWithFunction:shader->fn error:&error];
    CHECK(pipeline!=nil,"resource ABI pipeline compiles"); if (!pipeline) return;
    mtl_sub_begin(); id<MTL4ComputeCommandEncoder> en=enc_c_begin();
    for (int i=0;i<2;i++) {
        CHECK(encode_args((id<MTLCommandEncoder>)en,shader,i?5:6,0),"RT and compute resource argument blocks encode through production helpers");
        enc_c_pso(en,pipeline); enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));
    }
    enc_c_end(en);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull) && !mtl_last_commit_error(),"resource ABI GPU dispatches complete"); mtl_drain();
    CHECK(rtout[0]==17 && csout[0]==29,"actual GPU reads/writes separate RT and compute UBO/SSBO tables");
}

int main(void) { @autoreleasepool {
    mtl_init();
    if (posix_memalign((void **)&memory,getpagesize(),128u<<10)) return 2;
    memset(memory,0,128u<<10); mtl_pool_add(memory,128u<<10);
    CHECK(h_nvnCommandBufferInitialize(&commands,NULL),"command buffer initializes");
    recording(0); recording(1); generations(); gpu_arguments();
    h_nvnCommandBufferFinalize(&commands); mtl_pool_del(memory); free(memory);
    printf("t_nvn_resource_stages: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
