// Experimental camera-only interpolation preview. This replaces the pre-UI scene with its
// intermediate image; it does NOT yet present two frames per game update or claim object motion.
static id<MTL4FXFrameInterpolator> g_temporal_interp;
static id<MTLTexture> g_temporal_color[2], g_temporal_motion, g_temporal_output;
static id<MTLComputePipelineState> g_temporal_motion_ps;
static uint64_t g_temporal_encoded_frame = UINT64_MAX, g_temporal_previous_frame = UINT64_MAX, g_temporal_previous_ns;
static int g_temporal_next, g_temporal_disabled;
static nvn_camera_input g_temporal_previous_camera;
static MTLPixelFormat g_temporal_ui_format;
// Mode 2 captures the reconstructed scene before UI and interpolates once UI is complete.
static id<MTLTexture> g_temporal_full_scene, g_temporal_full_ui, g_temporal_game_ui, g_temporal_game_depth;
static nvn_camera_input g_temporal_full_camera;
static uint64_t g_temporal_depth_frame=UINT64_MAX, g_temporal_full_frame=UINT64_MAX, g_temporal_ui_serial;
static int g_temporal_ui_seen;
static id<MTLTexture> g_temporal_pair_ui;
static uint64_t g_temporal_pair_frame=UINT64_MAX,g_temporal_pair_final_frame=UINT64_MAX,g_temporal_pair_final_serial;
static int g_temporal_pair_viewport[4];
static uint64_t g_temporal_flow[32]; static unsigned g_temporal_flow_n;
static struct {uint64_t scenes,camera_missing,captured,ui_complete,encoded,resets,camera_cuts,failed;} g_temporal_coverage;

