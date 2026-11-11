// Actual Metal4 snapshot generation and native-ticket lifetime invariants.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void*p){(void)p;return NULL;}
module_t *g_mod;int g_nmod;void svc_thread_dump(void){}
uint64_t now_ns(void){return bp_now();}
static int checks,failed;
static int test_ui_visible=1;
#define CHECK(x,m) do{checks++;if(!(x)){failed++;fprintf(stderr,"FAIL: %s\n",m);}}while(0)
static uint8_t srgb_byte(float x){float v=x<=.0031308f?12.92f*x:1.055f*powf(x,1.f/2.4f)-.055f;return(uint8_t)lroundf(fminf(1,fmaxf(0,v))*255);}
static void pixel(float out[4],int x,int y,int left,int ui) {
    out[0]=x>=left && x<left+16 && y>=24 && y<48?1:.01f;out[1]=out[2]=.01f;out[3]=1;
    if(ui && x>=8 && x<40 && y>=4 && y<12){out[0]=.01f;out[1]=1;}
}
static void upload(id<MTLTexture>t,int left,int ui,int bytes) {
    int packed=t.pixelFormat==MTLPixelFormatRG11B10Float;
    NSUInteger bpp=bytes||packed?4:8,pitch=(t.width*bpp+255)&~255ul;
    id<MTLBuffer>b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];
    memset(b.contents,0,b.length);
    for(NSUInteger y=0;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++) {
        float c[4];pixel(c,(int)x,(int)y,left,ui);
        if(bytes){uint8_t*p=(uint8_t*)b.contents+y*pitch+4*x;for(int k=0;k<3;k++)p[k]=srgb_byte(c[k]);p[3]=255;}
        else if(packed) {
            uint32_t v[3];
            for(int k=0;k<3;k++) {
                __fp16 h=c[k];uint16_t bits;memcpy(&bits,&h,2);unsigned shift=k==2?5:4;
                v[k]=((uint32_t)bits+(1u<<(shift-1))-1+((bits>>shift)&1))>>shift;
            }
            uint32_t value=v[0]|(v[1]<<11)|(v[2]<<22);memcpy((uint8_t*)b.contents+y*pitch+4*x,&value,4);
        }
        else {__fp16*p=(__fp16*)((uint8_t*)b.contents+y*pitch)+4*x;for(int k=0;k<4;k++)p[k]=c[k];}
    }
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch*t.height sourceSize:MTLSizeMake(t.width,t.height,1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(!cb.error,"input upload completes");
}
static NSData *read_bytes(id<MTLTexture>t) {
    NSUInteger pitch=(t.width*4+255)&~255ul;
    id<MTLBuffer>b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer>cb=q_cb();id<MTLBlitCommandEncoder>e=[cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*t.height];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(!cb.error,"readback completes");
    return [NSData dataWithBytes:b.contents length:b.length];
}
static int native_matches(id<MTLTexture>t,int left) {
    NSData*d=read_bytes(t);const uint8_t*p=d.bytes;NSUInteger pitch=(t.width*4+255)&~255ul;
    for(int y=0;y<72;y++)for(int x=0;x<128;x++){float c[4];pixel(c,x,y,left,test_ui_visible);for(int k=0;k<3;k++)if(p[y*pitch+4*x+k]!=srgb_byte(c[k]))return 0;}
    return 1;
}
static int generated_matches(id<MTLTexture>t,int left) {
    NSData*d=read_bytes(t);const uint8_t*p=d.bytes;NSUInteger pitch=(t.width*4+255)&~255ul;double sum=0,weight=0;
    int red=t.pixelFormat==MTLPixelFormatBGRA8Unorm?2:0,blue=red==2?0:2;
    for(int y=0;y<72;y++)for(int x=0;x<128;x++) {
        const uint8_t*q=p+y*pitch+4*x;
        if(y>=4 && y<12 && x>=8 && x<40){if(abs(q[red]-srgb_byte(.01))>1 || abs(q[1]-(test_ui_visible?255:srgb_byte(.01)))>1 || abs(q[blue]-srgb_byte(.01))>1)return 0;}
        if(y>=20){double w=fmax(0,q[red]-50);sum+=(x+.5)*w;weight+=w;}
    }
    double center=weight?sum/weight:NAN;printf("snapshot generated centroid %.3f expected %d\n",center,left+8);
    return isfinite(center)&&fabs(center-(left+8))<1;
}
static int scaled_native_matches(id<MTLTexture> t,int scale,int expected_center) {
    NSData *data=read_bytes(t);const uint8_t *p=data.bytes;NSUInteger pitch=(t.width*4+255)&~255ul;
    int red=t.pixelFormat==MTLPixelFormatBGRA8Unorm?2:0;double sum=0,weight=0;
    const uint8_t *hud=p+8*scale*pitch+20*scale*4;if(test_ui_visible) {if(hud[1]<240 || hud[red]>40)return 0;} else {for(int i=0;i<3;i++)if(abs(hud[i]-srgb_byte(.01f))>1)return 0;}
    for(NSUInteger y=20*scale;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++){
        double value=fmax(0,p[y*pitch+4*x+red]-50);sum+=(x+.5)*value;weight+=value;
    }
    double center=weight?sum/weight:NAN;printf("scaled auxiliary centroid %.3f expected %d\n",center,expected_center*scale);
    return isfinite(center)&&fabs(center-expected_center*scale)<2;
}

