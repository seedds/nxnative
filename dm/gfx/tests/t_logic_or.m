// OR accumulation and COPY replacement through production shader/pipeline/draw paths.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int command(NSArray *args) {
    NSTask *t=[NSTask new]; t.executableURL=[NSURL fileURLWithPath:@"/usr/bin/xcrun"]; t.arguments=args;
    NSError *e=nil; if (![t launchAndReturnError:&e]) return 1; [t waitUntilExit]; return t.terminationStatus;
}
int main(void) { @autoreleasepool {
    mtl_init(); char scratch[]="/private/tmp/nvn-logic-or-XXXXXX"; if (!mkdtemp(scratch)) return 2;
    NSString *dir=@(scratch); NSMutableString *src=[NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 vs(uint v [[vertex_id]]) { return float4(v==1?3.0:-1.0,v==2?3.0:-1.0,0.5,1); }\n"];
    const unsigned value[3][2]={{0x80000001,0x00020002},{0x04000404,0x40000008},{0,0}};
    for (int i=0;i<3;i++) [src appendFormat:@"fragment float4 f%d() { return as_type<float4>(uint4(%u,%u,0,0)); }\nfragment uint4 u%d() { return uint4(%u,%u,0,0); }\nfragment uint4 o%d(uint4 previous [[color(0)]]) { return uint4(%u,%u,0,0)|previous; }\n",i,value[i][0],value[i][1],i,value[i][0],value[i][1],i,value[i][0],value[i][1]];
    NSString *metal=[dir stringByAppendingPathComponent:@"fixture.metal"], *air=[dir stringByAppendingPathComponent:@"fixture.air"], *lib=[dir stringByAppendingPathComponent:@"fixture.metallib"];
    [src writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if (command(@[@"-sdk",@"macosx",@"metal",@"-c",metal,@"-o",air]) || command(@[@"-sdk",@"macosx",@"metallib",air,@"-o",lib])) return 2;
    NSString *old=g_msl_dir; g_msl_dir=dir; NvShader *vs=nil; prog_t pg[3]; memset(pg,0,sizeof pg);
    for (int i=-1;i<3;i++) {
        NSString *key=i<0?@"logic_vs":[NSString stringWithFormat:@"logic_fs%d",i];
        NSMutableDictionary *j=[@{@"route":@"B",@"chain":@DMSH_CHAIN,@"entry":i<0?@"vs":[NSString stringWithFormat:@"f%d",i],@"bindings":@[],@"c1":@"",@"vertex_inputs":@[],@"raster":@{@"colors":i<0?@[]:@[@0]}} mutableCopy];
        if (i>=0) { j[@"entry_u"]=[NSString stringWithFormat:@"u%d",i]; j[@"entry_u_or"]=[NSString stringWithFormat:@"o%d",i]; }
        [NSFileManager.defaultManager copyItemAtPath:lib toPath:[dir stringByAppendingPathComponent:[key stringByAppendingString:@".metallib"]] error:nil];
        [[NSJSONSerialization dataWithJSONObject:j options:0 error:nil] writeToFile:[dir stringByAppendingPathComponent:[key stringByAppendingString:@".json"]] atomically:YES];
        NvShader *sh=shader_load(key,i<0?0:1,0); if (!sh) return 2;
        if (i<0) vs=sh; else { NvProg *pr=[NvProg new]; pr->sh[0]=vs; pr->sh[1]=sh; pg[i].serial=995800+i; serial_put(pg[i].serial,pr); }
    }
    g_msl_dir=old; tex_t target={.magic=MAGIC('T','E','X',0),.serial=995810};
    target.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RG32UI,.width=2,.height=2,.depth=1,.levels=1,.samples=1};
    nvn_tex_layout(&target.d,&target.L); mtl_tex_init(&target);
    state_reset(); memset(&R,0,sizeof R); R.rt.n=1; R.rt.color[0]=&target; R.has_rt=1; R.clear_c[0]=1; R.cc[0]=MTLClearColorMake(0,0,0,0);
    const int shader[]={0,1,2,1,0}, op[]={7,7,7,3,7}; const uint32_t expected[][2]={{0x80000001,0x00020002},{0x84000405,0x4002000a},{0x84000405,0x4002000a},{0x04000404,0x40000008},{0x84000405,0x4002000a}}; int failed=0;
    for (int i=0;i<5;i++) {
        S.prog=&pg[shader[i]]; S.color.logic_op=op[i]; g_pso_dirty=1; p_draw dw={.prim=4,.count=3,.instances=1};
        mtl_sub_begin(); draw(&dw,NULL); mtl_sub_commit(); mtl_drain();
        id<MTLBuffer> b=[g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb=[g_q commandBuffer]; id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
        [e copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(2,2,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:32];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted]; const uint32_t *got=b.contents;
        for (int px=0;px<4;px++) if (got[2*px]!=expected[i][0] || got[2*px+1]!=expected[i][1]) { failed++; fprintf(stderr,"FAIL step%d pixel%d op%d expected %u,%u got %u,%u\n",i,px,op[i],expected[i][0],expected[i][1],got[2*px],got[2*px+1]); }
    }
    // A remapped OR draw cannot use the float readable-tile variant. Seed the
    // normal specialization cache so rejection cannot pass just because a file is missing.
    NvProg *last=prog_get(&pg[0]); NvShader *fs=last->sh[1];
    fs->rf.color_mask=1; fs->rf.nfo=1;
    memset(&g_fz,0,sizeof g_fz); g_fz.active=1; g_fz.phase=2; g_fz.p2map[0]=1;
    size_t kl=strlen(fs->key); NSString *base=[NSString stringWithFormat:@"%.*s",(int)(kl-2),fs->key];
    g_bf_lib_cache=[NSMutableDictionary dictionary]; g_bf_fn_cache=[NSMutableDictionary dictionary];
    NSError *error=nil; g_bf_lib_cache[base]=lib_from_file(lib,&error);
    NSString *ck=[base stringByAppendingString:@"|00/ff/00/ff/ff/ff/ff/ff/ff"];
    g_bf_fn_cache[ck]=fs->fn_u;
    S.prog=&pg[0]; S.color.logic_op=NVN_LOGIC_OP_OR;
    p_draw remapped={.prim=4,.count=3,.instances=1}; id<MTLFunction> selected=nil; uint8_t lmap[8];
    if (fz_p2_draw(last,&remapped,NULL,lmap,&selected)) { failed++; fprintf(stderr,"FAIL OR admitted to float readable-tile specialization\n"); }
    memset(&g_fz,0,sizeof g_fz);
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil]; printf("logic OR: 20 pixel checks, %d failed\n",failed); return failed!=0;
} }