static void temporal_drop_size(void) {
    id<MTLTexture> textures[] = {g_temporal_color[0], g_temporal_color[1], g_temporal_motion, g_temporal_output};
    for (int i=0;i<4;i++) if (textures[i]) { res_remove(textures[i]); defer_obj_release(textures[i]); }
    g_temporal_color[0] = g_temporal_color[1] = g_temporal_motion = g_temporal_output = nil;
    if (g_temporal_interp) defer_obj_unref(g_temporal_interp);
    g_temporal_interp = nil; g_temporal_next = 0; g_temporal_previous_frame = UINT64_MAX;
    memset(&g_temporal_previous_camera,0,sizeof g_temporal_previous_camera);
}
static id<MTLTexture> temporal_texture(MTLPixelFormat format, NSUInteger w, NSUInteger h, MTLTextureUsage usage) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = usage;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    if (t) res_add(t);
    return t;
}
static int temporal_motion_pipeline(void) {
    if(g_temporal_motion_ps)return 1;
    NSError *error=nil;id<MTLLibrary> library=host_lib("camera_motion",&error);id<MTLFunction> fn=[library newFunctionWithName:@"camera_motion_composed"];
    if(fn)g_temporal_motion_ps=[g_dev newComputePipelineStateWithFunction:fn error:&error];
    return g_temporal_motion_ps!=nil;
}
static id<MTL4FXFrameInterpolator> temporal_create_formats(NSUInteger w,NSUInteger h,NSUInteger input_w,NSUInteger input_h,MTLPixelFormat color_format,MTLPixelFormat output_format,MTLPixelFormat ui_format) {
    NSError *error = nil;
    if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
    temporal_motion_pipeline();
    MTLFXFrameInterpolatorDescriptor *d = [MTLFXFrameInterpolatorDescriptor new];
    d.inputWidth = input_w; d.inputHeight = input_h; d.outputWidth = w; d.outputHeight = h;
    d.uiTextureFormat = ui_format;
    d.colorTextureFormat=color_format;d.outputTextureFormat=output_format;
    d.depthTextureFormat = MTLPixelFormatDepth32Float; d.motionTextureFormat = MTLPixelFormatRG16Float;
    id<MTL4FXFrameInterpolator> f = g_fx_comp ? [d newFrameInterpolatorWithDevice:g_dev compiler:g_fx_comp] : nil;
    if (!f || !g_temporal_motion_ps) { MLOG("[RED] interpolation creation failed: %s", error.localizedDescription.UTF8String ?: "MetalFX refused the format"); return nil; }
    return f;
}
static id<MTL4FXFrameInterpolator> temporal_create(NSUInteger w,NSUInteger h,NSUInteger input_w,NSUInteger input_h,MTLPixelFormat ui_format) {
    return temporal_create_formats(w,h,input_w,input_h,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA16Float,ui_format);
}
static int temporal_size(NSUInteger w, NSUInteger h, NSUInteger input_w, NSUInteger input_h, MTLPixelFormat ui_format) {
    if (g_temporal_disabled) return 0;
    if (g_temporal_interp && g_temporal_output.width == w && g_temporal_output.height == h && g_temporal_motion.width == input_w && g_temporal_motion.height == input_h && g_temporal_ui_format == ui_format) return 1;
    temporal_drop_size();
    id<MTL4FXFrameInterpolator> f=temporal_create(w,h,input_w,input_h,ui_format);
    if(!f){g_temporal_disabled=1;return 0;}
    g_temporal_interp = f; g_temporal_ui_format = ui_format;
    for (int i=0;i<2;i++) g_temporal_color[i] = temporal_texture(MTLPixelFormatRGBA16Float,w,h,f.colorTextureUsage|MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
    g_temporal_motion = temporal_texture(MTLPixelFormatRG16Float,input_w,input_h,f.motionTextureUsage|MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead);
    g_temporal_output = temporal_texture(MTLPixelFormatRGBA16Float,w,h,f.outputTextureUsage|MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    if (!g_temporal_color[0] || !g_temporal_color[1] || !g_temporal_motion || !g_temporal_output) { temporal_drop_size(); g_temporal_disabled=1; return 0; }
    res_commit();
    return 1;
}
// One opt-in moving-scene readback for checking real game inputs. Disabled in normal play.
// Capture happens on the GPU timeline; reading live textures later would race their next use.
static void temporal_capture(id<MTLTexture> current,id<MTLTexture> previous,id<MTLTexture> depth,id<MTLTexture> motion,id<MTLTexture> output,id<MTLTexture> ui,nvn_camera_input camera,uint64_t frame,int reset) {
    static int captured, consecutive;
    const char *path=getenv("DM_OOT_TEMPORAL_CAPTURE");
    if(!path || !*path || captured) return;
    consecutive=reset?0:consecutive+1;
    float delta=0;
    for(int r=0;r<4;r++)for(int c=0;c<4;c++) {
        float v=0;for(int k=0;k<4;k++)v+=camera.previous_world_to_clip[r*4+k]*camera.clip_to_world[k*4+c];
        delta+=fabsf(v-(r==c));
    }
    // Keep the real inputs even when irregular game frames prevent framework warm-up.
    // The metadata records history age; this capture alone never certifies a midpoint.
    const char *age_env=getenv("DM_OOT_TEMPORAL_CAPTURE_MIN_HISTORY");
    int min_age=age_env?atoi(age_env):1;if(min_age<1)min_age=1;
    if(consecutive<min_age || delta<.001f) return;
    const char *armed=getenv("DM_OOT_TEMPORAL_CAPTURE_WAIT_FOR_ARM");
    if(armed && atoi(armed)) {
        char marker[1024];int n=snprintf(marker,sizeof marker,"%s/ARM",path);
        if(n<0 || (size_t)n>=sizeof marker || access(marker,F_OK))return;
    }
    captured=1;
    id<MTLTexture> textures[]={current,previous,depth,motion,output,ui};
    const char *names[]={"current.rgba16f","previous.rgba16f","depth.f32","motion.rg16f","generated.rgba16f","ui.rgba16f"};
    const int bpp[]={8,8,4,4,8,8};
    NSString *prefix=@(path);
    NSMutableArray *metadata=[NSMutableArray array];
    id<MTL4ComputeCommandEncoder> ce=(id<MTL4ComputeCommandEncoder>)dispatch_enc();
    for(int i=0;i<6;i++) {
        id<MTLTexture> t=textures[i];if(!t)continue;NSUInteger pitch=(t.width*bpp[i]+255)&~255ul,length=pitch*t.height;
        id<MTLBuffer> b=[g_dev newBufferWithLength:length options:MTLResourceStorageModeShared];
        if(!b){MLOG("[RED] temporal capture: buffer allocation failed");break;}
        res_add(b);
        [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
        [ce copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:length];
        NSString *file=[prefix stringByAppendingPathComponent:@(names[i])];
        [metadata addObject:@{@"file":@(names[i]),@"width":@(t.width),@"height":@(t.height),@"row_bytes":@(pitch)}];
        [g_commit_blocks addObject:^{NSError *error=nil;NSData *data=[NSData dataWithBytesNoCopy:b.contents length:length freeWhenDone:NO];if(![data writeToFile:file options:NSDataWritingAtomic error:&error])MLOG("[RED] temporal capture: %s",error.localizedDescription.UTF8String);}];
        defer_obj_release(b);
    }
    NSMutableArray *inverse_values=[NSMutableArray array],*previous_values=[NSMutableArray array],*current_values=[NSMutableArray array];
    for(int i=0;i<16;i++){[inverse_values addObject:@(camera.clip_to_world[i])];[previous_values addObject:@(camera.previous_world_to_clip[i])];[current_values addObject:@(camera.world_to_clip[i])];}
    NSDictionary *info=@{@"frame":@(frame),@"reset":@(reset),@"camera_delta":@(delta),@"history_age":@(consecutive),@"textures":metadata,@"near":@(camera.near_plane),@"far":@(camera.far_plane),@"fov_radians":@(camera.fov),@"clip_to_world":inverse_values,@"previous_world_to_clip":previous_values,@"world_to_clip":current_values};
    [g_commit_blocks addObject:^{NSError *error=nil;NSData *json=[NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingPrettyPrinted error:&error];[json writeToFile:[prefix stringByAppendingPathComponent:@"capture.json"] atomically:YES];MLOG("temporal moving-scene capture frame %llu saved to %s",(unsigned long long)frame,prefix.UTF8String);}];
    // dispatch_enc's close publishes its readbacks before any later writes to these textures.
}

// Camera uniforms are explicit rows; simd matrices store columns.
static simd_float4x4 temporal_camera_matrix(const float rows[16]) {
    simd_float4x4 m;
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)m.columns[j][i]=rows[4*i+j];
    return m;
}
static void temporal_encode_fx(id<MTL4CommandBuffer> cb,id<MTL4FXFrameInterpolator> f,id<MTL4ArgumentTable> table,id<MTLComputePipelineState> pipeline,NSUInteger w,NSUInteger h,nvn_camera_input camera,float dt,int reset,id<MTLTexture> current,id<MTLTexture> previous,id<MTLTexture> depth,id<MTLTexture> motion,id<MTLTexture> output,id<MTLTexture> ui,id<MTL4CounterHeap> profile) {
        id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch visibilityOptions:resource_vis()];
        [e setComputePipelineState:pipeline]; [e setArgumentTable:table];
        [e dispatchThreadgroups:MTLSizeMake((w+7)/8,(h+7)/8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
        // Publish motion writes before the framework opens its own encoders.
        [e barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()]; [e endEncoding];
        if(profile)[cb writeTimestampIntoHeap:profile atIndex:3];
        f.uiTexture=ui;f.uiTextureComposited=ui!=nil;
        f.colorTexture=current;f.prevColorTexture=previous;f.depthTexture=depth;f.motionTexture=motion;f.outputTexture=output;
        f.motionVectorScaleX=f.motionVectorScaleY=1;f.nearPlane=camera.near_plane;f.farPlane=camera.far_plane;
        f.fieldOfView=camera.fov*180.f/(float)M_PI;f.aspectRatio=(float)w/h;f.deltaTime=dt;f.shouldResetHistory=reset;f.depthReversed=NO;
        // Separate camera transforms let MetalFX distinguish camera motion from
        // independent motion. Legacy/manual inputs without them remain optional.
        if (@available(macOS 27.0, iOS 27.0, tvOS 27.1, *)) {
            f.worldToViewMatrix=camera.has_render_matrices?temporal_camera_matrix(camera.world_to_view):matrix_identity_float4x4;
            f.viewToClipMatrix=camera.has_render_matrices?temporal_camera_matrix(camera.view_to_clip):matrix_identity_float4x4;
        }
        [f encodeToCommandBuffer:cb];
        if(profile)[cb writeTimestampIntoHeap:profile atIndex:4];
}

enum { TEMPORAL_ENCODED = 1, TEMPORAL_RESET = 2, TEMPORAL_SHOWN = 4 };
static int temporal_encode_scene(id<MTLTexture> scene, id<MTLTexture> depth, id<MTLTexture> ui, nvn_camera_input camera, uint64_t frame, uint64_t now, int preview) {
    if (frame == g_temporal_encoded_frame || !scene || !depth || depth.pixelFormat != MTLPixelFormatDepth32Float ||
        depth.width != camera.width || depth.height != camera.height ||
        scene.width * depth.height != scene.height * depth.width ||
        (ui && (ui.width != scene.width || ui.height != scene.height || ui.sampleCount != 1)) ||
        scene.sampleCount != 1 || depth.sampleCount != 1) return 0;
    if (!temporal_size(scene.width, scene.height, depth.width, depth.height, ui ? ui.pixelFormat : MTLPixelFormatInvalid)) return 0;
    id<MTL4FXFrameInterpolator> f = g_temporal_interp;
    if ((depth.usage & f.depthTextureUsage) != f.depthTextureUsage || (ui && (ui.usage & f.uiTextureUsage) != f.uiTextureUsage)) return 0;
    // The API's previous color is the previous encode's input, not necessarily the
    // preceding presentation. Match the game's previous camera to that retained input.
    // Callers without decoded current transforms retain the conservative frame-ID check.
    int have_matrices=camera.has_current_transform && g_temporal_previous_camera.has_current_transform;
    int continuity=have_matrices?nvn_camera_previous_matches(&camera,&g_temporal_previous_camera):frame==g_temporal_previous_frame+1;
    int slot = g_temporal_next, reset = g_temporal_previous_frame == UINT64_MAX || frame < g_temporal_previous_frame || !continuity || now <= g_temporal_previous_ns;
    id<MTLTexture> current = g_temporal_color[slot], previous = reset ? current : g_temporal_color[1-slot], motion = g_temporal_motion, output = g_temporal_output;
    NVNcopyRegion region = {0,0,0,(int)scene.width,(int)scene.height,1};
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit=nil; }
    if (!copy_t2t_draw(scene,current,0,0,0,0,&region,&region,0,0)) return 0;
    rp_end();
    id<MTLBuffer> uniforms; size_t offset = arena_alloc(sizeof(float)*32,&uniforms);
    if (!uniforms) return 0;
    nvn_cam_mul(camera.previous_world_to_clip,camera.clip_to_world,(float *)((uint8_t *)uniforms.contents+offset));
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount=1; ad.maxTextureBindCount=2;
    id<MTL4ArgumentTable> table = [g_dev newArgumentTableWithDescriptor:ad error:nil];
    if (!table) return 0;
    [table setAddress:uniforms.gpuAddress+offset atIndex:0];
    [table setTexture:depth.gpuResourceID atIndex:0]; [table setTexture:motion.gpuResourceID atIndex:1];
    id<MTLComputePipelineState> pipeline = g_temporal_motion_ps;
    NSUInteger w=depth.width,h=depth.height;
    float dt=reset?1.f/60:(now-g_temporal_previous_ns)*1e-9f;
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        temporal_encode_fx(cb,f,table,pipeline,w,h,camera,dt,reset,current,previous,depth,motion,output,ui,nil);
    });
    defer_obj_unref(table); g_last_end_relaxed=0;
    temporal_capture(current,previous,depth,motion,output,ui,camera,frame,reset);
    int shown = preview && !reset && copy_t2t_draw(output,scene,0,0,0,0,&region,&region,0,0);
    if (preview && !reset && !shown) MLOG("[RED] interpolation preview: generated scene could not be copied back");
    args_dirty_all();g_pso_dirty=g_dss_dirty=1;
    g_temporal_coverage.encoded++;g_temporal_coverage.resets+=reset;g_temporal_coverage.camera_cuts+=have_matrices&&!continuity;
    g_temporal_encoded_frame=g_temporal_previous_frame=frame;g_temporal_previous_ns=now;g_temporal_previous_camera=camera;g_temporal_next=1-slot;
    static int notes; if(notes++<3) MLOG("interpolation encoded frame %llu: depth/motion %lux%lu, color %lux%lu, UI=%d reset=%d preview-copy=%d (no extra display frames yet)",(unsigned long long)frame,(unsigned long)w,(unsigned long)h,(unsigned long)scene.width,(unsigned long)scene.height,ui!=nil,reset,shown);
    return TEMPORAL_ENCODED | (reset ? TEMPORAL_RESET : 0) | (shown ? TEMPORAL_SHOWN : 0);
}

