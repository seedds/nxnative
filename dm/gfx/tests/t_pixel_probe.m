// Real replay, literal pixels and samples; the unrelated store suite supplies helpers.
#define main existing_store_main
#include "t_unwritten_store.m"
#undef main
#include <unistd.h>
static prog_t px_programs[5];
static int px_generated_path;
static id<MTLBuffer> px_test_indices;
static void px_draw(unsigned index) {
    struct {op_hdr a;p_set_rt rt;op_hdr b;p_bind_program program;op_hdr c;p_draw draw;op_hdr end;} code={0};
    code.a=(op_hdr){OP_SET_RT,sizeof(op_hdr)+sizeof(p_set_rt)};code.rt=(p_set_rt){.n=1,.depth=&depth};code.rt.color[0]=&color;
    code.b=(op_hdr){OP_BIND_PROGRAM,sizeof(op_hdr)+sizeof(p_bind_program)};code.program=(p_bind_program){&px_programs[index],3};
    code.c=(op_hdr){OP_DRAW,sizeof(op_hdr)+sizeof(p_draw)};code.draw=(p_draw){NVN_DRAW_PRIMITIVE_TRIANGLES,0,3};code.end=(op_hdr){OP_END,sizeof(op_hdr)};
    if(px_generated_path)g_geo_program=prog_get(&px_programs[index]);
    mtl_exec((const uint8_t *)&code);
    g_geo_program=nil;
}
static int pixel_probe_test(void){@autoreleasepool {
    alarm(30);setbuf(stdout,NULL);char arm[256],receipt[256];snprintf(arm,sizeof arm,"/tmp/nvn-pixel-probe-%d.arm",getpid());snprintf(receipt,sizeof receipt,"/tmp/nvn-pixel-probe-%d.json",getpid());unlink(arm);
    setenv("DM_NVN_PIXEL_PROBE_ARM",arm,1);if(!getenv("DM_NVN_PIXEL_PROBE_OUT"))setenv("DM_NVN_PIXEL_PROBE_OUT",receipt,1);
    setenv("DM_NVN_TBDR","0",1);mtl_init();mtl_set_frame(9);
    CHECK(g_px_frame==UINT64_MAX,"unarmed frame leaves diagnostic disabled");
    color.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=1,.levels=1,.samples=1};depth.d=color.d;depth.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
    NSError *error=nil;id<MTLLibrary> library=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
      "float2 p(uint n){float2 a[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return a[n];}\n"
      "vertex float4 v0(uint n[[vertex_id]]){return float4(p(n),.5,1);}\n"
      "vertex float4 v1(uint n[[vertex_id]]){return float4(p(n),.25,1);}\n"
      "vertex float4 v2(uint n[[vertex_id]]){return float4(p(n),.6,1);}\n"
      "vertex float4 v3(uint n[[vertex_id]]){return float4(p(n)+float2(8,0),.4,1);}\n"
      "fragment float4 red(){return float4(1,0,0,1);}fragment float4 green(){return float4(0,1,0,1);}fragment float4 blue(){return float4(0,0,1,1);}" options:nil error:&error];
    CHECK(library!=nil,"literal shaders compile");if(!library)return 1;
    for(int i=0;i<5;i++){
        NvProg *p=[NvProg new];p->sh[0]=[NvShader new];p->sh[1]=[NvShader new];
        p->sh[0]->fn=[library newFunctionWithName:[NSString stringWithFormat:@"v%d",i==4?0:i]];
        p->sh[1]->fn=[library newFunctionWithName:i==1?@"green":i==4?@"blue":@"red"];
        snprintf(p->sh[0]->key,sizeof p->sh[0]->key,"%016x",0x101+i);snprintf(p->sh[1]->key,sizeof p->sh[1]->key,"%016x",0x201+i);
        p->sh[1]->stage=1;p->sh[1]->rf.color_known=1;p->sh[1]->rf.color_mask=1;px_programs[i].serial=99700+i;serial_put(px_programs[i].serial,p);
    }
    px_generated_path=getenv("DM_TEST_EARLY_RETURN")&&atoi(getenv("DM_TEST_EARLY_RETURN"));
    if(px_generated_path){uint32_t indices[]={0,1,2};px_test_indices=[g_dev newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];res_add(px_test_indices);g_geo_indices=px_test_indices.gpuAddress;g_geo_index_bytes=px_test_indices.length;g_geo_count=3;}
    FILE *f=fopen(arm,"w");fprintf(f,"8 8 4 4\n");fclose(f);mtl_set_frame(10);
    CHECK(px_probe_active(),"ARM is consumed at the next frame boundary");
    state_reset();memset(&R,0,sizeof R);mtl_sub_begin();seed();
    int game_query=getenv("DM_TEST_GAME_QUERY")&&atoi(getenv("DM_TEST_GAME_QUERY"));g_vis_on=game_query;g_vis_counting=game_query;
    S.ds=(ds_st){.dt=1,.dw=1,.df=NVN_DEPTH_FUNC_LESS};
    for(int i=0;i<4;i++)px_draw(i);
    CHECK(g_px_records.count==4,"all four candidate draws are recorded");
    CHECK(g_nvs==(game_query?9:0),"diagnostic samples and fresh continuation slots are owned by the game query");
    id<MTL4RenderCommandEncoder> encoder=g_enc;g_px_finished=1;S.ds=(ds_st){.dt=0,.dw=0,.df=NVN_DEPTH_FUNC_ALWAYS};g_dss_dirty=1;px_draw(4);g_px_finished=0;
    CHECK(g_enc==encoder,"sentinel exercises restored state in the same encoder");
    id<MTLBuffer> query=g_vis;NSUInteger slots=g_vis_n;
    CHECK(!vis_in_ring(query),"diagnostic results are outside the recyclable submission ring");
    id<MTLBuffer> pixels=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared],z=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&color) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:z destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256 options:MTLBlitOptionDepthFromDepthStencil];
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"probe GPU work completes");CHECK(!mtl_last_commit_error(),"GPU reports no error");
    int good_color=1,good_depth=1;for(int i=0;i<64;i++){const uint8_t *p=(const uint8_t *)pixels.contents+4*i;good_color&=p[0]==0&&p[1]==0&&p[2]==255&&p[3]==255;good_depth&=((float *)z.contents)[i]==(i==36?.25f:.75f);}
    CHECK(good_color,"scissor is restored: sentinel paints every pixel blue");CHECK(good_depth,"only the chosen depth pixel changed");
    uint64_t total=0;for(NSUInteger i=0;i<slots;i++)total+=((uint64_t *)query.contents)[i];CHECK(total==(game_query?66:2),"visibility mode is restored without polluting diagnostic slots");
    // Start a later native submission in this same diagnostic frame. Normal
    // rendering can recycle a completed ring buffer; retained probe slots cannot.
    mtl_sub_begin();g_px_finished=1;px_draw(4);g_px_finished=0;
    CHECK(g_vis!=query,"next submission cannot reuse the retained diagnostic buffer");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"later submission completes before the receipt");
    mtl_set_frame(11);CHECK([g_px_result[@"complete"] boolValue],"receipt is read after GPU completion");NSArray *rows=g_px_result[@"rows"];
    CHECK(rows.count==4,"receipt retains every draw");uint64_t expected[]={1,1,0,0};for(NSUInteger i=0;i<rows.count;i++)CHECK([rows[i][@"samples"] unsignedLongLongValue]==expected[i],"visible, closer, occluded and outside samples match literal counts");
    CHECK([rows[1][@"original_vertex"] isEqual:@"0000000000000102"],"nearest visible draw has the expected original vertex shader");CHECK([rows[1][@"fragment"] isEqual:@"0000000000000202"],"nearest visible draw has the expected fragment shader");
    CHECK(!px_probe_active(),"diagnostic does not rearm after its frame");unlink(arm);printf("PIXEL_PROBE %d checks, %d failed\n",checks,failed);return failed!=0;
}}

#ifndef NVN_PIXEL_PROBE_NO_MAIN
int main(void) { return pixel_probe_test(); }
#endif
