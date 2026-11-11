// Exercises the actual replay path: motion, MetalFX, copies, history and argument lifetime.
// Object motion and real display cadence are outside this offscreen test.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) {failed++;fprintf(stderr,"FAIL: %s\n",label);} } while(0)
static void fill(id<MTLTexture> t, int depth) {
    id<MTLCommandBuffer> cb=q_cb();MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor new];
    if(depth){p.depthAttachment.texture=t;p.depthAttachment.loadAction=MTLLoadActionClear;p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=100.f/99.9f-(10.f/99.9f)/2;}
    else {p.colorAttachments[0].texture=t;p.colorAttachments[0].loadAction=MTLLoadActionClear;p.colorAttachments[0].storeAction=MTLStoreActionStore;p.colorAttachments[0].clearColor=MTLClearColorMake(.25,.5,.75,1);}
    [[cb renderCommandEncoderWithDescriptor:p]endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(!cb.error,"input fill completes");
}
static int pixels(id<MTLTexture> t, const float *want, int channels) {
    NSUInteger pitch=t.width*channels*2;id<MTLBuffer>b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*t.height];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.error)return 0;const __fp16*p=b.contents;
    for(NSUInteger i=0;i<t.width*t.height;i++)for(int k=0;k<channels;k++)if(!isfinite(p[i*channels+k])||fabsf(p[i*channels+k]-want[k])>.01f)return 0;return 1;
}
static void square_content(id<MTLTexture> t,int left,int hud) {
    NSUInteger pitch=t.width*8;id<MTLBuffer>b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];__fp16*p=b.contents;
    for(NSUInteger y=0;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++){int hit=(int)x>=left&&(int)x<left+16&&y>=24&&y<48;NSUInteger i=4*(y*t.width+x);p[i]=hit?1:.01;p[i+1]=p[i+2]=.01;p[i+3]=1;if(hud && x>=8 && x<40 && y>=4 && y<12){p[i]=.01;p[i+1]=1;p[i+2]=.01;}}
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch*t.height sourceSize:MTLSizeMake(t.width,t.height,1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(!cb.error,"moving input upload completes");
}
static void square(id<MTLTexture> t,int left) { square_content(t,left,0); }
static int hud_green(id<MTLTexture> t) {
    id<MTLBuffer>b=[g_dev newBufferWithLength:32*8*8 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(8,4,0) sourceSize:MTLSizeMake(32,8,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:32*8 destinationBytesPerImage:32*8*8];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.error)return 0;const __fp16*p=b.contents;
    for(int i=0;i<32*8;i++) if(fabsf(p[i*4]-.01)>.01 || fabsf(p[i*4+1]-1)>.01 || fabsf(p[i*4+2]-.01)>.01 || fabsf(p[i*4+3]-1)>.01)return 0;return 1;
}
static double center_x(id<MTLTexture>t) {
    NSUInteger pitch=t.width*8;id<MTLBuffer>b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*t.height];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.error)return NAN;
    const __fp16*p=b.contents;double sum=0,weight=0;for(NSUInteger y=0;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++){double w=fmax(0,p[4*(y*t.width+x)]-.1);sum+=(x+.5)*w;weight+=w;}return weight?sum/weight:NAN;
}
static float square_shape_error(id<MTLTexture>t,int left) {
    NSUInteger pitch=t.width*8;id<MTLBuffer>b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*t.height];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.error)return INFINITY;
    const __fp16*p=b.contents;float error=0;
    // Leave out the separate HUD strip, but include both object edges and surrounding background.
    for(NSUInteger y=20;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++) {
        float want=(int)x>=left && (int)x<left+16 && y>=24 && y<48?1:.01f;
        float difference=fabsf(p[4*(y*t.width+x)]-want);
        // Permit a one-pixel antialiased contour. A blended pair leaves additional
        // edges several pixels away and must still fail, regardless of its centroid.
        int on_contour=(y>=23 && y<=48 && (abs((int)x-left)<=1 || abs((int)x-(left+16))<=1)) ||
                       ((int)x>=left-1 && (int)x<=left+16 && (abs((int)y-24)<=1 || abs((int)y-48)<=1));
        if(!on_contour)error=fmaxf(error,difference);
    }
    return error;
}
static void camera_binding_shadow_test(void) {
    float raw[128]={0};
    for(int i=0;i<3;i++){raw[4*i+i]=1;raw[0x150/4+4*i+i]=1;}
    float n=.1f,z=100.f,fov=.872664626f,aspect=16.f/9;
    float projection[16]={1/tanf(fov/2)/aspect,0,0,0,0,1/tanf(fov/2),0,0,0,0,-(z+n)/(z-n),-2*z*n/(z-n),0,0,-1,0};
    memcpy(raw+0x70/4,projection,sizeof projection);memcpy(raw+0x1c0/4,projection,sizeof projection);
    raw[0xe0/4]=n;raw[0xe4/4]=z;raw[0x118/4]=fov;raw[0x120/4]=1536;raw[0x124/4]=864;
    p_bind_buf binding={0};binding.addr=(uint64_t)raw;binding.size=sizeof raw;
    atomic_store(&g_cur_frame,1);g_temporal_camera_frame=UINT64_MAX;
    temporal_camera_note(&binding,0);
    CHECK(g_temporal_camera_frame==1 && g_temporal_camera.width==1536,"direct camera binding decodes");
    id<MTLBuffer>shadow=[g_dev newBufferWithLength:sizeof raw options:MTLResourceStorageModeShared];
    memcpy(shadow.contents,raw,sizeof raw);((float*)shadow.contents)[0x120/4]=2560;((float*)shadow.contents)[0x124/4]=1440;
    g_ubd[0]=(ubd_ent){binding.addr,binding.addr+sizeof raw,shadow.gpuAddress,0,shadow};g_nubd=1;g_ubd_ver++;
    ubd_pf_add(binding.addr,binding.addr+sizeof raw);
    atomic_store(&g_cur_frame,2);temporal_camera_note(&binding,0);
    CHECK(g_temporal_camera_frame==2 && g_temporal_camera.width==2560 && g_temporal_camera.height==1440,"deferred camera binding uses new GPU shadow rather than stale CPU bytes");
    CHECK(raw[0x120/4]==1536,"camera acquisition does not mutate the guest's old bytes");
    nvn_camera_input main_camera=g_temporal_camera,shadow_camera=g_temporal_camera;
    shadow_camera.width=shadow_camera.height=512;shadow_camera.fov=(float)M_PI/2;
    temporal_camera_record(3,0x111,shadow_camera);temporal_camera_record(3,0x222,main_camera);
    const nvn_camera_input *selected=temporal_camera_for_depth(3,0x222);
    CHECK(selected && selected->width==2560 && selected->fov==main_camera.fov,"a secondary camera cannot replace the camera associated with the main depth target");
    selected=temporal_camera_for_depth(3,0x111);
    CHECK(selected && selected->width==512,"secondary camera retains its own depth association");
    CHECK(!temporal_camera_for_depth(4,0x222),"depth association is not reused from an older frame");
    // Restore the deferred-list metadata without landing this synthetic test update.
    ubd_bm_clear();g_nubd=0;g_ubd_ver++;memset(g_ubd_pf,0,sizeof g_ubd_pf);g_ubd_wide=0;g_ubd_env_lo=UINT64_MAX;g_ubd_env_hi=0;
}
int main(void){@autoreleasepool{
    setenv("DM_OOT_INTERPOLATION_PREVIEW","1",1);mtl_init();camera_binding_shadow_test();
    CHECK([MTLFXFrameInterpolatorDescriptor supportsMetal4FX:g_dev],"device supports Metal4 interpolation");
    id<MTLTexture>scene=temporal_texture(MTLPixelFormatRGBA16Float,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    id<MTLTexture>depth=temporal_texture(MTLPixelFormatDepth32Float,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    res_commit();fill(scene,0);fill(depth,1);
    nvn_camera_input camera={0};camera.width=128;camera.height=72;camera.near_plane=.1f;camera.far_plane=100;camera.fov=(float)M_PI/2;
    float x=72.f/128,az=-100.f/99.9f,bz=-10.f/99.9f;
    camera.clip_to_world[0]=1/x;camera.clip_to_world[5]=1;camera.clip_to_world[11]=-1;camera.clip_to_world[14]=1/bz;camera.clip_to_world[15]=az/bz;
    camera.previous_world_to_clip[0]=x;camera.previous_world_to_clip[5]=1;camera.previous_world_to_clip[10]=az;camera.previous_world_to_clip[11]=bz;camera.previous_world_to_clip[14]=-1;
    const float color[4]={.25,.5,.75,1};
    for(uint64_t frame=1;frame<=4;frame++){
        float shift=frame==3?.1f:frame==4?-.1f:0;
        camera.previous_world_to_clip[3]=x*shift;
        mtl_sub_begin();int status=temporal_encode_scene(scene,depth,nil,camera,frame,1000000000ull+frame*16666667ull,1);CHECK(status&TEMPORAL_ENCODED,"scene is encoded");
        CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"interpolation submission completes");
        CHECK(pixels(scene,color,4),"constant scene remains correct after preview");
        float motion[2]={64*x*shift/2,0};CHECK(pixels(g_temporal_motion,motion,2),"actual Metal4 motion pixels have correct sign and scale");
        CHECK(!!(status&TEMPORAL_RESET)==(frame==1),"only first contiguous frame requests a history reset");
        CHECK(!!(status&TEMPORAL_SHOWN)==(frame!=1),"only non-reset frames preview an intermediate image");
    }
    mtl_sub_begin();CHECK(!temporal_encode_scene(scene,depth,nil,camera,4,1100000000ull,1),"duplicate frame rejected");
    CHECK(mtl_wait(mtl_sub_commit(),10000000000ull),"empty duplicate submission completes");
    camera.width=127;mtl_sub_begin();CHECK(!temporal_encode_scene(scene,depth,nil,camera,5,1200000000ull,1),"camera and scene size mismatch rejected");
    CHECK(mtl_wait(mtl_sub_commit(),10000000000ull),"empty rejected submission completes");
    camera.width=128;mtl_sub_begin();int gap_status=temporal_encode_scene(scene,depth,nil,camera,9,1300000000ull,1);CHECK(gap_status&TEMPORAL_ENCODED,"gap starts a new history");
    CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"reset submission completes");
    CHECK(gap_status&TEMPORAL_RESET,"gap requests a new MetalFX history");CHECK(!(gap_status&TEMPORAL_SHOWN),"reset frame is not shown as generated");
    // A camera pan moves every feature by -8 pixels each rendered frame.
    // The preview must lie halfway between the previous and current positions, not duplicate the current frame.
    camera.previous_world_to_clip[3]=.25f;
    for(uint64_t frame=101;frame<=107;frame++) {
        square(scene,96-(int)(frame-101)*8);mtl_sub_begin();
        int status=temporal_encode_scene(scene,depth,nil,camera,frame,2000000000ull+(frame-100)*16666667ull+(frame>=105?300000000ull:0),1);
        CHECK(status&TEMPORAL_ENCODED,"moving scene encodes");
        if(frame==105)CHECK(!(status&TEMPORAL_RESET),"a long interval between consecutive frames preserves valid history");CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"moving interpolation completes");
        // MetalFX may return the current image while rebuilding its internal history.
        // Validate the steady-state midpoint over several consecutive pairs, not only a static image.
        double generated=center_x(g_temporal_output), expected=108-(int)(frame-101)*8;
        if(frame>=104) CHECK(isfinite(generated)&&fabs(generated-expected)<2,"consecutive moving pairs generate their midpoint after history warm-up");
    }
    const float pan_motion[2]={8,0};CHECK(pixels(g_temporal_motion,pan_motion,2),"camera pan motion is eight pixels across the image");
    double center=center_x(scene);
    printf("actual replay intermediate centroid %.3f, expected 60 (previous64 current56)\n",center);
    CHECK(isfinite(center)&&fabs(center-60)<2,"actual replay generates midpoint rather than current image");
    id<MTLTexture> small_depth=temporal_texture(MTLPixelFormatDepth32Float,64,36,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    res_commit();fill(small_depth,1);camera.width=64;camera.height=36;
    for(uint64_t frame=201;frame<=207;frame++) {
        square(scene,96-(int)(frame-201)*8);mtl_sub_begin();
        CHECK(temporal_encode_scene(scene,small_depth,nil,camera,frame,4000000000ull+(frame-200)*16666667ull,1)&TEMPORAL_ENCODED,"lower-resolution depth encodes with full-resolution color");
        CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"mixed-resolution interpolation completes");
        double expected=108-(int)(frame-201)*8;
        if(frame>=204) CHECK(fabs(center_x(scene)-expected)<2,"lower-resolution motion generates full-resolution midpoint");
    }
    const float small_motion[2]={4,0};CHECK(pixels(g_temporal_motion,small_motion,2),"motion remains in depth-resolution pixel units");
    // UI contains the current full scene plus an opaque green HUD. The world must interpolate,
    // while that HUD must remain fixed and sharp in the generated output.
    id<MTLTexture>ui=temporal_texture(MTLPixelFormatRGBA16Float,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    res_commit();
    for(uint64_t frame=301;frame<=307;frame++) {
        int left=96-(int)(frame-301)*8;square(scene,left);square_content(ui,left,1);mtl_sub_begin();
        CHECK(temporal_encode_scene(scene,small_depth,ui,camera,frame,6000000000ull+(frame-300)*16666667ull,1)&TEMPORAL_ENCODED,"composited UI input encodes");
        CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"UI interpolation submission completes");
        if(frame>=304) {
            CHECK(hud_green(scene),"generated frame preserves every HUD pixel");
            CHECK(fabs(center_x(scene)-(108-(int)(frame-301)*8))<2,"world still interpolates under composited UI");
        }
    }
    // Motion supplied by the camera need not describe an independently moving object.
    // Require a sharp translated shape, not merely the right centroid of two ghost images.
    for(int variant=0;variant<2;variant++) {
        camera.previous_world_to_clip[3]=variant?-.25f:0;
        uint64_t first=401+variant*100;
        for(uint64_t frame=first;frame<first+7;frame++) {
            square(scene,96-(int)(frame-first)*8);mtl_sub_begin();
            CHECK(temporal_encode_scene(scene,small_depth,nil,camera,frame,8000000000ull+(frame-400)*16666667ull,1)&TEMPORAL_ENCODED,"independent-object scene encodes");
            CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"independent-object interpolation completes");
            if(frame>=first+3) {
                int midpoint_left=100-(int)(frame-first)*8;
                CHECK(fabs(center_x(scene)-(midpoint_left+8))<2,"independent object reaches midpoint despite absent or opposite camera motion");
                float edge_error=square_shape_error(scene,midpoint_left);printf("object variant %d age %llu shape error %.6f\n",variant,(unsigned long long)(frame-first+1),edge_error);
                CHECK(edge_error<.03f,"independent object has a sharp intermediate shape without double edges");
            }
        }
    }
    camera.previous_world_to_clip[3]=0;
    CHECK(nvn_cam_inverse(camera.clip_to_world,camera.world_to_clip),"test current projection is invertible");
    camera.has_current_transform=1;fill(scene,0);
    for(int i=0;i<3;i++) {
        uint64_t frame=i==0?601:i==1?605:606;
        if(i==2)camera.previous_world_to_clip[3]=.5f; // engine reset its previous camera across a scene cut
        mtl_sub_begin();
        int status=temporal_encode_scene(scene,small_depth,nil,camera,frame,12000000000ull+i*300000000ull,1);
        CHECK(status&TEMPORAL_ENCODED,"decoded-transform history case encodes");
        CHECK(mtl_wait(mtl_sub_commit(),10000000000ull)&&!mtl_last_commit_error(),"decoded-transform history submission completes");
        CHECK(!!(status&TEMPORAL_RESET)==(i!=1),"camera continuity preserves retained input across presentation gaps and resets mismatched cuts");
    }
    printf("t_temporal_preview: %d checks, %d failures\n",checks,failed);return failed!=0;
}}