// A final scene copy is a proof that UI work is finished even when this frame draws no HUD.
static int temporal_final_copy(NvProg *program,int rect[4]) {
    if(!program || !program->sh[1] || strncmp(program->sh[1]->key,"29e3c9dd92b8a571",16) ||
       R.rt.n!=1 || !R.rt.color[0] || !S.has_vp)return 0;
    int tid=(int)(tex_slot_h(S.tex[1],S.img[1],0)&0xfffff);
    os_unfair_lock_lock(&g_rmu);
    int source_ok=tid>0 && tid<g_ntreg && g_treg[tid].ok && g_treg[tid].t.serial==g_temporal_ui_serial;
    os_unfair_lock_unlock(&g_rmu);
    if(!source_ok)return 0;
    int r[4];
    for(int i=0;i<4;i++){double v=S.vp[0][i];if(!isfinite(v) || v<INT_MIN || v>INT_MAX || v!=(int)v)return 0;r[i]=(int)v;}
    if(r[0]<0 || r[1]<0 || r[2]<=0 || r[3]<=0)return 0;
    if(rect)memcpy(rect,r,sizeof r);
    return 1;
}
static void temporal_after_draw(void) {
    if (!temporal_preview_on()) return;
    // Immutable shader identity of the observed Ocarina pre-UI scene composition.
    // This is a development adapter, not a general "all games" recognition rule.
    NvProg *program = S.prog ? prog_get(S.prog) : nil;
    if(temporal_preview_on()==3 && g_temporal_pair_frame==atomic_load(&g_cur_frame) &&
       temporal_final_copy(program,g_temporal_pair_viewport)) {
        g_temporal_pair_final_frame=g_temporal_pair_frame;g_temporal_pair_final_serial=R.rt.color[0]->serial;
    }
    if (!program || !program->sh[1] || (strncmp(program->sh[1]->key, "9e09b406de439a34", 16) && strncmp(program->sh[1]->key, "99d1ecbf2947464b", 16))) return;
    uint64_t frame = atomic_load(&g_cur_frame), now = now_ns();
    if(temporal_preview_on()>=2 && ++g_temporal_coverage.scenes%120==0)
        MLOG("temporal coverage: scenes %llu, missing matching camera %llu, full captures %llu, UI completions %llu, encoded %llu, resets %llu (camera mismatches %llu), failed %llu",
             (unsigned long long)g_temporal_coverage.scenes,(unsigned long long)g_temporal_coverage.camera_missing,(unsigned long long)g_temporal_coverage.captured,
             (unsigned long long)g_temporal_coverage.ui_complete,(unsigned long long)g_temporal_coverage.encoded,(unsigned long long)g_temporal_coverage.resets,
             (unsigned long long)g_temporal_coverage.camera_cuts,(unsigned long long)g_temporal_coverage.failed);
    static int probes; if(probes++<3) MLOG("interpolation preview: scene composition frame %llu camera frame %llu targets=%d depth=%d",(unsigned long long)frame,(unsigned long long)g_temporal_camera_frame,R.rt.n,R.rt.depth!=NULL);
    if (frame == g_temporal_encoded_frame || !R.rt.n || !R.rt.color[0] || !R.rt.depth) return;
    const nvn_camera_input *selected=temporal_camera_for_depth(frame,R.rt.depth->serial);
    if(!selected){g_temporal_coverage.camera_missing++;return;}
    id<MTLTexture> scene = tex_mtl(R.rt.color[0]), depth = tex_mtl(R.rt.depth);
    nvn_camera_input camera = *selected;
    if ((R.rt.has_cview[0] && ((R.rt.cview[0].set_levels && R.rt.cview[0].base_level) || (R.rt.cview[0].set_layers && R.rt.cview[0].min_layer))) ||
        (R.rt.has_dview && ((R.rt.dview.set_levels && R.rt.dview.base_level) || (R.rt.dview.set_layers && R.rt.dview.min_layer)))) return;
    if (temporal_preview_on()>=2) {
        g_temporal_game_depth=depth;g_temporal_full_camera=camera;g_temporal_depth_frame=frame;
        g_temporal_flow[0]=R.rt.color[0]->serial;g_temporal_flow_n=1;
        return;
    }
    temporal_encode_scene(scene, depth, nil, camera, frame, now, 1);
}