static void ui_completion_setup(prog_t *pg,tex_t *window,id<MTLTexture> ui,uint64_t frame) {
    state_reset();memset(&R,0,sizeof R);mtl_set_frame(frame);
    S.prog=pg;S.tex[1][0]=701;S.has_vp=1;S.vp[0][2]=128;S.vp[0][3]=72;
    R.rt.n=1;R.rt.color[0]=window;
    g_temporal_full_frame=frame;g_temporal_game_ui=ui;g_temporal_ui_serial=94001;g_temporal_ui_seen=0;
    g_temporal_pair_ui=nil;g_temporal_pair_frame=g_temporal_pair_final_frame=UINT64_MAX;
}
static void ui_completion_test(void) {
    prog_t pg={.serial=94003};tex_t source={.magic=MAGIC('T','E','X',0),.serial=94001},window={.serial=94002},other={.serial=94004};
    source.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=128,.height=72,.depth=1,.levels=1,.samples=1};
    id<MTLTexture> ui=temporal_snap_texture(MTLPixelFormatRGBA8Unorm,128,72);
    other.d=window.d=source.d;serial_put(source.serial,ui);mtl_reg_tex(701,&source,NULL);mtl_reg_tex(702,&other,NULL);
    NvProg *program=[NvProg new];program->sh[1]=[NvShader new];serial_put(pg.serial,program);
    ui_completion_setup(&pg,&window,ui,100);
    strcpy(program->sh[1]->key,"unrelated");temporal_before_draw();
    CHECK(g_temporal_game_ui==ui&&g_temporal_pair_frame==UINT64_MAX,"a draw of another shader does not complete an unseen UI stage");
    strcpy(program->sh[1]->key,"29e3c9dd92b8a571_b");
    S.tex[1][0]=702;temporal_before_draw();
    CHECK(g_temporal_game_ui==ui&&g_temporal_pair_frame==UINT64_MAX,"the final-copy shader must read this frame's reconstructed texture");
    S.tex[1][0]=701;g_temporal_full_frame=99;temporal_before_draw();
    CHECK(g_temporal_pair_frame==UINT64_MAX,"a final copy cannot reuse an older frame's capture");
    g_temporal_full_frame=100;S.has_vp=0;temporal_before_draw();
    CHECK(g_temporal_pair_frame==UINT64_MAX,"an unseen UI stage needs the final viewport proof");
    S.has_vp=1;S.vp[0][2]=0;temporal_before_draw();
    CHECK(g_temporal_pair_frame==UINT64_MAX,"an empty final viewport does not complete the stage");
    S.vp[0][2]=128;S.vp[0][0]=-1;temporal_before_draw();
    CHECK(g_temporal_pair_frame==UINT64_MAX,"an invalid final viewport does not complete the stage");
    S.vp[0][0]=NAN;temporal_before_draw();
    CHECK(g_temporal_pair_frame==UINT64_MAX,"a non-finite final viewport does not complete the stage");
    S.vp[0][0]=1e30f;temporal_before_draw();
    CHECK(g_temporal_pair_frame==UINT64_MAX,"a final viewport outside integer range is rejected before conversion");
    S.vp[0][0]=0;
    S.has_vp=1;uint64_t before=g_temporal_coverage.ui_complete;temporal_before_draw();
    CHECK(g_temporal_pair_frame==100&&g_temporal_pair_ui==ui,"a direct final copy completes the frame without any HUD draw");
    CHECK(!g_temporal_game_ui&&g_temporal_coverage.ui_complete==before+1,"HUD-free completion consumes its pending stage once");
    temporal_after_draw();
    CHECK(g_temporal_pair_final_frame==100&&g_temporal_pair_final_serial==window.serial,"the unchanged final-copy adapter recognizes the HUD-free output");
    temporal_before_draw();CHECK(g_temporal_coverage.ui_complete==before+1,"the same final draw cannot complete the stage twice");
    ui_completion_setup(&pg,&window,ui,101);R.rt.color[0]=&source;temporal_before_draw();
    CHECK(g_temporal_ui_seen&&g_temporal_game_ui==ui,"drawing into the UI texture keeps the capture pending");
    R.rt.color[0]=&window;temporal_before_draw();
    CHECK(g_temporal_pair_frame==101&&g_temporal_pair_ui==ui,"the existing visible-HUD path still completes on leaving its target");
    state_reset();memset(&R,0,sizeof R);g_temporal_game_ui=g_temporal_pair_ui=nil;
    g_temporal_full_frame=g_temporal_pair_frame=g_temporal_pair_final_frame=UINT64_MAX;g_temporal_ui_seen=0;
}


