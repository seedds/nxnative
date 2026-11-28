// Production argument encoding around inline updates: old GPU tables must remain
// immutable while later draws/dispatches see the new texture and sampler handles.
#include "../nvn/nvn_mtl.m"
#include <unistd.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (%d)\n", label, __LINE__); } } while (0)
static uint8_t *mem;
static uint64_t next_serial = 96000;

static id<MTLTexture> texture(tex_t *t, int width, int height) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=next_serial++};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8,
        .width=width, .height=height, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
    CHECK(out != nil, "synthetic texture allocated");
    if (!out) return nil;
    res_add(out); mtl_tex_init(t); serial_put(t->serial, out); return out;
}
static NvShader *shader(id<MTLLibrary> lib, NSString *name, int stage) {
    NvShader *s = [NvShader new]; s->stage = stage; strcpy(s->key, "0123456789abcdef");
    s->fn = [lib newFunctionWithName:name]; s->ae = [s->fn newArgumentEncoderWithBufferIndex:0]; s->arg_len = s->ae.encodedLength;
    s->rf.nb = 2; s->rf.htab = s->rf.bindless = 1;
    s->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEXH, .aid=0, .dtt=MTLTextureType2D};
    s->rf.b[1] = (nvn_rbind){.kind=NVN_RB_SAMPH, .aid=1};
    return s;
}
static void draw_handles(NvShader *fs, id<MTLRenderPipelineState> pso, int x) {
    CHECK(rp_begin() != nil, "production render pass open");
    enc_r_pso(g_enc, pso); enc_r_viewport(g_enc, (MTLViewport){x, 0, 4, 4, 0, 1});
    CHECK(encode_args((id<MTLCommandEncoder>)g_enc, fs, 1, 0), "production fragment arguments encoded");
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
}
static uint64_t bound(NvShader *s, int i) { return ((uint64_t *)((uint8_t *)s->ab_buf.contents + s->ab_off))[i]; }
// The texture table's handles were read again (1) or kept from the last look (0): htab_gpu as encode_args calls it
static int reread(NvShader *s) {
    uint64_t reads = g_stat_htab_reads; __unsafe_unretained id<MTLBuffer> pb = nil;
    os_unfair_lock_lock(&g_rmu); htab_gpu(s->stage, &s->rf.b[0], &pb); os_unfair_lock_unlock(&g_rmu);
    return g_stat_htab_reads != reads;
}
static void render_case(NvShader *fs, id<MTLRenderPipelineState> pso, uint64_t first, uint64_t second, int partial, int unrelated) {
    memset(mem, 0, 64); memcpy(mem, &first, 8);
    tex_t output; id<MTLTexture> target = texture(&output, 8, 4);
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; CHECK(result != nil && target != nil, "render readback storage allocated");
    if (!result || !target) return;
    res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    S.ubo[1][0] = (p_bind_buf){.addr=(uint64_t)mem, .size=16};
    R.has_rt=1; R.rt.n=1; R.rt.color[0]=&output; R.clear_c[0]=1; R.cc[0]=MTLClearColorMake(0, 0, 0, 1);
    draw_handles(fs, pso, 0); uint64_t before[2] = {bound(fs, 0), bound(fs, 1)}, pass = g_pass.serial;
    if (unrelated) mtl_sub_update(&second, (uint64_t)mem + 32, 8);
    else if (partial) { mtl_sub_update(&second, (uint64_t)mem, 4); mtl_sub_update((uint8_t *)&second + 4, (uint64_t)mem + 4, 4); }
    else mtl_sub_update(&second, (uint64_t)mem, 8);
    CHECK(g_enc && g_pass.serial == pass && g_nubd, "the inline update stays deferred in the same pass");
    if (unrelated) CHECK(!reread(fs), "a write beside the table keeps it without reading the handles again");
    draw_handles(fs, pso, 4);
    CHECK(!reread(fs), "no write since the last look keeps the table without reading the handles again");
    if (unrelated) CHECK(bound(fs, 0)==before[0] && bound(fs, 1)==before[1], "unrelated update keeps identical resource tables");
    else CHECK(bound(fs, 0)!=before[0] || bound(fs, 1)!=before[1], "changed handle gets a new immutable resource table");
    rp_end(); enc_b_copy_texture_buffer(blit(), target, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(8,4,1), result, 0, 256, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "real GPU submission completed");
    int left=1,right=1; int first_green=(first & 0xfffff)==3;
    for (int y=0;y<4;y++) for (int x=0;x<8;x++) {
        const uint8_t *p=(const uint8_t *)result.contents+y*256+x*4;
        int green=x<4?first_green:unrelated?first_green:!first_green;
        int ok=p[0]==(green?0:255) && p[1]==(green?255:0) && p[2]==0 && p[3]==255;
        if(x<4)left&=ok;else right&=ok;
    }
    CHECK(left,"earlier draw keeps its original literal colour"); CHECK(right,"later draw has the updated literal colour");
    mtl_tex_release(&output);
}
static void compute_case(NvShader *cs, id<MTLComputePipelineState> pso, int reverse) {
    uint64_t first=(reverse?2:1)|(1ull<<20),second=(reverse?1:2)|(1ull<<20); memset(mem,0,64);memcpy(mem,&first,8);
    id<MTLBuffer> result=[g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    CHECK(result != nil, "compute readback storage allocated"); if (!result) return; res_add(result);
    state_reset();memset(&R,0,sizeof R);mtl_sub_begin();S.ubo[5][0]=(p_bind_buf){.addr=(uint64_t)mem,.size=16};
    for(int i=0;i<2;i++) {
        if(i)mtl_sub_update(&second,(uint64_t)mem,8);
        id<MTL4ComputeCommandEncoder> ce=dispatch_enc();enc_c_pso(ce,pso);
        CHECK(encode_args((id<MTLCommandEncoder>)ce,cs,5,0),"production compute handle tables encoded");
        enc_c_args(ce,result.gpuAddress+16*i,1);enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));
    }
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull)&&!mtl_last_commit_error(),"compute GPU submission completed");
    const float *p=result.contents;
    CHECK(p[0]==(reverse?0:1)&&p[1]==(reverse?1:0)&&p[2]==0&&p[3]==1,"first dispatch reads the current CPU backing in a new submission");
    CHECK(p[4]==(reverse?1:0)&&p[5]==(reverse?0:1)&&p[6]==0&&p[7]==1,"second dispatch reads the new handle after inline update");
}
// Cross-submission arena lifetime and repeated-table reuse: unchanged handle tables must emit
// into the new submission's frame arena (never reading an earlier reclaimed arena slice) without
// rebuilding or bumping g_psnap_gen, including across arena reclamation and unrelated registrations.
static void cross_sub_case(NvShader *cs, id<MTLComputePipelineState> pso, const tex_t *unrelated_tex) {
    uint8_t *ubo = mem + 0x100;
    uint64_t handle = 1 | (1ull << 20);
    memset(ubo, 0, 64); memcpy(ubo, &handle, 8);
    id<MTLBuffer> result = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    CHECK(result != nil, "cross-sub readback storage allocated"); if (!result) return; res_add(result);

    // Submission 1: initial build of a fresh UBO handle table.
    uint64_t builds0 = g_stat_htab_builds;
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)ubo, .size = 16};
    id<MTL4ComputeCommandEncoder> ce = dispatch_enc(); enc_c_pso(ce, pso);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, cs, 5, 0), "cross-sub 1 handle tables encoded");
    enc_c_args(ce, result.gpuAddress, 1); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    uint64_t sub1_gpu = g_htab_last ? g_htab_last->gpu : 0;
    uint64_t builds1 = g_stat_htab_builds, pgen1 = g_psnap_gen;
    CHECK(builds1 == builds0 + 2, "initial submission builds texture and sampler handle tables once");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "cross-sub 1 completed");
    const float *p = result.contents;
    CHECK(p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 1, "cross-sub 1 reads red texture");

    // Submission 2 (shared arena continuation): unchanged UBO re-emits into new submission without rebuild or g_psnap_gen bump.
    memset(result.contents, 0, 16);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)ubo, .size = 16};
    ce = dispatch_enc(); enc_c_pso(ce, pso);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, cs, 5, 0), "cross-sub 2 handle tables encoded");
    enc_c_args(ce, result.gpuAddress, 1); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    CHECK(g_htab_last && g_htab_last->epoch == g_arena_epoch && g_htab_last->gpu != sub1_gpu, "cross-sub 2 re-emits into the current submission arena");
    CHECK(g_stat_htab_builds == builds1 && g_psnap_gen == pgen1, "unchanged table across submissions skips rebuild and keeps g_psnap_gen");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "cross-sub 2 completed");
    CHECK(p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 1, "cross-sub 2 reads red texture from re-emitted slice");

    // Submission 3 (reclaimed arena overwritten from offset 0 before htab_gpu): proves earlier arena slices are never reused.
    g_arena_next = nil;
    memset(result.contents, 0, 16);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)ubo, .size = 16};
    __unsafe_unretained id<MTLBuffer> scratch = nil;
    arena_at clobber = arena_take_u(4096, &scratch);
    memset(clobber.cpu, 0, 4096);
    ce = dispatch_enc(); enc_c_pso(ce, pso);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, cs, 5, 0), "cross-sub 3 handle tables encoded after arena reclamation");
    enc_c_args(ce, result.gpuAddress, 1); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    CHECK(g_htab_last && g_htab_last->epoch == g_arena_epoch && g_htab_last->gpu >= clobber.gpu + 4096, "re-emitted table lands past overwritten recycled arena region");
    CHECK(g_stat_htab_builds == builds1 && g_psnap_gen == pgen1, "reclaimed-arena re-emit still skips rebuild and keeps g_psnap_gen");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "cross-sub 3 completed");
    CHECK(p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 1, "cross-sub 3 reads red texture after reclaimed arena clobber");

    // Submission 4 (unrelated registration bumps g_reg_gen): resolved elements match, so re-emit without rebuild or g_psnap_gen bump.
    mtl_reg_tex(9, unrelated_tex, NULL);
    memset(result.contents, 0, 16);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)ubo, .size = 16};
    ce = dispatch_enc(); enc_c_pso(ce, pso);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, cs, 5, 0), "cross-sub 4 handle tables encoded after unrelated registration");
    enc_c_args(ce, result.gpuAddress, 1); enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    CHECK(g_stat_htab_builds == builds1 && g_psnap_gen == pgen1, "unrelated registration with unchanged table elements skips rebuild and keeps g_psnap_gen");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "cross-sub 4 completed");
    CHECK(p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 1, "cross-sub 4 reads red texture after unrelated registration");
    mtl_reg_tex(9, NULL, NULL);
}
int main(void) { @autoreleasepool {
    alarm(45);setenv("DM_PSO_ARCHIVE","off",1);setenv("DM_PSO_PREBUILD","0",1);setenv("DM_PSO_MANIFEST","off",1);setenv("DM_PSO_USER","/dev/null",1);setenv("DM_PSO_RECORD","/dev/null",1);
    mtl_init();NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct TP {uint n;ulong seen;array<texture2d<float>,1> e;};struct SP {uint n;ulong seen;array<sampler,1> e;};\n"
        "struct A {constant TP*t[[id(0)]];constant SP*s[[id(1)]];};\n"
        "float4 value(constant A&a){return a.t->e[0].sample(a.s->e[0],float2(1.25,0.5),level(0));}\n"
        "vertex float4 v(uint i[[vertex_id]]){return float4(i==1?3.0:-1.0,i==2?3.0:-1.0,0,1);}\n"
        "fragment float4 f(constant A&a[[buffer(0)]]){return value(a);}\n"
        "kernel void k(constant A&a[[buffer(0)]],device float4*out[[buffer(1)]]){out[0]=value(a);}\n" options:nil error:&error];
    CHECK(lib!=nil,"synthetic library compiles");if(!lib){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 2;}
    NvShader *fs=shader(lib,@"f",1),*cs=shader(lib,@"k",5);
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[lib newFunctionWithName:@"v"];d.fragmentFunction=fs->fn;d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pso=[g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    id<MTLComputePipelineState> cps=[g_dev newComputePipelineStateWithFunction:cs->fn error:&error];CHECK(pso&&cps,"render and compute pipelines compile");if(!pso||!cps)return 2;
    mem=mmap(NULL,0x10000,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);CHECK(mem!=MAP_FAILED,"uniform backing allocated");if(mem==MAP_FAILED)return 2;mtl_pool_add(mem,0x10000);
    tex_t t[3];const uint8_t values[3][8]={{255,0,0,255,255,0,0,255},{0,255,0,255,0,255,0,255},{255,0,0,255,0,255,0,255}};
    for(int i=0;i<3;i++){id<MTLTexture> tex=texture(&t[i],2,1);if(!tex)return 2;[tex replaceRegion:MTLRegionMake2D(0,0,2,1) mipmapLevel:0 withBytes:values[i] bytesPerRow:8];t[i].mem=mem+0x1000*(i+1);t[i].L.size=8;memcpy((void *)t[i].mem,values[i],8);mtl_reg_tex(i+1,&t[i],NULL);}
    samp_par sp={.minf=NVN_MIN_FILTER_NEAREST,.magf=NVN_MAG_FILTER_NEAREST,.ws=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wt=NVN_WRAP_MODE_CLAMP_TO_EDGE,.wr=NVN_WRAP_MODE_CLAMP_TO_EDGE,.lod_max=1000};mtl_reg_samp(1,&sp);sp.ws=NVN_WRAP_MODE_REPEAT;mtl_reg_samp(2,&sp);
    render_case(fs,pso,1|(1ull<<20),2|(1ull<<20),0,0);
    render_case(fs,pso,3|(1ull<<20),3|(2ull<<20),0,0);
    render_case(fs,pso,1|(1ull<<20),2|(1ull<<20),1,0);
    render_case(fs,pso,1|(1ull<<20),2|(1ull<<20),0,1);
    compute_case(cs,cps,0);
    compute_case(cs,cps,1);
    cross_sub_case(cs,cps,&t[0]);
    printf("t_htab_update: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