// Follow the compute dependencies from the captured scene into the reconstructed image.
// Each NDSR-VR dispatch can use a different shader; a cached argument block's last
// pass-dump label does not establish that all four dispatches use the same shader.
// Match live bindings and a floating-point output, not guessed texture addresses.
#include "nvn_upscale_audit.inc"
static void temporal_after_dispatch(NvShader *cs) {
    if(g_temporal_disabled || temporal_preview_on()<2 || !cs)return;
    uint64_t frame=atomic_load(&g_cur_frame);
    if(g_temporal_depth_frame!=frame || g_temporal_full_frame==frame || !g_temporal_game_depth)return;
    id<MTLTexture> out=nil;uint64_t serial=0;
    os_unfair_lock_lock(&g_rmu);
    int follows=0;
    for(int i=0;i<cs->rf.nb && !follows;i++) {
        const nvn_rbind *b=&cs->rf.b[i];int id_=0;
        if(b->kind==NVN_RB_TEX)id_=(int)(tex_slot_h(S.tex[5],S.img[5],b->reg)&0xfffff);
        else if(b->kind==NVN_RB_IMG && b->reg<32 && !b->rw && !b->atom)id_=(int)(S.img[5][b->reg]&0xfffff);
        if(id_<=0 || id_>=g_ntreg || !g_treg[id_].ok)continue;
        uint64_t serial_=g_treg[id_].t.serial;
        for(unsigned j=0;j<g_temporal_flow_n;j++)if(g_temporal_flow[j]==serial_){follows=1;break;}
    }
    if(!follows){os_unfair_lock_unlock(&g_rmu);return;}
    upscale_audit_dispatch(cs);
    if(gp_active() && g_gp_state==1)MLOG("[temporal-chain] F%llu compute after pass %llu shader %.16s",(unsigned long long)frame,(unsigned long long)g_stat_pass,cs->key);
    for(int i=0;i<cs->rf.nb;i++) if(cs->rf.b[i].kind==NVN_RB_IMG && cs->rf.b[i].reg<32 && (cs->rf.b[i].rw || cs->rf.b[i].atom)) {
        int id_=(int)(S.img[5][cs->rf.b[i].reg]&0xfffff);uint64_t rid=0;int real=0;
        if(id_<=0 || id_>=g_ntreg || !g_treg[id_].ok)continue;
        uint64_t written=g_treg[id_].t.serial;unsigned j;
        for(j=0;j<g_temporal_flow_n;j++)if(g_temporal_flow[j]==written)break;
        if(j==g_temporal_flow_n && j<32)g_temporal_flow[g_temporal_flow_n++]=written;
        id<MTLTexture> t=img_binding(id_,&cs->rf.b[i],&rid,&real);
        // Packed image stores can use a R32Uint view of an RG11B10Float backing.
        // Sample the color backing only for the full base-level/base-slice view.
        if(t.parentTexture && !t.parentRelativeLevel && !t.parentRelativeSlice &&
           t.parentTexture.pixelFormat==MTLPixelFormatRG11B10Float)t=t.parentTexture;
        id<MTLTexture> backing=tex_mtl(&g_treg[id_].t);
        if(t.pixelFormat==MTLPixelFormatR32Uint && backing.pixelFormat==MTLPixelFormatRG11B10Float &&
           t.width==backing.width && t.height==backing.height)t=backing;
        if(real && t.pixelFormat==MTLPixelFormatRG11B10Float && t.sampleCount==1 &&
           t.width>=g_temporal_game_depth.width && t.width*g_temporal_game_depth.height==t.height*g_temporal_game_depth.width) {
            out=t;serial=g_treg[id_].t.serial;break;
        }
    }
    os_unfair_lock_unlock(&g_rmu);
    if(!out)return;
    MTLPixelFormat retained_format=temporal_preview_on()==3?out.pixelFormat:MTLPixelFormatRGBA16Float;
    if(!g_temporal_full_scene || g_temporal_full_scene.width!=out.width || g_temporal_full_scene.height!=out.height || g_temporal_full_scene.pixelFormat!=retained_format) {
        if(g_temporal_full_scene){res_remove(g_temporal_full_scene);defer_obj_release(g_temporal_full_scene);}
        if(g_temporal_full_ui){res_remove(g_temporal_full_ui);defer_obj_release(g_temporal_full_ui);}
        g_temporal_full_scene=temporal_texture(retained_format,out.width,out.height,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
        g_temporal_full_ui=temporal_preview_on()==3?nil:temporal_texture(MTLPixelFormatRGBA16Float,out.width,out.height,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
        if(!g_temporal_full_scene || (temporal_preview_on()!=3 && !g_temporal_full_ui)){g_temporal_disabled=1;return;}
        res_commit();
    }
    NVNcopyRegion r={0,0,0,(int)out.width,(int)out.height,1};
    if(!copy_t2t_draw(out,g_temporal_full_scene,0,0,0,0,&r,&r,0,0))return;
    g_temporal_coverage.captured++;
    g_temporal_game_ui=out;g_temporal_ui_serial=serial;g_temporal_full_frame=frame;g_temporal_ui_seen=0;
    static int once;if(!once++)MLOG("interpolation full-scene capture: %lux%lu color, %lux%lu depth, before UI",(unsigned long)out.width,(unsigned long)out.height,(unsigned long)g_temporal_game_depth.width,(unsigned long)g_temporal_game_depth.height);
}
static void temporal_before_draw(void) {
    if(temporal_preview_on()<2 || g_temporal_full_frame!=atomic_load(&g_cur_frame) || !g_temporal_game_ui)return;
    // Wait until draws have actually used the UI target and then leave it.
    // Merely seeing a new target immediately after a compute dispatch is insufficient.
    if(R.rt.n && R.rt.color[0] && R.rt.color[0]->serial==g_temporal_ui_serial) {
        g_temporal_ui_seen=1;return;
    }
    if(!g_temporal_ui_seen) {
        NvProg *program=S.prog?prog_get(S.prog):nil;
        if(temporal_preview_on()!=3 || !temporal_final_copy(program,NULL))return;
        static int once;if(!once++)MLOG("interpolation final scene copy completes a frame without UI draws (frame %llu)",(unsigned long long)g_temporal_full_frame);
    }
    g_temporal_ui_seen=0;g_temporal_coverage.ui_complete++;
    id<MTLTexture> ui=g_temporal_game_ui;g_temporal_game_ui=nil;
    if(temporal_preview_on()==3){g_temporal_pair_ui=ui;g_temporal_pair_frame=g_temporal_full_frame;return;}
    NVNcopyRegion r={0,0,0,(int)ui.width,(int)ui.height,1};
    if(!copy_t2t_draw(ui,g_temporal_full_ui,0,0,0,0,&r,&r,0,0))return;
    int status=temporal_encode_scene(g_temporal_full_scene,g_temporal_game_depth,g_temporal_full_ui,g_temporal_full_camera,g_temporal_full_frame,now_ns(),0);
    if(!status)g_temporal_coverage.failed++;
    if((status&TEMPORAL_ENCODED) && !(status&TEMPORAL_RESET)) {
        if(!copy_t2t_draw(g_temporal_output,ui,0,0,0,0,&r,&r,0,0))MLOG("[RED] full-scene interpolation preview copy failed");
        else {static int once;if(!once++)MLOG("interpolation full-scene preview: generated world and composited UI shown (no extra display frames yet)");}
    }
}
