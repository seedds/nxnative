// Independent generation and display lanes: a future producer wait must not
// hold an older ready display. Slot reuse waits for both reader timelines.
static id<MTLCommandQueue> g_temporal_async_queue,g_temporal_display_queue;
static id<MTLSharedEvent> g_temporal_async_done,g_temporal_display_done;
static id<MTLFXFrameInterpolator> g_temporal_async_interp;
static pthread_mutex_t g_temporal_async_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_temporal_async_sequence,g_temporal_display_sequence;
static _Atomic uint64_t g_temporal_async_latest,g_temporal_display_latest;
static int temporal_async_init(void) {
    pthread_mutex_lock(&g_temporal_async_mu);
    if(!g_temporal_async_queue) {
        g_temporal_async_queue=[g_dev newCommandQueue];
        g_temporal_async_done=[g_dev newSharedEvent];
        g_temporal_display_queue=[g_dev newCommandQueue];
        g_temporal_display_done=[g_dev newSharedEvent];
        g_temporal_async_queue.label=@"frame generation";
        g_temporal_display_queue.label=@"snapshot display";
    }
    int ok=g_temporal_async_queue && g_temporal_async_done && g_temporal_display_queue && g_temporal_display_done;
    pthread_mutex_unlock(&g_temporal_async_mu);return ok;
}
static int temporal_async_slot(uint64_t serial) {
    for(int i=0;i<SNAP_MAX;i++)if(serial && (g_snap[i].serial==serial || g_temporal_snap[i].serial==serial))return i;
    return -1;
}
typedef struct { uint64_t generation,presentation; } temporal_async_readers;
static temporal_async_readers temporal_async_reader(unsigned slot) {
    if(!temporal_async_on() || slot>=SNAP_MAX)return (temporal_async_readers){0};
    return (temporal_async_readers){atomic_load(&g_temporal_snap[slot].async_reader),atomic_load(&g_temporal_snap[slot].display_reader)};
}
static void temporal_async_copy_wait(temporal_async_readers readers) {
    if(readers.generation)[g_q4 waitForEvent:g_temporal_async_done value:readers.generation];
    if(readers.presentation)[g_q4 waitForEvent:g_temporal_display_done value:readers.presentation];
}
static void temporal_async_drain(void) {
    if(!temporal_async_on())return;
    pthread_mutex_lock(&g_temporal_async_mu);
    uint64_t generation=atomic_load(&g_temporal_async_latest),display=atomic_load(&g_temporal_display_latest);
    id<MTLSharedEvent> generation_event=g_temporal_async_done,display_event=g_temporal_display_done;
    pthread_mutex_unlock(&g_temporal_async_mu);
    if(generation && ![generation_event waitUntilSignaledValue:generation timeoutMS:10000])MDIE("frame generation: GPU did not drain within 10 s");
    if(display && ![display_event waitUntilSignaledValue:display timeoutMS:10000])MDIE("snapshot display: GPU did not drain within 10 s");
}
static void temporal_async_wait_source(uint64_t serial) {
    if(!temporal_async_on())return;int slot=temporal_async_slot(serial);if(slot<0)return;
    if(g_snap[slot].serial==serial) {
        uint64_t v=atomic_load(&g_snap[slot].ev);
        if(v && ![g_snap_ev waitUntilSignaledValue:v timeoutMS:10000])MDIE("snapshot diagnostic: producer did not complete");
    } else {
        uint64_t v=atomic_load(&g_temporal_snap[slot].async_ready);
        if(v && ![g_temporal_async_done waitUntilSignaledValue:v timeoutMS:10000])MDIE("snapshot diagnostic: auxiliary producer did not complete");
    }
}
// Buffer diagnostic records: per-image disk flushes on completion/presented
// callbacks can disturb cadence. Graphics drain flushes these before graceful _exit.
static pthread_mutex_t g_framegen_async_gpu_mu=PTHREAD_MUTEX_INITIALIZER;static FILE *g_framegen_async_gpu_file;
static void temporal_async_note_gpu(uint64_t frame,const char *kind,double begin,double end) {
    const char *path=getenv("DM_FRAMEGEN_ASYNC_GPU_TIMES");if(!path || !*path)return;
    static char trace_buffer[1u<<20];static int opened;
    pthread_mutex_lock(&g_framegen_async_gpu_mu);
    if(!opened){opened=1;g_framegen_async_gpu_file=fopen(path,"w");if(g_framegen_async_gpu_file){setvbuf(g_framegen_async_gpu_file,trace_buffer,_IOFBF,sizeof trace_buffer);fprintf(g_framegen_async_gpu_file,"frame\tkind\tgpu_start_s\tgpu_end_s\tgpu_ms\n");}}
    if(g_framegen_async_gpu_file){fprintf(g_framegen_async_gpu_file,"%llu\t%s\t%.9f\t%.9f\t%.6f\n",(unsigned long long)frame,kind,begin,end,(end-begin)*1000);}
    pthread_mutex_unlock(&g_framegen_async_gpu_mu);
}
static id<MTLFXFrameInterpolator> temporal_async_create(NSUInteger w,NSUInteger h,NSUInteger iw,NSUInteger ih,MTLPixelFormat color,MTLPixelFormat ui) {
    if(!temporal_async_init() || !temporal_motion_pipeline())return nil;
    MTLFXFrameInterpolatorDescriptor *d=[MTLFXFrameInterpolatorDescriptor new];
    d.inputWidth=iw;d.inputHeight=ih;d.outputWidth=w;d.outputHeight=h;
    d.colorTextureFormat=color;d.uiTextureFormat=ui;d.outputTextureFormat=MTLPixelFormatRGBA16Float;
    d.depthTextureFormat=MTLPixelFormatDepth32Float;d.motionTextureFormat=MTLPixelFormatRG16Float;
    return [d newFrameInterpolatorWithDevice:g_dev];
}
static int temporal_async_copy(id<MTLCommandBuffer> cb,id<MTLTexture> src,id<MTLTexture> dst,const int rect[4]) {
    id<MTLRenderPipelineState> ps=temporal_snap_copy_pipeline(dst.pixelFormat);if(!ps)return 0;
    if(!g_temporal_snap_sampler) {
        MTLSamplerDescriptor *d=[MTLSamplerDescriptor new];d.minFilter=d.magFilter=MTLSamplerMinMagFilterLinear;
        d.sAddressMode=d.tAddressMode=MTLSamplerAddressModeClampToEdge;d.supportArgumentBuffers=YES;
        g_temporal_snap_sampler=[g_dev newSamplerStateWithDescriptor:d];
    }
    MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor new];rp.colorAttachments[0].texture=dst;
    rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];if(!e)return 0;
    float v[8]={0,0,(float)src.width/rect[2],(float)src.height/rect[3],rect[0],rect[1],src.width,src.height};
    [e setRenderPipelineState:ps];[e setFragmentTexture:src atIndex:0];[e setFragmentSamplerState:g_temporal_snap_sampler atIndex:0];[e setFragmentBytes:v length:sizeof v atIndex:0];
    [e setViewport:(MTLViewport){rect[0],rect[1],rect[2],rect[3],0,1}];[e setScissorRect:(MTLScissorRect){rect[0],rect[1],rect[2],rect[3]}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];return 1;
}
static void temporal_async_capture(id<MTLCommandBuffer> cb,temporal_snap_slot *s,id<MTLTexture> previous,id<MTLTexture> native,nvn_camera_input camera,uint64_t frame,int reset) {
    static int captured;const char *path=getenv("DM_OOT_TEMPORAL_CAPTURE");if(captured || !path || !*path || reset)return;
    const char *age=getenv("DM_OOT_TEMPORAL_CAPTURE_MIN_HISTORY");if(g_temporal_snap_age<(unsigned)(age?MAX(1,atoi(age)):8))return;
    NSString *prefix=@(path);const char *wait=getenv("DM_OOT_TEMPORAL_CAPTURE_WAIT_FOR_ARM");
    if(wait && atoi(wait) && ![[NSFileManager defaultManager]fileExistsAtPath:[prefix stringByAppendingPathComponent:@"ARM"]])return;
    id<MTLTexture> textures[]={s->world,previous,s->depth_copy,s->motion,s->output,s->ui,native,s->display};
    NSString *names[]={s->world.pixelFormat==MTLPixelFormatRGBA16Float?@"current.rgba16f":@"current.rgb11f",s->world.pixelFormat==MTLPixelFormatRGBA16Float?@"previous.rgba16f":@"previous.rgb11f",@"depth.f32",@"motion.rg16f",@"generated.rgba16f",s->ui.pixelFormat==MTLPixelFormatRGBA16Float?@"ui.rgba16f":@"ui.rgb11f",native.pixelFormat==MTLPixelFormatRGBA16Float?@"native.rgba16f":@"native.rgba8",s->display.pixelFormat==MTLPixelFormatRGBA16Float?@"display.rgba16f":@"display.rgba8"};
    int bytes[]={s->world.pixelFormat==MTLPixelFormatRGBA16Float?8:4,s->world.pixelFormat==MTLPixelFormatRGBA16Float?8:4,4,4,8,s->ui.pixelFormat==MTLPixelFormatRGBA16Float?8:4,native.pixelFormat==MTLPixelFormatRGBA16Float?8:4,s->display.pixelFormat==MTLPixelFormatRGBA16Float?8:4};
    NSMutableArray *buffers=[NSMutableArray array],*metadata=[NSMutableArray array];
    for(int i=0;i<8;i++) {
        id<MTLTexture> t=textures[i];if(!t)return;NSUInteger pitch=(t.width*bytes[i]+255)&~255ul;
        id<MTLBuffer> b=[g_dev newBufferWithLength:pitch*t.height options:MTLResourceStorageModeShared];if(!b)return;
        [buffers addObject:b];[metadata addObject:@{@"file":names[i],@"width":@(t.width),@"height":@(t.height),@"row_bytes":@(pitch),@"pixel_format":@(t.pixelFormat)}];
    }
    id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
    for(int i=0;i<8;i++){id<MTLTexture>t=textures[i];id<MTLBuffer>b=buffers[i];NSUInteger pitch=[metadata[i][@"row_bytes"]unsignedIntegerValue];
        [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:b.length];}
    [e endEncoding];
    NSMutableArray *inverse=[NSMutableArray array],*prior=[NSMutableArray array],*current=[NSMutableArray array];
    for(int i=0;i<16;i++){[inverse addObject:@(camera.clip_to_world[i])];[prior addObject:@(camera.previous_world_to_clip[i])];[current addObject:@(camera.world_to_clip[i])];}
    NSMutableArray *view=[NSMutableArray array],*projection=[NSMutableArray array];
    for(int i=0;i<16;i++){[view addObject:@(camera.world_to_view[i])];[projection addObject:@(camera.view_to_clip[i])];}
    NSDictionary *info=@{@"output_alias":@(s->output_alias),@"hdr_linear":@(s->hdr),@"backend":@"asynchronous Metal3",@"frame":@(frame),@"reset":@(reset),@"history_age":@(g_temporal_snap_age),@"textures":metadata,@"clip_to_world":inverse,@"previous_world_to_clip":prior,@"world_to_clip":current,@"has_render_matrices":@(camera.has_render_matrices),@"world_to_view":view,@"view_to_clip":projection,@"near":@(camera.near_plane),@"far":@(camera.far_plane),@"fov_radians":@(camera.fov)};
    captured=1;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        if(finished.error)return;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
            for(NSUInteger i=0;i<buffers.count;i++){id<MTLBuffer>b=buffers[i];NSError *error=nil;
                NSData *data=[NSData dataWithBytesNoCopy:b.contents length:b.length freeWhenDone:NO];
                if(![data writeToFile:[prefix stringByAppendingPathComponent:metadata[i][@"file"]] options:NSDataWritingAtomic error:&error])MLOG("[RED] auxiliary capture: %s",error.localizedDescription.UTF8String);}
            NSData *json=[NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingPrettyPrinted error:nil];
            [json writeToFile:[prefix stringByAppendingPathComponent:@"capture.json"]atomically:YES];
            MLOG("asynchronous moving-scene capture saved to %s",prefix.UTF8String);
        });
    }];
}
static int temporal_async_submit(temporal_snap_slot *s,unsigned slot,id<MTLTexture> previous,id<MTLTexture> native,id<MTLSharedEvent> producer,uint64_t producer_value,nvn_camera_input camera,uint64_t frame,float dt,int reset,const int rect[4]) {
    if(!producer || !producer_value || !s->async_interpolator || !temporal_async_init())return 0;
    pthread_mutex_lock(&g_temporal_async_mu);
    id<MTLCommandBuffer> cb=[g_temporal_async_queue commandBuffer];if(!cb){pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    [cb encodeWaitForEvent:producer value:producer_value];
    float matrices[16];nvn_cam_mul(camera.previous_world_to_clip,camera.clip_to_world,matrices);
    id<MTLComputeCommandEncoder> e=[cb computeCommandEncoder];[e setComputePipelineState:g_temporal_motion_ps];
    [e setBytes:matrices length:sizeof matrices atIndex:0];[e setTexture:s->depth_copy atIndex:0];[e setTexture:s->motion atIndex:1];
    [e dispatchThreadgroups:MTLSizeMake((s->depth_copy.width+7)/8,(s->depth_copy.height+7)/8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[e endEncoding];
    id<MTLFXFrameInterpolator> f=s->async_interpolator;
    f.colorTexture=s->world;f.prevColorTexture=previous;f.depthTexture=s->depth_copy;f.motionTexture=s->motion;f.outputTexture=s->output;f.uiTexture=s->ui;f.uiTextureComposited=YES;
    f.motionVectorScaleX=f.motionVectorScaleY=1;f.nearPlane=camera.near_plane;f.farPlane=camera.far_plane;f.fieldOfView=camera.fov*180.f/(float)M_PI;f.aspectRatio=(float)s->depth_copy.width/s->depth_copy.height;
    f.deltaTime=dt;f.shouldResetHistory=reset;f.depthReversed=NO;
    if(@available(macOS 27.0,iOS 27.0,tvOS 27.1,*)){f.worldToViewMatrix=camera.has_render_matrices?temporal_camera_matrix(camera.world_to_view):matrix_identity_float4x4;
        f.viewToClipMatrix=camera.has_render_matrices?temporal_camera_matrix(camera.view_to_clip):matrix_identity_float4x4;}
    [f encodeToCommandBuffer:cb];
    if(s->output!=s->srgb && !temporal_async_copy(cb,s->output,s->srgb,rect)){pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    temporal_async_capture(cb,s,previous,native,camera,frame,reset);
    uint64_t value=++g_temporal_async_sequence;
    atomic_store(&s->async_reader,value);atomic_store(&s->async_ready,value);
    for(unsigned i=0;i<SNAP_MAX;i++)if(g_temporal_snap[i].world==previous)atomic_store(&g_temporal_snap[i].async_reader,value);
    [cb encodeSignalEvent:g_temporal_async_done value:value];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        (void)[f class]; // retain this context through resize and GPU completion
        if(finished.error){MLOG("[RED] asynchronous frame-generation GPU error: %s",finished.error.description.UTF8String);gpu_lost_check(finished.error,"asynchronous frame generation");}
        temporal_async_note_gpu(frame,"generation",finished.GPUStartTime,finished.GPUEndTime);
    }];
    [cb commit];atomic_store(&g_temporal_async_latest,value);
    pthread_mutex_unlock(&g_temporal_async_mu);(void)slot;return 1;
}
typedef struct { id<MTLFXSpatialScaler> scaler;id<MTLTexture> input,output;uint64_t made_at;NSUInteger refused[7];int hdr; } temporal_async_spatial;
static temporal_async_spatial g_temporal_async_spatial;
// Writable drawables can receive the scaler directly when the final quad is an
// identity copy. Keep the intermediate path for letterboxing, origin flips and
// destinations without the scaler's required usage. The flag is opt-in.
static int temporal_async_direct_on(void) {
    static int on=-1;
    if(on<0){const char *e=getenv("DM_PRESENT_UPSCALE_DIRECT");on=e && !strcmp(e,"1");}
    return on;
}
static id<MTLTexture> temporal_async_upscale(id<MTLCommandBuffer> cb,id<MTLTexture> source,const int crop[4],id<MTLTexture> destination,NSArray *__strong *keep) {
    NSUInteger r[4];if(fx_mode()!=1 || !present_crop_rect(source,crop,r))return nil;
    MTLViewport vp=present_fit(source,destination.width,destination.height);if(!fx_wanted(vp,r[2],r[3]))return nil;
    temporal_async_spatial *s=&g_temporal_async_spatial;id<MTLTexture> raw=present_raw_src(source);
    int hdr=front_hdr_enabled() && raw.pixelFormat==MTLPixelFormatRGBA16Float && destination.pixelFormat==MTLPixelFormatRGBA16Float;
    NSUInteger key[7]={r[2],r[3],raw.pixelFormat,(NSUInteger)vp.width,(NSUInteger)vp.height,destination.pixelFormat,(NSUInteger)hdr};
    if(!s->scaler || s->hdr!=hdr || s->input.width!=key[0] || s->input.height!=key[1] || s->input.pixelFormat!=key[2] || s->output.width!=key[3] || s->output.height!=key[4] || s->output.pixelFormat!=key[5]) {
        if(!memcmp(key,s->refused,sizeof key) || (s->scaler && g_present_n<s->made_at+FX_REMAKE_PRESENTS))return nil;
        MTLFXSpatialScalerDescriptor *d=[MTLFXSpatialScalerDescriptor new];d.inputWidth=key[0];d.inputHeight=key[1];d.colorTextureFormat=raw.pixelFormat;d.outputWidth=key[3];d.outputHeight=key[4];d.outputTextureFormat=destination.pixelFormat;d.colorProcessingMode=hdr?MTLFXSpatialScalerColorProcessingModeHDR:MTLFXSpatialScalerColorProcessingModePerceptual;
        id<MTLFXSpatialScaler> f=[d newSpatialScalerWithDevice:g_dev];if(!f){memcpy(s->refused,key,sizeof key);return nil;}
        MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:raw.pixelFormat width:key[0] height:key[1] mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=f.colorTextureUsage|MTLTextureUsageShaderRead;
        id<MTLTexture> in=[g_dev newTextureWithDescriptor:td];td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:destination.pixelFormat width:key[3] height:key[4] mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=f.outputTextureUsage|MTLTextureUsageShaderRead;
        id<MTLTexture> out=[g_dev newTextureWithDescriptor:td];if(!in || !out)return nil;
        s->scaler=f;s->input=in;s->output=out;s->made_at=g_present_n;s->hdr=hdr;
    }
    // Exact physical bounds are required: inputContent alone does not clamp padded texels.
    int exact=!r[0] && !r[1] && r[2]==raw.width && r[3]==raw.height &&
        raw.textureType==MTLTextureType2D && raw.sampleCount==1 &&
        (raw.usage & s->scaler.colorTextureUsage)==s->scaler.colorTextureUsage;
    id<MTLTexture> input=exact?raw:s->input;
    if(!exact){
        id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];[e copyFromTexture:raw sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(r[0],r[1],0) sourceSize:MTLSizeMake(r[2],r[3],1) toTexture:input destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[e endEncoding];
    }
    int direct=temporal_async_direct_on() && !atomic_load_explicit(&g_origin_ll,memory_order_relaxed) &&
        vp.originX==0 && vp.originY==0 && vp.width==destination.width && vp.height==destination.height &&
        s->output.width==destination.width && s->output.height==destination.height && s->output.pixelFormat==destination.pixelFormat &&
        !destination.framebufferOnly && (destination.usage & s->scaler.outputTextureUsage)==s->scaler.outputTextureUsage;
    // MetalFX requires private output storage. IOSurface-backed drawables can
    // be managed/shared even when their usage bits permit shader writes.
    if(direct && destination.storageMode!=MTLStorageModePrivate) {
        static int noted; if(!noted++)MLOG("MetalFX spatial output: drawable storage %lu is not private; using the scaler's private output",(unsigned long)destination.storageMode);
        direct=0;
    }
    id<MTLTexture> output=direct?destination:s->output;
    s->scaler.colorTexture=input;s->scaler.outputTexture=output;s->scaler.inputContentWidth=r[2];s->scaler.inputContentHeight=r[3];[s->scaler encodeToCommandBuffer:cb];
    *keep=@[s->scaler,input,output];return output;
}
// A full-size, same-format copy preserves the scaler's encoded pixels. It
// avoids a render pass when the drawable cannot be a legal MetalFX output.
// Crops, orientation changes and framebuffer-only destinations keep present_encode.
static int temporal_async_present_copy(id<MTLCommandBuffer> cb,id<MTLTexture> source,id<MTLTexture> destination) {
    if(atomic_load_explicit(&g_origin_ll,memory_order_relaxed) || source.framebufferOnly || destination.framebufferOnly ||
       source.textureType!=MTLTextureType2D || destination.textureType!=MTLTextureType2D ||
       source.storageMode==MTLStorageModeMemoryless || destination.storageMode==MTLStorageModeMemoryless ||
       source.width!=destination.width || source.height!=destination.height || source.pixelFormat!=destination.pixelFormat)return 0;
    id<MTLBlitCommandEncoder> copy=[cb blitCommandEncoder];if(!copy)return 0;
    [copy copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
              sourceSize:MTLSizeMake(source.width,source.height,1) toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [copy endEncoding];return 1;
}
static int temporal_async_present(uint64_t serial,id<MTLTexture> src,double period,uint64_t frame,uint64_t ticket,const int crop[4],id<CAMetalDrawable> drawable,double target,int generated) {
    int slot=temporal_async_slot(serial);if(slot<0 || !temporal_async_init())return -1;
    pthread_mutex_lock(&g_temporal_async_mu);
    id<MTLCommandBuffer> cb=[g_temporal_display_queue commandBuffer];if(!cb){pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    uint64_t producer=atomic_load(&g_snap[slot].ev);if(producer)[cb encodeWaitForEvent:g_snap_ev value:producer];
    // Native snapshots need only their copy. Generated snapshots additionally
    // wait for their own result, never a later history read of this slot.
    uint64_t ready=generated?atomic_load(&g_temporal_snap[slot].async_ready):0;
    if(ready)[cb encodeWaitForEvent:g_temporal_async_done value:ready];
    NSArray *keep=nil;id<MTLTexture> up=temporal_async_upscale(cb,src,crop,drawable.texture,&keep);int rect[4]={0,0,(int)up.width,(int)up.height};
    static int noted[2];if(up && !noted[!!generated]++){MLOG("asynchronous final MetalFX: %s, source %lux%lu -> drawable %lux%lu",generated?"generated":"native",(unsigned long)src.width,(unsigned long)src.height,(unsigned long)drawable.texture.width,(unsigned long)drawable.texture.height);}
    if(up!=drawable.texture && !(up && temporal_async_present_copy(cb,up,drawable.texture)) && !present_encode(cb,up?:src,drawable.texture,up?rect:crop)){pthread_mutex_unlock(&g_temporal_async_mu);return 0;}
    uint64_t value=++g_temporal_display_sequence;atomic_store(&g_temporal_snap[slot].display_reader,value);
    [cb encodeSignalEvent:g_temporal_display_done value:value];
    present_track_drawable(drawable,frame,ticket,generated);
    if(target>0 || period<=0)[cb presentDrawable:drawable];else [cb presentDrawable:drawable afterMinimumDuration:period];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        (void)keep.count;
        if(finished.error){MLOG("[RED] auxiliary display GPU error: %s",finished.error.description.UTF8String);gpu_lost_check(finished.error,"auxiliary display");}
        pthread_mutex_lock(&g_pq_mu);if(!generated && ticket>g_pq_done)g_pq_done=ticket;g_pq_inflight--;present_gpu_note(finished.GPUEndTime-finished.GPUStartTime,up!=nil);pthread_cond_broadcast(&g_pq_cv);pthread_mutex_unlock(&g_pq_mu);
        temporal_async_note_gpu(frame,up==drawable.texture?(generated?"generated display direct":"native display direct"):(generated?"generated display":"native display"),finished.GPUStartTime,finished.GPUEndTime);
    }];
    pthread_mutex_lock(&g_pq_mu);g_pq_inflight++;pthread_mutex_unlock(&g_pq_mu);
    [cb commit];atomic_store(&g_temporal_display_latest,value);
    pthread_mutex_unlock(&g_temporal_async_mu);present_note_latency(target);return 1;
}
