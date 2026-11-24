// Exercise shader loading and lean geometry eligibility.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, expression) do { checks++; if (!(expression)) { fprintf(stderr,"%s\n",label); failed++; } } while (0)
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 3) { fprintf(stderr,"fixture directory and expected mask flag required\n"); return 2; }
    mtl_init(); g_msl_dir=@(argv[1]); g_shcache=[NSMutableDictionary dictionary];
    int mask=atoi(argv[2]);
    NvShader *vs=shader_load(@"v",0,0), *fold=shader_load(@"v",0,1);
    CHECK("ordinary shader loads",vs != nil); CHECK("folded layer shader loads",fold != nil);
    if (!vs || !fold) return 1;
    CHECK("reflection preserves ordinary mask",vs->viewport_mask == mask);
    CHECK("folded later GS ignores prior mask",fold->viewport_mask == 0);
    NvShader *base=[NvShader new], *lean=[NvShader new], *reader=[NvShader new];
    base->geo_invocations=1; base->geo_bg_tried=0x80; base->geo_bgv_v=reader;
    base->geo_lean_tried=1<<3; base->geo_lean_v[3]=lean;
    lean->geo_vertices=3; lean->geo_invocations=1; lean->geo_slot=calloc(GEO_CAPTURE_WORDS,sizeof *lean->geo_slot);
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void k(uint i[[thread_position_in_grid]]){(void)i;}" options:nil error:&error];
    id<MTLFunction> function=lib ? [lib newFunctionWithName:@"k"] : nil;
    CHECK("ordinary compute function",function != nil); if(!function)return 1;
    lean->geo_pso=[g_dev newComputePipelineStateWithFunction:function error:&error];
    CHECK("ordinary compute pipeline",lean->geo_pso != nil); if(!lean->geo_pso)return 1;
    for(int i=0;i<3;i++)g_geo_lean_k[i]=lean->geo_pso;
    NvProg *program=[NvProg new];program->sh[0]=vs;program->sh[1]=[NvShader new];program->sh[2]=base;S.tf_on=0;
    CHECK("prior VS mask uses full GS path",geo_lean_pick(program,NULL,3,1) == (mask ? nil : lean));
    vs->viewport_mask=0;
    CHECK("otherwise eligible draw keeps lean replay",geo_lean_pick(program,NULL,3,1) == lean);
    vs->viewport_mask=1;
    CHECK("mask cannot reapply after GS",geo_lean_pick(program,NULL,3,1) == nil);
    printf("vertex viewport mask: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
