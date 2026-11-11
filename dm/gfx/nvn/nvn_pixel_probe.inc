// Opt-in, one-frame ownership diagnostic. It intentionally restricts selected draws
// to one pixel, so its frame is not a rendering/performance validation.
static uint64_t g_px_frame=UINT64_MAX;
static unsigned g_px_width,g_px_height,g_px_x,g_px_y;
static int g_px_finished,g_px_truncated;
static NSMutableArray *g_px_records;
static NSDictionary *g_px_result;

static int px_probe_active(void) { return !g_px_finished&&g_px_frame==atomic_load(&g_cur_frame); }
static void px_probe_frame(uint64_t frame) {
    static int configured=-1;static const char *arm;
    if(configured<0){arm=getenv("DM_NVN_PIXEL_PROBE_ARM");configured=arm&&*arm;}
    if(!configured||g_px_finished)return;
    if(g_px_frame!=UINT64_MAX) {
        if(frame==g_px_frame)return;
        // Do not use mtl_drain: it discards presentation work for suspend.
        uint64_t fence=g_cb?mtl_sub_commit():mtl_last_fence();
        int complete=mtl_wait(fence,2000000000ull);
        NSMutableArray *rows=[NSMutableArray array];
        if(complete)for(NSDictionary *record in g_px_records) {
            id<MTLBuffer> buffer=record[@"buffer"];NSUInteger offset=[record[@"offset"] unsignedIntegerValue];
            uint64_t samples;memcpy(&samples,(const uint8_t *)buffer.contents+offset,sizeof samples);
            NSMutableDictionary *row=[record mutableCopy];[row removeObjectForKey:@"buffer"];row[@"samples"]=@(samples);[rows addObject:row];
        }
        g_px_result=@{@"frame":@(g_px_frame),@"width":@(g_px_width),@"height":@(g_px_height),@"x":@(g_px_x),@"y":@(g_px_y),@"complete":@(complete),@"truncated":@(g_px_truncated),@"rows":rows};
        const char *out=getenv("DM_NVN_PIXEL_PROBE_OUT");
        if(out&&*out) {
            NSError *error=nil;NSData *json=[NSJSONSerialization dataWithJSONObject:g_px_result options:NSJSONWritingPrettyPrinted error:&error];
            if(!json||![json writeToFile:@(out) options:NSDataWritingAtomic error:&error])MLOG("[RED] pixel probe receipt: %s",error.localizedDescription.UTF8String);
        }
        MLOG("%sPIXEL_PROBE_DONE frame %llu rows %lu complete=%d truncated=%d",complete?"":"[RED] ",(unsigned long long)g_px_frame,(unsigned long)rows.count,complete,g_px_truncated);
        g_px_records=nil;g_px_finished=1;return;
    }
    FILE *f=fopen(arm,"r");if(!f)return;unsigned w,h,x,y;int n=fscanf(f,"%u %u %u %u",&w,&h,&x,&y);fclose(f);
    if(n!=4||!w||!h||x>=w||y>=h)return;
    g_px_frame=frame;g_px_width=w;g_px_height=h;g_px_x=x;g_px_y=y;g_px_records=[NSMutableArray array];
    MLOG("PIXEL_PROBE_ARMED frame %llu target %ux%u pixel %u,%u",(unsigned long long)frame,w,h,x,y);
}
typedef struct { __unsafe_unretained id<MTL4RenderCommandEncoder> encoder;MTLScissorRect scissor;int visibility,active; } px_probe_scope;
static NSString *px_key(NvShader *shader) { return shader?@(shader->key):@"-"; }
static px_probe_scope px_probe_begin(id<MTL4RenderCommandEncoder> en,NvProg *program,MTLScissorRect scissor,unsigned raster_mask) {
    px_probe_scope scope={0};
    if(!px_probe_active()||g_pass.tw!=g_px_width||g_pass.th!=g_px_height||!R.rt.depth||!g_pass.hd||!g_enc_has_vis)return scope;
    if(g_px_x<scissor.x||g_px_y<scissor.y||g_px_x>=scissor.x+scissor.width||g_px_y>=scissor.y+scissor.height)return scope;
    if(g_px_records.count>=4096||g_vis_n>=VIS_SLOTS){g_px_truncated=1;return scope;}
    NSUInteger offset=g_vis_n++*8;memset((uint8_t *)g_vis.contents+offset,0,8);
    if(g_vis_counting) {
        // The diagnostic replaces this draw's normal query slot. Fold its samples
        // into the game's query exactly once instead of silently stealing them.
        if(g_nvs==g_vscap){g_vscap=g_vscap?g_vscap*2:64;g_vs=realloc_or_die(g_vs,g_vscap*sizeof *g_vs,"pixel probe visibility slots");}
        g_vs[g_nvs++]=(vis_slot){(void *)CFBridgingRetain(g_vis),(uint32_t)(offset/8)};
    }
    NvProg *original=S.prog?prog_get(S.prog):nil;
    NSMutableDictionary *record=[@{@"buffer":g_vis,@"offset":@(offset),@"pass":@(g_stat_pass),@"draw":@(g_stat_draw+1),@"depth_serial":@(R.rt.depth->serial),
        @"depth_test":@(S.ds.dt),@"depth_write":@(S.ds.dw),@"color_mask":@(raster_mask),
        @"original_vertex":px_key(original?original->sh[0]:nil),@"executed_vertex":px_key(program->sh[0]),@"fragment":px_key(program->sh[1]),
        @"task":px_key(g_mtk.on?g_mtk.ts:nil),@"mesh":px_key(g_mtk.on?g_mtk.ms:nil)} mutableCopy];
    [g_px_records addObject:record];
    scope=(px_probe_scope){en,scissor,g_enc_vis,1};
    enc_r_scissor(en,(MTLScissorRect){g_px_x,g_px_y,1,1});enc_r_visibility(en,MTLVisibilityResultModeCounting,offset);
    return scope;
}
static void px_probe_end(px_probe_scope *scope) {
    // Index/geometry helpers can end or replace an encoder before an early return.
    if(!scope->active||g_enc!=scope->encoder)return;
    enc_r_scissor(scope->encoder,scope->scissor);
    if(scope->visibility&&g_vis_counting&&g_vis_n<VIS_SLOTS) {
        // Metal 4 forbids selecting the same counting offset twice in an encoder.
        // Continue the game's query in a fresh slot and fold both pieces normally.
        g_vis_cur=(int)g_vis_n++;g_vis_cur_pass=g_pass.serial;
        memset((uint8_t *)g_vis.contents+(NSUInteger)g_vis_cur*8,0,8);
        if(g_nvs==g_vscap){g_vscap=g_vscap?g_vscap*2:64;g_vs=realloc_or_die(g_vs,g_vscap*sizeof *g_vs,"pixel probe visibility slots");}
        g_vs[g_nvs++]=(vis_slot){(void *)CFBridgingRetain(g_vis),(uint32_t)g_vis_cur};
        enc_r_visibility(scope->encoder,MTLVisibilityResultModeCounting,(NSUInteger)g_vis_cur*8);g_enc_vis=g_vis_cur+1;
    } else {
        enc_r_visibility(scope->encoder,MTLVisibilityResultModeDisabled,0);g_enc_vis=0;
        if(g_vis_counting){g_vis_cur=-1;g_vis_cur_pass=0;} // next draw can open an overflow pass
    }
}