static int scene_copy_observed(uint64_t frame) {
    mtl_set_frame(frame);g_temporal_depth_frame=UINT64_MAX;g_temporal_flow_n=0;
    temporal_after_draw();return g_temporal_depth_frame==frame;
}
// The game's no-grading path copies its scene with the same shader as the final
// window copy. Only a full floating-point scene with its current depth/camera
// and a separate full-size source may begin the pre-UI dependency chain.
static void scene_copy_test(void) {
    const uint64_t frame=200;
    prog_t pg={.serial=95003};tex_t source={.magic=MAGIC('T','E','X',0),.serial=95001};
    source.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_R11G11B10F,.width=128,.height=72,.depth=1,.levels=1,.samples=1};
    tex_t target=source,depth=source;target.serial=95002;depth.serial=95004;depth.d.format=NVN_FORMAT_DEPTH32F;
    id<MTLTexture> input=temporal_texture(MTLPixelFormatRG11B10Float,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    id<MTLTexture> output=temporal_texture(MTLPixelFormatRG11B10Float,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    id<MTLTexture> z=temporal_texture(MTLPixelFormatDepth32Float,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    serial_put(source.serial,input);serial_put(target.serial,output);serial_put(depth.serial,z);mtl_reg_tex(711,&source,NULL);
    NvProg *program=[NvProg new];program->sh[1]=[NvShader new];serial_put(pg.serial,program);
    state_reset();memset(&R,0,sizeof R);S.prog=&pg;S.tex[1][0]=711;S.has_vp=1;S.vp[0][2]=128;S.vp[0][3]=72;
    R.rt.n=1;R.rt.color[0]=&target;R.rt.depth=&depth;
    nvn_camera_input camera={.width=128,.height=72,.near_plane=.1f,.far_plane=100,.fov=1};
    temporal_camera_record(frame,depth.serial,camera);
    strcpy(program->sh[1]->key,"unrelated");CHECK(!scene_copy_observed(frame),"an unrelated full-size draw is not a scene composition");
    strcpy(program->sh[1]->key,"29e3c9dd92b8a571_b");
    CHECK(!scene_copy_observed(frame+1),"a copy cannot borrow the preceding frame's depth camera");
    R.rt.depth=NULL;CHECK(!scene_copy_observed(frame),"a window copy without depth is not a new scene");R.rt.depth=&depth;
    S.has_vp=0;CHECK(!scene_copy_observed(frame),"a scene copy needs an explicit viewport");S.has_vp=1;
    S.vp[0][2]=64;CHECK(!scene_copy_observed(frame),"a partial viewport is not a full scene copy");S.vp[0][2]=128;
    S.vp[0][0]=1;CHECK(!scene_copy_observed(frame),"an offset viewport is not the full scene");S.vp[0][0]=0;
    S.tex[1][0]=0;CHECK(!scene_copy_observed(frame),"an absent source cannot begin the scene chain");S.tex[1][0]=711;
    mtl_reg_tex(711,&target,NULL);CHECK(!scene_copy_observed(frame),"sampling the output itself is not a proven scene copy");mtl_reg_tex(711,&source,NULL);
    view_t view={.set_levels=1,.base_level=1,.nlevels=1};mtl_reg_tex(711,&source,&view);
    CHECK(!scene_copy_observed(frame),"a source mip view is not the full scene");mtl_reg_tex(711,&source,NULL);
    R.rt.has_cview[0]=1;R.rt.cview[0]=view;CHECK(!scene_copy_observed(frame),"a target mip view is not the full scene");R.rt.has_cview[0]=0;
    id<MTLTexture> rgba=temporal_texture(MTLPixelFormatRGBA8Unorm,128,72,MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget);
    serial_put(target.serial,rgba);CHECK(!scene_copy_observed(frame),"a same-size UI/window target is not a floating-point scene");serial_put(target.serial,output);
    CHECK(scene_copy_observed(frame),"the observed full floating-point scene copy captures current depth and camera");
    CHECK(g_temporal_flow_n==1 && g_temporal_flow[0]==target.serial && g_temporal_game_depth==z,"the scene-copy output begins the real compute dependency chain");
    id<MTLTexture> remembered=g_temporal_game_depth;R.rt.depth=NULL;temporal_after_draw();
    CHECK(g_temporal_game_depth==remembered && g_temporal_flow[0]==target.serial,"a later window copy cannot replace the captured scene");
    R.rt.depth=&depth;strcpy(program->sh[1]->key,"99d1ecbf2947464b_b");S.tex[1][0]=0;
    CHECK(scene_copy_observed(frame),"the original graded scene recognition remains available");
    mtl_reg_tex(711,NULL,NULL);serial_del(pg.serial);serial_del(source.serial);serial_del(target.serial);serial_del(depth.serial);
    state_reset();memset(&R,0,sizeof R);g_temporal_depth_frame=UINT64_MAX;g_temporal_flow_n=0;g_temporal_game_depth=nil;
}

static void diagnostic_flush_test(void) {
    char dir[]="/tmp/dm-framegen-flush-XXXXXX";CHECK(mkdtemp(dir)!=NULL,"diagnostic temporary directory is created");
    const char *vars[]={"DM_FRAMEGEN_TIMES","DM_FRAMEGEN_ASYNC_GPU_TIMES","DM_FRAMEGEN_GPU_TIMES"};char paths[3][1024];
    for(unsigned i=0;i<3;i++){snprintf(paths[i],sizeof paths[i],"%s/trace-%u.tsv",dir,i);setenv(vars[i],paths[i],1);}
    setenv("DM_OOT_INTERPOLATION_PREVIEW","3",1);mtl_init();
    framegen_presented(123,7,0,0);temporal_async_note_gpu(123,"generation",1,1.001);temporal_snap_feedback(123,nil);
    for(unsigned i=0;i<3;i++){
        FILE *f=fopen(paths[i],"r");CHECK(f!=NULL,"diagnostic stream opens its file");
        CHECK(f && fgetc(f)==EOF,"a short diagnostic stream stays buffered during play");if(f)fclose(f);
    }
    // This is the graphics path SIGTERM reaches before the host calls _exit(0).
    mtl_drain();
    for(unsigned i=0;i<3;i++){
        char bytes[512]={0};FILE *f=fopen(paths[i],"r");size_t n=f?fread(bytes,1,sizeof bytes-1,f):0;if(f)fclose(f);
        CHECK(n && !strncmp(bytes,"frame\t",6) && strstr(bytes,"\n123\t"),"graphics drain persists the header and record before _exit");
        unlink(paths[i]);
    }
    rmdir(dir);
}

int main(void){@autoreleasepool{
    alarm(45);int asynchronous=getenv("DM_TEST_ASYNC")!=NULL;
    if(getenv("DM_TEST_FRAMEGEN_FLUSH")){diagnostic_flush_test();printf("t_framegen_trace_flush: %d checks, %d failures\n",checks,failed);return failed!=0;}
    test_ui_visible=!getenv("DM_TEST_NO_UI");
    if(getenv("DM_TEST_UI_COMPLETION"))setenv("DM_OOT_INTERPOLATION_PREVIEW","3",1);
    if(asynchronous){setenv("DM_OOT_INTERPOLATION_PREVIEW","3",1);setenv("DM_OOT_FRAMEGEN_ASYNC","1",1);}
    mtl_init();if(getenv("DM_TEST_UI_COMPLETION")){ui_completion_test();scene_copy_test();}
    pq_item item={0};item.t.serial=101;item.generated.serial=102;g_pq_read=g_pq_done=9;g_pq_inflight=2;
    CHECK(pq_take_phase(&item,10),"first tick chooses generated picture");
    CHECK(g_pq_read==9 && g_pq_done==9,"generated phase leaves native read/completion ticket pending");
    present_complete(0,1,10,1);
    CHECK(g_pq_done==9 && g_pq_inflight==1 && g_present_alloc_done[0]==1,"generated GPU feedback completes only its own display allocator");
    CHECK(!pq_take_phase(&item,10) && g_pq_read==10 && g_pq_done==9,"next tick takes original without early GPU completion");
    present_complete(1,2,10,0);
    CHECK(g_pq_done==10 && g_pq_inflight==0,"original GPU feedback completes the native ticket");
    memset(&item,0,sizeof item);item.t.serial=103;
    CHECK(!pq_take_phase(&item,11) && g_pq_read==11,"native-only fallback uses one tick and native ticket");
    pq_item cropped={.crop={16,8,128,72},.generated_crop={0,0,128,72}};
    CHECK(pq_picture_crop(&cropped,0)==cropped.crop,"native phase retains canvas coordinates");
    CHECK(pq_picture_crop(&cropped,1)==cropped.generated_crop,"compact generated phase uses its own coordinates");
    memset(cropped.generated_crop,0,sizeof cropped.generated_crop);
    CHECK(pq_picture_crop(&cropped,1)==cropped.crop,"legacy generated surfaces retain the shared crop");
    MTLPixelFormat color=getenv("DM_TEST_PACKED_COLOR")?MTLPixelFormatRG11B10Float:MTLPixelFormatRGBA16Float;
    id<MTLTexture>world=temporal_snap_texture(color,128,72);
    id<MTLTexture>ui=temporal_snap_texture(color,128,72);
    id<MTLTexture>depth=temporal_snap_texture(MTLPixelFormatDepth32Float,64,36);
    id<MTLTexture>native=temporal_snap_texture(MTLPixelFormatRGBA8Unorm,160,90);
    res_add(world);res_add(ui);res_add(depth);res_add(native);res_commit();
    id<MTLCommandBuffer>b=q_cb();MTLRenderPassDescriptor*rp=[MTLRenderPassDescriptor new];
    rp.depthAttachment.texture=depth;rp.depthAttachment.loadAction=MTLLoadActionClear;rp.depthAttachment.storeAction=MTLStoreActionStore;rp.depthAttachment.clearDepth=.95095095095;
    [[b renderCommandEncoderWithDescriptor:rp]endEncoding];[b commit];[b waitUntilCompleted];CHECK(!b.error,"depth input clear completes");
    nvn_camera_input camera={0};camera.width=64;camera.height=36;camera.near_plane=.1;camera.far_plane=100;camera.fov=(float)M_PI/2;
    float x=72.f/128,az=-100.f/99.9f,bz=-10.f/99.9f;
    camera.clip_to_world[0]=1/x;camera.clip_to_world[5]=1;camera.clip_to_world[11]=-1;camera.clip_to_world[14]=1/bz;camera.clip_to_world[15]=az/bz;
    camera.previous_world_to_clip[0]=x;camera.previous_world_to_clip[5]=1;camera.previous_world_to_clip[10]=az;camera.previous_world_to_clip[11]=bz;camera.previous_world_to_clip[14]=-1;
    if(getenv("DM_TEST_RENDER_MATRICES")) {
        camera.has_render_matrices=1;
        for(int i=0;i<4;i++)camera.world_to_view[5*i]=1;
        camera.view_to_clip[0]=x;camera.view_to_clip[5]=1;
        camera.view_to_clip[10]=az;camera.view_to_clip[11]=bz;camera.view_to_clip[14]=-1;
        simd_float4x4 projection=temporal_camera_matrix(camera.view_to_clip);
        CHECK(fabsf(projection.columns[3][2]-bz)<1e-7f && projection.columns[2][3]==-1,"actual camera rows become the correct MetalFX column-major projection");
    }
    id<MTLSharedEvent>event=[g_dev newSharedEvent];id<MTL4CommandAllocator>alloc[2]={[g_dev newCommandAllocator],[g_dev newCommandAllocator]};
    int rect[4]={0,0,128,72};id<MTLSharedEvent>aux_gate=[g_dev newSharedEvent];
    for(uint64_t frame=1;frame<=8;frame++) {
        int slot=(frame-1)%2,left=96-(int)(frame-1)*8;
        upload(world,left,0,0);upload(ui,left,test_ui_visible,0);upload(native,left,test_ui_visible,1);
        if(asynchronous && frame==5){id<MTLCommandBuffer>hold=[g_temporal_async_queue commandBuffer];[hold encodeWaitForEvent:aux_gate value:1];[hold commit];}
        temporal_async_readers reader=temporal_async_reader((unsigned)slot);
        [alloc[slot]reset];id<MTL4CommandBuffer>cb=[g_dev newCommandBuffer];[cb beginCommandBufferWithAllocator:alloc[slot]];
        uint64_t generated=temporal_snap_encode_producer(cb,slot,native,rect,frame,1000000000ull+frame*16666667ull,world,ui,depth,camera,asynchronous?event:nil,asynchronous?frame:0,rect);
        [cb endCommandBuffer];temporal_async_copy_wait(reader);[g_q4 commit:&cb count:1];[g_q4 signalEvent:event value:frame];
        CHECK([event waitUntilSignaledValue:frame timeoutMS:10000],"snapshot GPU work completes");
        if(asynchronous && frame==5) {
            CHECK(g_temporal_async_done.signaledValue<atomic_load(&g_temporal_async_latest),"auxiliary gate holds the consumer while input copy completes");
            upload(world,0,0,0);upload(ui,0,test_ui_visible,0);
            b=q_cb();rp.depthAttachment.clearDepth=1;[[b renderCommandEncoderWithDescriptor:rp]endEncoding];[b commit];[b waitUntilCompleted];
            aux_gate.signaledValue=1;
        }
        if(asynchronous)temporal_async_drain();
        if(asynchronous && frame==5) {
            NSData *d=read_bytes(g_temporal_snap[slot].depth_copy);float value;memcpy(&value,d.bytes,4);
            CHECK(fabsf(value-.95095095095f)<.000001f,"auxiliary work uses immutable copied depth after live depth is overwritten");
            b=q_cb();rp.depthAttachment.clearDepth=.95095095095;[[b renderCommandEncoderWithDescriptor:rp]endEncoding];[b commit];[b waitUntilCompleted];
        }
        CHECK(g_temporal_snap[slot].compact_display && g_temporal_snap[slot].display.width==128 && g_temporal_snap[slot].display.height==72,"matching present crop produces a physically compact generated surface");
        CHECK(native_matches(native,left),"host generation leaves the native game's output unchanged");
        if(frame<4)CHECK(!generated,"no extra picture before history and conversion proof are ready");
        else {CHECK(generated!=0 && g_temporal_snap_fidelity==2,"a real generated snapshot becomes available");
              CHECK(generated_matches(g_temporal_snap[slot].display,left+4),"generated snapshot has midpoint motion and unchanged HUD with correct sRGB transfer");}
    }
    if(asynchronous) {
        // A ready native image must bypass a future generation's producer wait.
        id<MTLSharedEvent>future_gate=[g_dev newSharedEvent];
        id<MTLCommandBuffer>future=[g_temporal_async_queue commandBuffer];
        [future encodeWaitForEvent:future_gate value:1];[future commit];
        g_snap[1].serial=(1ull<<61)+99;g_snap[1].tex=native;
        CAMetalLayer *ready_layer=[CAMetalLayer layer];ready_layer.device=g_dev;
        ready_layer.pixelFormat=MTLPixelFormatBGRA8Unorm;ready_layer.framebufferOnly=NO;
        ready_layer.drawableSize=CGSizeMake(128,72);
        id<CAMetalDrawable>ready_drawable=[ready_layer nextDrawable];
        CHECK(ready_drawable!=nil,"ready native regression receives a drawable");
        CHECK(temporal_async_present(g_snap[1].serial,native,0,8,8,rect,ready_drawable,0,0)==1,"ready native display submits while future generation is gated");
        uint64_t ready_epoch=atomic_load(&g_temporal_display_latest);
        CHECK([g_temporal_display_done waitUntilSignaledValue:ready_epoch timeoutMS:1000],"ready native display completes without waiting for future generation");
        CHECK(future.status!=MTLCommandBufferStatusCompleted,"future generation remains gated while the older native image completes");
        future_gate.signaledValue=1;[future waitUntilCompleted];
        CHECK(!future.error,"independent generation lane resumes after producer release");
        temporal_async_drain();
        CHECK(generated_matches(ready_drawable.texture,40),"independent ready native display preserves pixels and HUD");
        // Frame9 reads slot1 as history. Frame10 must not overwrite slot1 while that read is gated.
        id<MTLCommandBuffer>hold=[g_temporal_async_queue commandBuffer];[hold encodeWaitForEvent:aux_gate value:2];[hold commit];
        for(uint64_t frame=9;frame<=10;frame++) {
            int slot=(frame-1)%2,left=96-(int)(frame-1)*8;upload(world,left,0,0);upload(ui,left,test_ui_visible,0);upload(native,left,test_ui_visible,1);
            temporal_async_readers reader=temporal_async_reader((unsigned)slot);
            [alloc[slot]reset];id<MTL4CommandBuffer>cb=[g_dev newCommandBuffer];[cb beginCommandBufferWithAllocator:alloc[slot]];
            CHECK(temporal_snap_encode_producer(cb,slot,native,rect,frame,1000000000ull+frame*16666667ull,world,ui,depth,camera,event,frame,rect)!=0,"gated history pair remains available");
            [cb endCommandBuffer];temporal_async_copy_wait(reader);[g_q4 commit:&cb count:1];[g_q4 signalEvent:event value:frame];
            if(frame==9)CHECK([event waitUntilSignaledValue:9 timeoutMS:10000],"first gated pair input is published");
        }
        CHECK(![event waitUntilSignaledValue:10 timeoutMS:20],"slot reuse waits for the unfinished auxiliary previous-color reader");
        CAMetalLayer *pending_layer=[CAMetalLayer layer];pending_layer.device=g_dev;
        pending_layer.pixelFormat=MTLPixelFormatBGRA8Unorm;pending_layer.framebufferOnly=NO;
        pending_layer.drawableSize=CGSizeMake(128,72);
        id<CAMetalDrawable>pending_drawable=[pending_layer nextDrawable];
        CHECK(pending_drawable!=nil,"pending generated result receives a drawable");
        CHECK(temporal_async_present(g_temporal_snap[0].serial,g_temporal_snap[0].display,0,9,9,rect,pending_drawable,0,1)==1,"generated display submits before its generation completes");
        uint64_t pending_epoch=atomic_load(&g_temporal_display_latest);
        CHECK(![g_temporal_display_done waitUntilSignaledValue:pending_epoch timeoutMS:20],"generated display waits for its own generation result");
        aux_gate.signaledValue=2;
        CHECK([event waitUntilSignaledValue:10 timeoutMS:10000],"releasing the gate lets both producer and consumer queues progress");
        temporal_async_drain();
        CHECK(generated_matches(pending_drawable.texture,36),"waiting generated display receives the completed midpoint and HUD");
        CHECK(generated_matches(g_temporal_snap[0].display,36),"previous-color slot stays intact until the older interpolation finishes");
        CHECK(generated_matches(g_temporal_snap[1].display,28),"next pair also retains its real moving midpoint after cross-queue reuse");
        g_pq_done=10;g_pq_inflight=0;
        g_snap[1].serial=(1ull<<61)+99;g_snap[1].tex=native;
        NSMutableArray *layers=[NSMutableArray array];
        for(int phase=0;phase<2;phase++) {
            CAMetalLayer *layer=[CAMetalLayer layer];layer.device=g_dev;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;layer.drawableSize=CGSizeMake(128,72);[layers addObject:layer];
            id<CAMetalDrawable> drawable=[layer nextDrawable];CHECK(drawable!=nil,"auxiliary presentation receives a real drawable");
            int slot=phase?1:0;uint64_t before=temporal_async_reader((unsigned)slot).presentation;
            CHECK(temporal_async_present(phase?g_snap[1].serial:g_temporal_snap[0].serial,phase?native:g_temporal_snap[0].display,0,phase?10:9,20,rect,drawable,0,!phase)==1,"snapshot is encoded and presented on the auxiliary lane");
            temporal_async_drain();
            pthread_mutex_lock(&g_pq_mu);struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
            while(g_pq_inflight && pthread_cond_timedwait(&g_pq_cv,&g_pq_mu,&deadline)!=ETIMEDOUT){}
            int inflight=g_pq_inflight;uint64_t done=g_pq_done;pthread_mutex_unlock(&g_pq_mu);
            CHECK(inflight==0,"auxiliary presentation GPU feedback completes");
            CHECK(done==(phase?20:10),"only the native auxiliary presentation completes the game ticket");
            CHECK(temporal_async_reader((unsigned)slot).presentation>before,"slot reader epoch includes the auxiliary display read");
            CHECK(generated_matches(drawable.texture,phase?24:36),"auxiliary drawable has the expected cropped colors, midpoint and HUD");
        }
        // Both old and resized spatial scalers must survive while their GPU work is pending.
        g_fx_mode=1;id<MTLCommandBuffer>resize_hold=[g_temporal_display_queue commandBuffer];[resize_hold encodeWaitForEvent:aux_gate value:3];[resize_hold commit];
        id<CAMetalDrawable> scaled[2];uintptr_t old_scaler=0;
        for(int i=0;i<2;i++){
            int scale=i+2;CAMetalLayer *layer=[CAMetalLayer layer];layer.device=g_dev;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;layer.drawableSize=CGSizeMake(128*scale,72*scale);[layers addObject:layer];scaled[i]=[layer nextDrawable];
            if(i)g_present_n+=FX_REMAKE_PRESENTS;
            CHECK(temporal_async_present(g_snap[1].serial,native,0,10,21+i,rect,scaled[i],0,0)==1,"auxiliary spatial scaling remains available across resize");
            CHECK(g_temporal_async_spatial.scaler!=nil && g_temporal_async_spatial.output.width==(NSUInteger)(128*scale),"the requested MetalFX spatial scaler is used");
            if(!i)old_scaler=(uintptr_t)(__bridge void*)g_temporal_async_spatial.scaler;
            else CHECK(old_scaler!=(uintptr_t)(__bridge void*)g_temporal_async_spatial.scaler,"resize creates a new context while the prior context remains in flight");
        }
        // Copy reuse must also wait for display reads, independently of FG.
        id<MTLSharedEvent>reuse_event=[g_dev newSharedEvent];
        temporal_async_copy_wait(temporal_async_reader(1));
        [g_q4 signalEvent:reuse_event value:1];
        CHECK(![reuse_event waitUntilSignaledValue:1 timeoutMS:20],"snapshot reuse waits for the gated presentation reader");
        aux_gate.signaledValue=3;temporal_async_drain();
        CHECK([reuse_event waitUntilSignaledValue:1 timeoutMS:10000],"snapshot reuse proceeds after presentation releases its input");
        pthread_mutex_lock(&g_pq_mu);struct timespec resize_deadline;clock_gettime(CLOCK_REALTIME,&resize_deadline);resize_deadline.tv_sec+=5;
        while(g_pq_inflight && pthread_cond_timedwait(&g_pq_cv,&g_pq_mu,&resize_deadline)!=ETIMEDOUT){}
        int remaining=g_pq_inflight;pthread_mutex_unlock(&g_pq_mu);CHECK(!remaining,"both resized auxiliary presentations finish");
        CHECK(scaled_native_matches(scaled[0].texture,2,32),"old spatial-scaled drawable preserves image and HUD after its context is replaced");
        CHECK(scaled_native_matches(scaled[1].texture,3,32),"new spatial-scaled drawable preserves image and HUD");
        CAMetalLayer *generated_layer=[CAMetalLayer layer];generated_layer.device=g_dev;generated_layer.pixelFormat=MTLPixelFormatBGRA8Unorm;generated_layer.framebufferOnly=NO;generated_layer.drawableSize=CGSizeMake(384,216);[layers addObject:generated_layer];
        id<CAMetalDrawable> generated_drawable=[generated_layer nextDrawable];
        CHECK(generated_drawable!=nil,"compact generated spatial result receives a drawable");
        CHECK(temporal_async_present(g_temporal_snap[0].serial,g_temporal_snap[0].display,0,9,23,rect,generated_drawable,0,1)==1,"compact generated picture passes through the real final spatial path");
        CHECK(g_temporal_async_spatial.scaler.colorTexture==g_temporal_snap[0].display,"physically exact generated input bypasses extraction without reading padding");
        temporal_async_drain();
        CHECK(scaled_native_matches(generated_drawable.texture,3,44),"direct compact spatial input preserves generated midpoint and HUD");
        g_fx_mode=0;
    }
    // Unsupported output must fall back before encoding, without corrupting source/history.
    [alloc[0]reset];id<MTL4CommandBuffer>cb=[g_dev newCommandBuffer];[cb beginCommandBufferWithAllocator:alloc[0]];
    uint64_t fallback=asynchronous?11:9;
    CHECK(!temporal_snap_encode_producer(cb,0,world,rect,fallback,1200000000ull,world,ui,depth,camera,asynchronous?event:nil,asynchronous?fallback:0,rect),"unsupported native output format falls back");
    [cb endCommandBuffer];[g_q4 commit:&cb count:1];[g_q4 signalEvent:event value:fallback];CHECK([event waitUntilSignaledValue:fallback timeoutMS:10000],"fallback command buffer completes");
    if(!asynchronous) {
        // Replacing a slot must not evict a generated image still read by an older Metal4 present.
        id<MTLTexture> old=g_temporal_snap[1].display,out=temporal_snap_texture(MTLPixelFormatBGRA8Unorm,128,72);
        id<MTLBuffer> uv=[g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];res_add(out);res_add(uv);res_commit();
        id<MTLSharedEvent> gate=[g_dev newSharedEvent],finished=[g_dev newSharedEvent];
        id<MTL4CommandAllocator> pa=[g_dev newCommandAllocator];id<MTL4CommandBuffer> present=[g_dev newCommandBuffer];[present beginCommandBufferWithAllocator:pa];
        CHECK(present_encode_m4(present,old,out,rect,uv,NULL)!=0,"old generated display is encoded before resize");[present endCommandBuffer];res_commit();
        [g_q4 waitForEvent:gate value:1];[g_q4 commit:&present count:1];[g_q4 signalEvent:finished value:1];
        id<MTLTexture> big_world=temporal_snap_texture(color,256,144),big_ui=temporal_snap_texture(color,256,144),big_depth=temporal_snap_texture(MTLPixelFormatDepth32Float,128,72),big_native=temporal_snap_texture(MTLPixelFormatRGBA8Unorm,320,180);
        res_add(big_world);res_add(big_ui);res_add(big_depth);res_add(big_native);res_commit();upload(big_world,48,0,0);upload(big_ui,48,test_ui_visible,0);upload(big_native,48,test_ui_visible,1);
        b=q_cb();MTLRenderPassDescriptor *big_rp=[MTLRenderPassDescriptor new];big_rp.depthAttachment.texture=big_depth;big_rp.depthAttachment.loadAction=MTLLoadActionClear;big_rp.depthAttachment.storeAction=MTLStoreActionStore;big_rp.depthAttachment.clearDepth=.95095095095;[[b renderCommandEncoderWithDescriptor:big_rp]endEncoding];[b commit];[b waitUntilCompleted];
        nvn_camera_input larger=camera;larger.width=128;larger.height=72;int big_rect[4]={0,0,256,144};
        [alloc[1]reset];id<MTL4CommandBuffer> resized=[g_dev newCommandBuffer];[resized beginCommandBufferWithAllocator:alloc[1]];
        CHECK(!temporal_snap_encode(resized,1,big_native,big_rect,10,1300000000ull,big_world,big_ui,big_depth,larger),"resize resets history before another generated picture is allowed");
        [resized endCommandBuffer];[g_q4 commit:&resized count:1];[g_q4 signalEvent:finished value:2];
        CHECK(g_temporal_snap[1].retired_keep!=nil,"replaced display allocations stay resident while earlier display work is gated");
        gate.signaledValue=1;CHECK([finished waitUntilSignaledValue:2 timeoutMS:10000],"old presentation and resized snapshot both finish");
        CHECK(generated_matches(out,44),"in-flight old generated display retains its original pixels across resize");
    }
    printf("t_temporal_snapshot: %d checks, %d failures\n",checks,failed);return failed!=0;
}}
