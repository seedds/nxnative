// Host-owned frame generation. Called only after this snapshot slot's GPU work and
// native presentation have completed. Its inputs, arguments and residency outlive
// the game's fence; none of these bytes come from the game's reusable frame arena.
#ifndef NVN_TEMPORAL_SNAPSHOT_INC
#define NVN_TEMPORAL_SNAPSHOT_INC
typedef struct {
    id<MTLResidencySet> residence; NSArray *keep,*retired_keep;
    NSArray *capture_buffers; NSDictionary *capture_info; NSString *capture_path;
    id<MTLTexture> world,ui,motion,output,display,srgb,control,control_srgb;
    id<MTLBuffer> motion_uniform,copy_uniform[3],native_check,converted_check;
    id<MTL4ArgumentTable> motion_args,copy_args[3];
    id<MTL4FXFrameInterpolator> used_interpolator;
    id<MTLFXFrameInterpolator> async_interpolator;id<MTLTexture> depth_copy;
    _Atomic uint64_t async_reader,async_ready,display_reader;
    id<MTL4CounterHeap> profile;uint64_t profile_frame;int profile_pending;
    uint64_t serial,check_generation;
    NSUInteger check_pitch,check_width,check_height;
} temporal_snap_slot;
static temporal_snap_slot g_temporal_snap[SNAP_MAX];
static id<MTL4FXFrameInterpolator> g_temporal_snap_interp;
static id<MTLTexture> g_temporal_snap_previous;
static nvn_camera_input g_temporal_snap_camera;
static uint64_t g_temporal_snap_frame=UINT64_MAX,g_temporal_snap_ns,g_temporal_snap_generation;
static unsigned g_temporal_snap_age;
static int g_temporal_snap_disabled,g_temporal_snap_fidelity; // 0 unknown,1 checking,2 passed,-1 failed
static NSUInteger g_temporal_snap_dimensions[6];
static int g_temporal_snap_rect[4];
static MTLPixelFormat g_temporal_snap_format,g_temporal_snap_color_format,g_temporal_snap_ui_format;
static id<MTLSamplerState> g_temporal_snap_sampler;
static NSMutableDictionary<NSNumber *,id<MTLRenderPipelineState>> *g_temporal_snap_copy_ps;
static id<MTLTexture> temporal_snap_texture(MTLPixelFormat format,NSUInteger w,NSUInteger h) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
    return [g_dev newTextureWithDescriptor:d];
}
static id<MTL4ArgumentTable> temporal_snap_table(int motion) {
    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];
    d.maxBufferBindCount=1;d.maxTextureBindCount=motion?2:1;d.maxSamplerStateBindCount=motion?0:1;
    return [g_dev newArgumentTableWithDescriptor:d error:nil];
}
static id<MTLRenderPipelineState> temporal_snap_copy_pipeline(MTLPixelFormat format) {
    if(!g_temporal_snap_copy_ps)g_temporal_snap_copy_ps=[NSMutableDictionary dictionary];
    id<MTLRenderPipelineState> p=g_temporal_snap_copy_ps[@(format)];if(p)return p;
    NSError *error=nil;id<MTLLibrary> lib=host_lib("copy",&error);
    MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=[lib newFunctionWithName:@"vs"];d.fragmentFunction=[lib newFunctionWithName:@"fs"];d.colorAttachments[0].pixelFormat=format;
    p=[g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    if(p)g_temporal_snap_copy_ps[@(format)]=p;
    else MLOG("[RED] frame generation copy pipeline: %s",error.localizedDescription.UTF8String);
    return p;
}
static int temporal_snap_copy(id<MTL4CommandBuffer> cb,temporal_snap_slot *s,int index,id<MTLTexture> src,id<MTLTexture> dst,const int rect[4]) {
    if(src!=dst && src.pixelFormat==dst.pixelFormat && src.width==dst.width && src.height==dst.height &&
       !rect[0] && !rect[1] && rect[2]==(int)dst.width && rect[3]==(int)dst.height) {
        id<MTL4ComputeCommandEncoder> e=[cb computeCommandEncoder];
        [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
        [e copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(src.width,src.height,1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [e barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];[e endEncoding];return 1;
    }
    id<MTLRenderPipelineState> pipeline=temporal_snap_copy_pipeline(dst.pixelFormat);if(!pipeline)return 0;
    if(!g_temporal_snap_sampler) {
        MTLSamplerDescriptor *d=[MTLSamplerDescriptor new];d.minFilter=d.magFilter=MTLSamplerMinMagFilterLinear;
        d.sAddressMode=d.tAddressMode=MTLSamplerAddressModeClampToEdge;d.supportArgumentBuffers=YES;
        g_temporal_snap_sampler=[g_dev newSamplerStateWithDescriptor:d];
    }
    float *v=s->copy_uniform[index].contents;
    v[0]=v[1]=0;v[2]=(float)src.width/rect[2];v[3]=(float)src.height/rect[3];
    v[4]=rect[0];v[5]=rect[1];v[6]=src.width;v[7]=src.height;
    id<MTL4ArgumentTable> table=s->copy_args[index];
    [table setTexture:src.gpuResourceID atIndex:0];[table setSamplerState:g_temporal_snap_sampler.gpuResourceID atIndex:0];[table setAddress:s->copy_uniform[index].gpuAddress atIndex:0];
    MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture=dst;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
    rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    id<MTL4RenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice|MTL4VisibilityOptionResourceAlias];
    [e setRenderPipelineState:pipeline];[e setArgumentTable:table atStages:MTLRenderStageFragment];
    [e setViewport:(MTLViewport){rect[0],rect[1],rect[2],rect[3],0,1}];[e setScissorRect:(MTLScissorRect){rect[0],rect[1],rect[2],rect[3]}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice|MTL4VisibilityOptionResourceAlias];[e endEncoding];
    return 1;
}
static NSMutableArray *g_temporal_capture_pending;
static void temporal_snap_capture_flush(void) {
    // A short sequence stays in memory until its inputs are captured: disk writes
    // must not interrupt the camera/object history we are trying to inspect.
    for(NSDictionary *capture in g_temporal_capture_pending) {
        NSArray *buffers=capture[@"buffers"],*entries=capture[@"info"][@"textures"];
        NSString *path=capture[@"path"];
        for(NSUInteger i=0;i<buffers.count;i++) {
            id<MTLBuffer> b=buffers[i];NSError *error=nil;
            NSData *bytes=[NSData dataWithBytesNoCopy:b.contents length:b.length freeWhenDone:NO];
            if(![bytes writeToFile:[path stringByAppendingPathComponent:entries[i][@"file"]] options:NSDataWritingAtomic error:&error])
                MLOG("[RED] snapshot capture: %s",error.localizedDescription.UTF8String);
        }
        NSData *json=[NSJSONSerialization dataWithJSONObject:capture[@"info"] options:NSJSONWritingPrettyPrinted error:nil];
        if(![json writeToFile:[path stringByAppendingPathComponent:@"capture.json"] atomically:YES])MLOG("[RED] snapshot capture metadata could not be written");
        MLOG("snapshot moving-scene capture saved to %s",path.UTF8String);
    }
    g_temporal_capture_pending=nil;
}
static void temporal_snap_check_completed(temporal_snap_slot *s) {
    if(s->capture_buffers) {
        if(!g_temporal_capture_pending)g_temporal_capture_pending=[NSMutableArray array];
        [g_temporal_capture_pending addObject:@{@"buffers":s->capture_buffers,@"info":s->capture_info,@"path":s->capture_path}];
        NSUInteger count=[s->capture_info[@"capture_count"] unsignedIntegerValue];
        s->capture_buffers=nil;s->capture_info=nil;s->capture_path=nil;
        if(count<=1 || g_temporal_capture_pending.count>=count)temporal_snap_capture_flush();
    }
    if(s->profile_pending) {
        NSData *data=[s->profile resolveCounterRange:NSMakeRange(0,6)];
        if(data.length>=6*sizeof(uint64_t)) {
            const uint64_t *ticks=data.bytes;double scale=1000.0/[g_dev queryTimestampFrequency];
            static FILE *file;static int opened;
            if(!opened){opened=1;file=fopen(getenv("DM_FRAMEGEN_STAGE_TIMES"),"w");if(file)fprintf(file,"frame\tworld_copy_ms\tui_copy_ms\tmotion_ms\tmetalfx_ms\tfinal_copy_ms\n");}
            if(file){fprintf(file,"%llu",(unsigned long long)s->profile_frame);for(int i=0;i<5;i++)fprintf(file,"\t%.6f",(ticks[i+1]-ticks[i])*scale);fprintf(file,"\n");fflush(file);}
        }
        s->profile_pending=0;
    }
    if(!s->native_check)return;
    if(s->check_generation==g_temporal_snap_generation && g_temporal_snap_fidelity==1) {
        const uint8_t *a=s->native_check.contents,*b=s->converted_check.contents;unsigned worst=0,lo=255,hi=0;
        for(NSUInteger y=0;y<s->check_height;y++)for(NSUInteger x=0;x<s->check_width;x++)for(int c=0;c<3;c++) {
            unsigned av=a[y*s->check_pitch+4*x+c],bv=b[y*s->check_pitch+4*x+c],d=av>bv?av-bv:bv-av;
            if(d>worst)worst=d;if(av<lo)lo=av;if(av>hi)hi=av;
        }
        // A black loading frame cannot establish the orientation/crop equivalence.
        g_temporal_snap_fidelity=hi-lo<16?0:worst<=1?2:-1;
        MLOG("%sframe generation final-copy control: RGB max error %u LSB, range %u, %s",g_temporal_snap_fidelity<0?"[RED] ":"",worst,hi-lo,g_temporal_snap_fidelity==2?"passed":g_temporal_snap_fidelity<0?"failed":"waiting for nonuniform content");
    }
    s->native_check=s->converted_check=nil;
    s->control=s->control_srgb=nil;
}
#include "nvn_temporal_async.inc"
static void temporal_snap_feedback(uint64_t frame,id<MTL4CommitFeedback> feedback) {
    const char *path=getenv("DM_FRAMEGEN_GPU_TIMES");if(!path || !*path)return;
    static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;static FILE *file;static int opened;
    pthread_mutex_lock(&mu);
    if(!opened){opened=1;file=fopen(path,"w");if(file)fprintf(file,"frame\tgpu_start_s\tgpu_end_s\tgpu_ms\n");}
    if(file){fprintf(file,"%llu\t%.9f\t%.9f\t%.6f\n",(unsigned long long)frame,feedback.GPUStartTime,feedback.GPUEndTime,(feedback.GPUEndTime-feedback.GPUStartTime)*1000);fflush(file);}
    pthread_mutex_unlock(&mu);
}
static uint64_t temporal_snap_encode_producer(id<MTL4CommandBuffer> cb,unsigned slot,id<MTLTexture> native,const int rect[4],uint64_t frame,uint64_t now,id<MTLTexture> world,id<MTLTexture> ui,id<MTLTexture> depth,nvn_camera_input camera,id<MTLSharedEvent> producer,uint64_t producer_value) {
    int asynchronous=temporal_async_on();
    if(asynchronous && (!producer || !producer_value))return 0;
    if(slot>=SNAP_MAX || g_temporal_snap_disabled || !world || !ui || !depth || !native)return 0;
    temporal_snap_slot *s=&g_temporal_snap[slot];temporal_snap_check_completed(s);
    if(g_temporal_snap_fidelity<0)return 0;
    MTLPixelFormat base=native.pixelFormat,srgb=MTLPixelFormatInvalid;
    if(base==MTLPixelFormatRGBA8Unorm || base==MTLPixelFormatRGBA8Unorm_sRGB){base=MTLPixelFormatRGBA8Unorm;srgb=MTLPixelFormatRGBA8Unorm_sRGB;}
    if(base==MTLPixelFormatBGRA8Unorm || base==MTLPixelFormatBGRA8Unorm_sRGB){base=MTLPixelFormatBGRA8Unorm;srgb=MTLPixelFormatBGRA8Unorm_sRGB;}
    if(!srgb || (world.pixelFormat!=MTLPixelFormatRGBA16Float && world.pixelFormat!=MTLPixelFormatRG11B10Float) || (ui.pixelFormat!=MTLPixelFormatRGBA16Float && ui.pixelFormat!=MTLPixelFormatRG11B10Float) || depth.pixelFormat!=MTLPixelFormatDepth32Float || depth.sampleCount!=1 || world.sampleCount!=1 || ui.sampleCount!=1 ||
       depth.width!=camera.width || depth.height!=camera.height || ui.width!=world.width || ui.height!=world.height ||
       rect[0]<0 || rect[1]<0 || rect[2]<=0 || rect[3]<=0 || (NSUInteger)(rect[0]+rect[2])>native.width || (NSUInteger)(rect[1]+rect[3])>native.height)return 0;
    NSUInteger dimensions[6]={world.width,world.height,depth.width,depth.height,native.width,native.height};
    int changed=world.pixelFormat!=g_temporal_snap_color_format || ui.pixelFormat!=g_temporal_snap_ui_format || memcmp(dimensions,g_temporal_snap_dimensions,sizeof dimensions)||base!=g_temporal_snap_format||memcmp(rect,g_temporal_snap_rect,sizeof g_temporal_snap_rect);
    if(changed || (asynchronous?!g_temporal_async_interp:!g_temporal_snap_interp)) {
        if(asynchronous) {
            g_temporal_async_interp=temporal_async_create(world.width,world.height,depth.width,depth.height,world.pixelFormat,ui.pixelFormat);
            if(!g_temporal_async_interp){g_temporal_snap_disabled=1;return 0;}
        } else {
            g_temporal_snap_interp=temporal_create_formats(world.width,world.height,depth.width,depth.height,world.pixelFormat,MTLPixelFormatRGBA16Float,ui.pixelFormat);
            if(!g_temporal_snap_interp){g_temporal_snap_disabled=1;return 0;}
        }
        memcpy(g_temporal_snap_dimensions,dimensions,sizeof dimensions);memcpy(g_temporal_snap_rect,rect,sizeof g_temporal_snap_rect);g_temporal_snap_format=base;g_temporal_snap_color_format=world.pixelFormat;g_temporal_snap_ui_format=ui.pixelFormat;
        g_temporal_snap_previous=nil;g_temporal_snap_frame=UINT64_MAX;g_temporal_snap_age=0;g_temporal_snap_fidelity=0;g_temporal_snap_generation++;
    }
    if(!s->residence) {
        MTLResidencySetDescriptor *d=[MTLResidencySetDescriptor new];d.initialCapacity=24;
        s->residence=[g_dev newResidencySetWithDescriptor:d error:nil];[g_q4 addResidencySet:s->residence];
        s->motion_uniform=[g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared];s->motion_args=temporal_snap_table(1);
        for(int i=0;i<3;i++){s->copy_uniform[i]=[g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];s->copy_args[i]=temporal_snap_table(0);}
        if(!s->residence || !s->motion_uniform || !s->motion_args){g_temporal_snap_disabled=1;return 0;}
        for(int i=0;i<3;i++)if(!s->copy_uniform[i] || !s->copy_args[i]){g_temporal_snap_disabled=1;return 0;}
        if(!s->serial)s->serial=(1ull<<60)+slot+1;
    }
    int resized=(asynchronous && !s->depth_copy) || !s->world || s->world.pixelFormat!=world.pixelFormat || s->ui.pixelFormat!=ui.pixelFormat || s->world.width!=world.width || s->world.height!=world.height || s->motion.width!=depth.width || s->motion.height!=depth.height ||
                s->display.width!=native.width || s->display.height!=native.height || s->display.pixelFormat!=base;
    // Old display allocations may still be read by an already submitted present.
    // Keep their residency through this copy's queue position; the next safe reuse
    // proves that those earlier reads have completed, including a resize/grab.
    s->retired_keep=resized?s->keep:nil;
    if(resized) {
        s->world=temporal_snap_texture(world.pixelFormat,world.width,world.height);s->ui=temporal_snap_texture(ui.pixelFormat,world.width,world.height);
        s->depth_copy=asynchronous?temporal_snap_texture(MTLPixelFormatDepth32Float,depth.width,depth.height):nil;
        s->output=temporal_snap_texture(MTLPixelFormatRGBA16Float,world.width,world.height);s->motion=temporal_snap_texture(MTLPixelFormatRG16Float,depth.width,depth.height);
        s->display=temporal_snap_texture(base,native.width,native.height);s->srgb=[s->display newTextureViewWithPixelFormat:srgb];
        if((asynchronous && !s->depth_copy) || !s->world || !s->ui || !s->output || !s->motion || !s->display || !s->srgb){g_temporal_snap_disabled=1;return 0;}
        serial_put(s->serial,s->display);
    }
    int matrices=camera.has_current_transform&&g_temporal_snap_camera.has_current_transform;
    int continuous=matrices?nvn_camera_previous_matches(&camera,&g_temporal_snap_camera):frame==g_temporal_snap_frame+1;
    int reset=!g_temporal_snap_previous || g_temporal_snap_previous==s->world || !continuous || now<=g_temporal_snap_ns || frame<=g_temporal_snap_frame;
    double decision_at=nvn_flow_begin();
    if(decision_at)nvn_flow_event(reset?(!continuous?"FG reset continuity":g_temporal_snap_previous==s->world?"FG reset same slot":"FG reset initial or clock"):"FG continuous",frame,0,decision_at,nvn_flow_clock());
    id<MTLTexture> previous=reset?s->world:g_temporal_snap_previous;
    s->used_interpolator=g_temporal_snap_interp;s->async_interpolator=g_temporal_async_interp;
    int check=g_temporal_snap_fidelity==0;
    if(check) {
        s->control=temporal_snap_texture(base,native.width,native.height);
        s->control_srgb=[s->control newTextureViewWithPixelFormat:srgb];
        if(!s->control || !s->control_srgb){g_temporal_snap_disabled=1;return 0;}
        s->check_width=rect[2];s->check_height=rect[3];s->check_pitch=(s->check_width*4+255)&~255ul;
        s->native_check=[g_dev newBufferWithLength:s->check_pitch*s->check_height options:MTLResourceStorageModeShared];
        s->converted_check=[g_dev newBufferWithLength:s->check_pitch*s->check_height options:MTLResourceStorageModeShared];s->check_generation=g_temporal_snap_generation;
        if(!s->native_check || !s->converted_check){g_temporal_snap_disabled=1;return 0;}
        g_temporal_snap_fidelity=1;
    }
    [s->residence removeAllAllocations];
    NSArray *allocations=@[world,ui,depth,native,previous,s->world,s->ui,s->output,s->motion,s->display,s->motion_uniform,s->copy_uniform[0],s->copy_uniform[1],s->copy_uniform[2]];
    if(asynchronous)allocations=[allocations arrayByAddingObject:s->depth_copy];
    s->keep=check?[allocations arrayByAddingObjectsFromArray:@[s->control,s->native_check,s->converted_check]]:allocations;
    for(id a in s->keep)[s->residence addAllocation:a];
    for(id a in s->retired_keep)[s->residence addAllocation:a];
    if(check){[s->residence addAllocation:s->native_check];[s->residence addAllocation:s->converted_check];}
    [s->residence commit];
    const char *stage_path=getenv("DM_FRAMEGEN_STAGE_TIMES"),*every_text=getenv("DM_FRAMEGEN_PROFILE_EVERY");
    unsigned every=every_text?(unsigned)MAX(1,atoi(every_text)):1;
    int profile_active=!asynchronous && stage_path && *stage_path && frame%every==0;
    if(profile_active && !s->profile) {
        MTL4CounterHeapDescriptor *d=[MTL4CounterHeapDescriptor new];d.type=MTL4CounterHeapTypeTimestamp;d.count=6;
        s->profile=[g_dev newCounterHeapWithDescriptor:d error:nil];
    }
    id<MTL4CounterHeap> profile=profile_active?s->profile:nil;
    if(profile){[profile invalidateCounterRange:NSMakeRange(0,6)];[cb writeTimestampIntoHeap:profile atIndex:0];s->profile_frame=frame;s->profile_pending=1;}
    id<MTL4ComputeCommandEncoder> e=[cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    [e copyFromTexture:world sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(world.width,world.height,1) toTexture:s->world destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    if(asynchronous)[e copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(depth.width,depth.height,1) toTexture:s->depth_copy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [e barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];[e endEncoding];
    if(profile)[cb writeTimestampIntoHeap:profile atIndex:1];
    int full[4]={0,0,(int)ui.width,(int)ui.height};
    if(!temporal_snap_copy(cb,s,0,ui,s->ui,full))return 0;
    if(profile)[cb writeTimestampIntoHeap:profile atIndex:2];
    float delta_time=reset?1.f/60:(now-g_temporal_snap_ns)*1e-9f;
    if(!asynchronous) {
        memcpy(s->motion_uniform.contents,camera.clip_to_world,64);memcpy((uint8_t*)s->motion_uniform.contents+64,camera.previous_world_to_clip,64);
        [s->motion_args setAddress:s->motion_uniform.gpuAddress atIndex:0];[s->motion_args setTexture:depth.gpuResourceID atIndex:0];[s->motion_args setTexture:s->motion.gpuResourceID atIndex:1];
        temporal_encode_fx(cb,s->used_interpolator,s->motion_args,g_temporal_motion_ps,depth.width,depth.height,camera,delta_time,reset,s->world,previous,depth,s->motion,s->output,s->ui,profile);
        if(!temporal_snap_copy(cb,s,1,s->output,s->srgb,rect))return 0;
        if(profile)[cb writeTimestampIntoHeap:profile atIndex:5];
    }
    if(check) {
        temporal_snap_copy(cb,s,2,s->ui,s->control_srgb,rect);
        e=[cb computeCommandEncoder];[e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice|MTL4VisibilityOptionResourceAlias];
        id<MTLTexture> textures[2]={native,s->control};id<MTLBuffer> buffers[2]={s->native_check,s->converted_check};
        for(int i=0;i<2;i++)[e copyFromTexture:textures[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(rect[0],rect[1],0) sourceSize:MTLSizeMake(rect[2],rect[3],1) toBuffer:buffers[i] destinationOffset:0 destinationBytesPerRow:s->check_pitch destinationBytesPerImage:s->check_pitch*s->check_height];
        [e barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];[e endEncoding];
    }
    g_temporal_snap_age=reset?1:g_temporal_snap_age+1;g_temporal_snap_previous=s->world;g_temporal_snap_camera=camera;g_temporal_snap_frame=frame;g_temporal_snap_ns=now;
    if(asynchronous && !temporal_async_submit(s,slot,previous,g_snap[slot].tex?:native,producer,producer_value,camera,frame,delta_time,reset,rect)){g_temporal_snap_disabled=1;return 0;}

    // Optional readback belongs to this slot, not the game's earlier completion fence.
    // Read it only at safe slot reuse, after the snapshot command buffer has finished.
    static int captured;
    const char *count_text=getenv("DM_OOT_TEMPORAL_CAPTURE_FRAMES");
    unsigned capture_count=count_text?(unsigned)MAX(1,MIN(16,atoi(count_text))):1;
    const char *capture=getenv("DM_OOT_TEMPORAL_CAPTURE");
    const char *age_text=getenv("DM_OOT_TEMPORAL_CAPTURE_MIN_HISTORY");
    unsigned min_age=age_text?(unsigned)MAX(1,atoi(age_text)):8;
    const char *wait=getenv("DM_OOT_TEMPORAL_CAPTURE_WAIT_FOR_ARM");
    NSString *path=capture&&*capture?@(capture):nil;
    int armed=!(wait&&atoi(wait)) || (path&&[[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"ARM"]]);
    if(!asynchronous && path && captured<capture_count && armed && !reset && g_temporal_snap_age>=min_age) {
        id<MTLTexture> textures[]={s->world,previous,depth,s->motion,s->output,s->ui,native,s->display};
        NSString *names[]={world.pixelFormat==MTLPixelFormatRGBA16Float?@"current.rgba16f":@"current.rgb11f",world.pixelFormat==MTLPixelFormatRGBA16Float?@"previous.rgba16f":@"previous.rgb11f",@"depth.f32",@"motion.rg16f",@"generated.rgba16f",ui.pixelFormat==MTLPixelFormatRGBA16Float?@"ui.rgba16f":@"ui.rgb11f",@"native.rgba8",@"display.rgba8"};
        int bpp[]={world.pixelFormat==MTLPixelFormatRGBA16Float?8:4,world.pixelFormat==MTLPixelFormatRGBA16Float?8:4,4,4,8,ui.pixelFormat==MTLPixelFormatRGBA16Float?8:4,4,4};
        NSMutableArray *buffers=[NSMutableArray array],*metadata=[NSMutableArray array];
        for(int i=0;i<8;i++) {
            id<MTLTexture> t=textures[i];NSUInteger pitch=(t.width*bpp[i]+255)&~255ul;
            id<MTLBuffer> b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];
            if(!b)break;
            [buffers addObject:b];[s->residence addAllocation:b];
            [metadata addObject:@{@"file":names[i],@"width":@(t.width),@"height":@(t.height),@"row_bytes":@(pitch),@"pixel_format":@(t.pixelFormat)}];
        }
        if(buffers.count==8) {
            [s->residence commit];e=[cb computeCommandEncoder];
            [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
            for(int i=0;i<8;i++) {
                id<MTLTexture> t=textures[i];id<MTLBuffer> b=buffers[i];NSUInteger pitch=[metadata[i][@"row_bytes"] unsignedIntegerValue];
                [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:b.length];
            }
            [e barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];[e endEncoding];
            NSMutableArray *inverse=[NSMutableArray array],*prev=[NSMutableArray array],*current=[NSMutableArray array];
            for(int i=0;i<16;i++){[inverse addObject:@(camera.clip_to_world[i])];[prev addObject:@(camera.previous_world_to_clip[i])];[current addObject:@(camera.world_to_clip[i])];}
            NSString *destination=capture_count>1?[path stringByAppendingPathComponent:[NSString stringWithFormat:@"frame-%010llu",(unsigned long long)frame]]:path;
            NSError *directory_error=nil;
            if(![[NSFileManager defaultManager] createDirectoryAtPath:destination withIntermediateDirectories:YES attributes:nil error:&directory_error])
                MLOG("[RED] snapshot capture directory: %s",directory_error.localizedDescription.UTF8String);
            s->capture_buffers=buffers;s->capture_path=destination;
            NSMutableArray *view=[NSMutableArray array],*projection=[NSMutableArray array];
            for(int i=0;i<16;i++){[view addObject:@(camera.world_to_view[i])];[projection addObject:@(camera.view_to_clip[i])];}
            s->capture_info=@{@"frame":@(frame),@"capture_count":@(capture_count),@"delta_time":@(delta_time),@"near":@(camera.near_plane),@"far":@(camera.far_plane),@"fov_radians":@(camera.fov),@"reset":@(reset),@"history_age":@(g_temporal_snap_age),@"textures":metadata,@"clip_to_world":inverse,@"previous_world_to_clip":prev,@"world_to_clip":current,@"has_render_matrices":@(camera.has_render_matrices),@"world_to_view":view,@"view_to_clip":projection,@"ready":@(g_temporal_snap_fidelity==2)};
            captured++;
            pose_probe_temporal_capture(frame,(unsigned)depth.width,(unsigned)depth.height,captured==capture_count);
        }
    }
    int ready=!reset && g_temporal_snap_age>=4 && g_temporal_snap_fidelity==2;
    static int notes;if(notes++<4 || (ready&&notes==5))MLOG("snapshot frame generation frame %llu: history %u reset=%d final-copy=%d ready=%d",(unsigned long long)frame,g_temporal_snap_age,reset,g_temporal_snap_fidelity,ready);
    if(decision_at)nvn_flow_event(ready?"FG ready":"FG warming",frame,0,decision_at,nvn_flow_clock());
    return ready?s->serial:0;
}
static uint64_t temporal_snap_encode(id<MTL4CommandBuffer> cb,unsigned slot,id<MTLTexture> native,const int rect[4],uint64_t frame,uint64_t now,id<MTLTexture> world,id<MTLTexture> ui,id<MTLTexture> depth,nvn_camera_input camera) {
    return temporal_snap_encode_producer(cb,slot,native,rect,frame,now,world,ui,depth,camera,nil,0);
}
#endif
