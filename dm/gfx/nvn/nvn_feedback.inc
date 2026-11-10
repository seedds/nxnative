// Transform feedback uses a void vertex entry. For GS output the generated
// reader gathers packed primitive indices; its draw count stays on the GPU.
enum { TF_SLOT = 8 };
static id<MTLBuffer> g_tf_state;
static id<MTLComputePipelineState> g_tf_prepare;
// The capture state (four byte offsets, the count, tf_prepare's words) is known on the CPU from a begin until a resume loads it from a control buffer or a geometry capture
// takes its count from the GPU: an array capture's count is in its draw. While it is known a capture needs no tf_prepare dispatch and no copy into g_tf_state, and captures that
// follow each other share one attachment-less pass (g_tf_enc), as long as none reads or writes what another of the pass writes. Pokémon Scarlet and Violet skin with a begin,
// one capture and an end per mesh, 73 to 182 a frame: each was a copy encoder, a compute encoder and a render pass behind full barriers, and on an iPad Pro M4 the GPU had
// nothing to run for 17 of a frame's 34 ms (Scarlet) and 45 of 58 ms (Violet) between those encoders. DM_NVN_TF_CPU=0: every capture through tf_prepare in a pass of its own
static uint32_t g_tf_cpu[8]; static int g_tf_known;
static id<MTL4RenderCommandEncoder> g_tf_enc; static struct { uint64_t lo, hi; } g_tf_w[1024]; static int g_tf_nw; static uint64_t g_tf_ncpu, g_tf_ngpu, g_tf_npass;
// The states an end or a pause saved into control buffers while the capture pass was open: copied when the pass ends, before anything that could read them (a draw that takes
// its count from one, a resume, the end of the command buffer). Scarlet ends every capture with a control buffer, and a copy at each end closed the pass after one capture
static struct { uint64_t ctl; uint32_t st[8]; } g_tf_save[256]; static int g_tf_nsave;
static int tf_cpu_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_TF_CPU"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("transform feedback: every capture through tf_prepare, in a pass of its own (DM_NVN_TF_CPU=0)"); } return v; }
static void tf_pass_end(void) {   // before any other encoder of the command buffer begins, and before the command buffer ends. Leaves no encoder open
    if (!g_tf_enc) return;
    id<MTL4RenderCommandEncoder> e = g_tf_enc; g_tf_enc = nil; g_tf_nw = 0; enc_r_end(e);
    if (!g_tf_nsave) return;
    int n = g_tf_nsave; g_tf_nsave = 0;
    id<MTLBuffer> buf; size_t so = arena_alloc((size_t)n * 32, &buf);
    for (int i = 0; i < n; i++) { void *buffer; uint64_t off, gpu;
        memcpy((uint8_t *)buf.contents + so + (size_t)i * 32, g_tf_save[i].st, 32);
        if (!mtl_gpu_addr(g_tf_save[i].ctl, 32, &buffer, &off, &gpu)) { RED_ONCE(103, "transform feedback control buffer is outside a memory pool"); continue; }
        enc_b_copy_buffer(blit_buf(0,0,g_tf_save[i].ctl,g_tf_save[i].ctl+32), buf, so + (size_t)i * 32, (__bridge id<MTLBuffer>)buffer, off, 32); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
}
void mtl_tf_stats(uint64_t v[3]) { v[0] = g_tf_ncpu; v[1] = g_tf_ngpu; v[2] = g_tf_npass; }
static void tf_state_to_gpu(void) {   // the CPU's state into g_tf_state, in GPU order: what follows takes it from there
    if (!g_tf_known) return;
    id<MTLBuffer> buf; size_t off = arena_alloc(32, &buf); memcpy((uint8_t *)buf.contents + off, g_tf_cpu, 32);
    enc_b_copy_buffer(blit(), buf, off, g_tf_state, 0, 32); g_tf_known = 0;
}
static void tf_state_gpu(const p_tf_state *b) {
    if (!g_tf_state) { g_tf_state = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared]; res_add(g_tf_state); }
    static const uint32_t initial[8] = {0,0,0,0,0,1,0,0};
    if (tf_cpu_on() && (b->what == 0 || !b->ctl)) {   // a begin, or an end / pause / resume that names no control buffer: nothing for the GPU
        if (b->what == 0) { memcpy(g_tf_cpu, initial, 32); g_tf_known = 1; }
        return; }
    if (tf_cpu_on() && g_tf_known && g_tf_enc && b->what != 3) {   // an end or a pause with a control buffer while the capture pass is open: saved when the pass ends (g_tf_save)
        if (g_tf_nsave == (int)(sizeof g_tf_save / sizeof *g_tf_save) || g_tf_nw == (int)(sizeof g_tf_w / sizeof *g_tf_w)) tf_pass_end();
        else { g_tf_save[g_tf_nsave].ctl = b->ctl; memcpy(g_tf_save[g_tf_nsave].st, g_tf_cpu, 32); g_tf_nsave++;
               g_tf_w[g_tf_nw].lo = b->ctl; g_tf_w[g_tf_nw].hi = b->ctl + 32; g_tf_nw++;   // a later capture of the pass that reads it ends the pass first (tf_joins)
               pend_add(b->ctl,32,4); g_wr_epoch++; g_mdx_epoch++; return; } }
    rp_end(); ubd_land();
    if (b->what == 0) {
        id<MTLBuffer> buf; size_t off = arena_alloc(32, &buf);
        memcpy((uint8_t *)buf.contents + off, initial, 32);
        enc_b_copy_buffer(blit(), buf, off, g_tf_state, 0, 32);
    } else if (b->ctl) {
        void *buffer; uint64_t off, gpu;
        if (!mtl_gpu_addr(b->ctl, 32, &buffer, &off, &gpu)) { RED_ONCE(103, "transform feedback control buffer is outside a memory pool"); return; }
        if (b->what == 3) { enc_b_copy_buffer(blit_buf(b->ctl,b->ctl+32,0,0), (__bridge id<MTLBuffer>)buffer, off, g_tf_state, 0, 32); g_tf_known = 0; }
        else if (g_tf_known) {   // the saved state is the CPU's: the same bytes tf_prepare would have left in g_tf_state
            id<MTLBuffer> buf; size_t so = arena_alloc(32, &buf); memcpy((uint8_t *)buf.contents + so, g_tf_cpu, 32);
            enc_b_copy_buffer(blit_buf(0,0,b->ctl,b->ctl+32), buf, so, (__bridge id<MTLBuffer>)buffer, off, 32); pend_add(b->ctl,32,4); }
        else { enc_b_copy_buffer(blit_buf(0,0,b->ctl,b->ctl+32), g_tf_state, 0, (__bridge id<MTLBuffer>)buffer, off, 32); pend_add(b->ctl,32,4); }
    }
    g_wr_epoch++; g_mdx_epoch++;
}
static id<MTLRenderPipelineState> tf_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = TF_MARK;
    id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached || hm_get_id(&g_pso_fail, &k)) return cached;
    NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(pso_desc(&k, vs, nil, nil, 0), !k.border, &e);
    if (!ps) { MLOG("[RED] transform feedback pipeline %s: %s", vs->key, e.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null); }
    else hm_put_id(&g_pso, &k, ps);
    return ps;
}
static uint64_t tf_prepare(NvShader *vs, uint64_t source_count, uint64_t order, uint32_t count, uint32_t primitive,
                           uint32_t first, uint32_t per_instance, uint32_t base_instance, uint64_t dst[4]) {
    if (!g_tf_state || !vs || !vs->tf_fn) { RED_ONCE(100, "transform feedback shader %s has no capture entry", vs ? vs->key : "-"); return 0; }
    tf_state_to_gpu(); g_tf_ngpu++;
    nvn_tf_job j = {.order=order,.first=first,.count=per_instance,.base_instance=base_instance,
                    .state=g_tf_state.gpuAddress,.source_count=source_count,.requested=count,.primitive=primitive};
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        void *buf; uint64_t off;
        if (!S.tf[b].addr || S.tf[b].size > UINT32_MAX || !mtl_gpu_addr(S.tf[b].addr,S.tf[b].size,&buf,&off,&dst[b])) {
            RED_ONCE(102, "transform feedback buffer %d of %s is unbound or outside its memory pool", b, vs->key); return 0; }
        j.stride[b]=vs->tf_stride[b];j.capacity[b]=(uint32_t)S.tf[b].size;
    }
    if (!g_tf_prepare) {
        NSError *e=nil; id<MTLLibrary> lib=host_lib("tf",&e);
        if (lib) g_tf_prepare=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tf_prepare"] error:&e];
        if (!g_tf_prepare) { MLOG("[RED] transform feedback prepare: %s",e.localizedDescription.UTF8String); return 0; }
    }
    rp_end(); ubd_land();
    if (g_blit) { enc_b_end(g_blit);g_blit=nil; }
    id<MTLBuffer> buf;size_t off=arena_alloc(sizeof j,&buf);memcpy((uint8_t *)buf.contents+off,&j,sizeof j);
    uint64_t job=buf.gpuAddress+off;
    id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,g_tf_prepare);enc_c_args(ce,job,0);
    enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(ce);
    return job;
}
static void tf_note(NvShader *vs) {
    for(int b=0;b<4;b++)if(vs->tf_stride[b])pend_add(S.tf[b].addr,S.tf[b].size,4);
    args_dirty_all();g_enc_pso=nil;g_enc_dss=nil;g_ed.ok=0;g_wr_epoch++;g_mdx_epoch++;
}
// An array capture whose state the CPU knows: tf_prepare's arithmetic (dm/gfx/msl/tf.metal) done here. 0: a capture buffer is unbound or outside its pool
static int tf_job_cpu(NvShader *vs, uint32_t count, uint32_t primitive, const p_draw *dw, nvn_tf_job *j, uint64_t dst[4]) {
    *j = (nvn_tf_job){.first=(uint32_t)dw->first,.count=(uint32_t)dw->count,.base_instance=(uint32_t)dw->base_instance,.state=g_tf_state.gpuAddress,.requested=count,.primitive=primitive};
    uint32_t n = count;
    for (int b = 0; b < 4; b++) {
        j->offset[b] = g_tf_cpu[b];
        if (!vs->tf_stride[b]) continue;
        void *buf; uint64_t off;
        if (!S.tf[b].addr || S.tf[b].size > UINT32_MAX || !mtl_gpu_addr(S.tf[b].addr,S.tf[b].size,&buf,&off,&dst[b])) {
            RED_ONCE(102, "transform feedback buffer %d of %s is unbound or outside its memory pool", b, vs->key); return 0; }
        j->stride[b] = vs->tf_stride[b]; j->capacity[b] = (uint32_t)S.tf[b].size;
        n = MIN(n, (j->capacity[b] - MIN(j->capacity[b], g_tf_cpu[b])) / j->stride[b]);
    }
    n -= n % primitive; j->args[0] = n; j->args[1] = 1; j->args[2] = 0; j->args[3] = j->base_instance;
    for (int b = 0; b < 4; b++) if (j->stride[b]) g_tf_cpu[b] += n * j->stride[b];
    g_tf_cpu[4] += n; g_tf_cpu[5] = 1; g_tf_cpu[6] = g_tf_cpu[7] = 0;
    return 1;
}
// Whether the capture about to be drawn may join the open capture pass: nothing it reads (vertex streams, the vertex stage's uniform and storage buffers) and nothing it
// writes lies in a range a capture of that pass writes. Metal orders nothing between the draws of one pass for what their vertex functions write
static int tf_joins(const nvn_tf_job *j) {
    if (g_tf_nw + 4 > (int)(sizeof g_tf_w / sizeof *g_tf_w)) return 0;
    struct { uint64_t lo, hi; } m[52]; int nm = 0;   // what this capture reads and writes
    for (int s = 0; s < 16; s++) if (S.vbuf[s].address && S.vbuf[s].size) { m[nm].lo = S.vbuf[s].address; m[nm].hi = m[nm].lo + S.vbuf[s].size; nm++; }
    for (int r = 0; r < 16; r++) { const p_bind_buf *u = &S.ubo[0][r], *t = &S.ssbo[0][r];
        if (u->addr && u->size) { m[nm].lo = u->addr; m[nm].hi = u->addr + u->size; nm++; }
        if (t->addr && t->size) { m[nm].lo = t->addr; m[nm].hi = t->addr + t->size; nm++; } }
    for (int b = 0; b < 4; b++) if (j->stride[b] && j->args[0]) { m[nm].lo = S.tf[b].addr + j->offset[b]; m[nm].hi = m[nm].lo + (uint64_t)j->args[0] * j->stride[b]; nm++; }
    for (int k = 0; k < g_tf_nw; k++) for (int i = 0; i < nm; i++) if (m[i].lo < g_tf_w[k].hi && g_tf_w[k].lo < m[i].hi) return 0;
    return 1;
}
// An inline update made while the capture pass is open is deferred (mtl_sub_update) and lands after the pass. A capture that reads one (ubd_conflict: a vertex stream, a
// storage buffer, a uniform binding the shadows cannot give) or writes over one does not join: the pass ends and the updates land before it, as before every tf_prepare
static int tf_update_waits(NvProg *pr, const p_draw *dw, const nvn_tf_job *j) {
    if (!g_nubd) return 0;
    for (int b = 0; b < 4; b++) if (j->stride[b] && j->args[0]) { uint64_t lo = S.tf[b].addr + j->offset[b]; if (ubd_hits(lo, lo + (uint64_t)j->args[0] * j->stride[b])) return 1; }
    return ubd_conflict(pr, dw, NULL);
}
static void tf_draw(NvProg *pr,const p_draw *dw,const p_draw_idx *di) {
    NvShader *vs=pr?pr->sh[0]:nil;
    if(!dw||dw->ind||dw->mdi_cnt||!(dw->prim==0||dw->prim==1||dw->prim==4)) {
        RED_ONCE(101,"transform feedback array capture requires a point, line or triangle list (vertex %s)",vs?vs->key:"-");return; }
    if(dw->count<=0||dw->instances<0)return;
    unsigned instances=MAX(dw->instances,1);uint64_t count=(uint64_t)dw->count*instances;
    if(count>UINT32_MAX)return;
    id<MTLRenderPipelineState> ps=vs&&vs->tf_fn?tf_pso(vs):nil;if(!ps)return;
    uint32_t primitive=dw->prim==0?1:dw->prim==1?2:3;
    uint64_t dst[4]={0},wl[4]={0},wh[4]={0};uint64_t job=0;int cpu=tf_cpu_on()&&g_tf_known&&g_tf_state,nw=0;
    if(cpu) {
        nvn_tf_job j;if(!tf_job_cpu(vs,(uint32_t)count,primitive,dw,&j,dst))return;
        if(g_tf_enc&&(!tf_joins(&j)||tf_update_waits(pr,dw,&j)))tf_pass_end();
        id<MTLBuffer> buf;size_t off=arena_alloc(sizeof j,&buf);memcpy((uint8_t *)buf.contents+off,&j,sizeof j);job=buf.gpuAddress+off;
        for(int b=0;b<4;b++)if(j.stride[b]&&j.args[0]){wl[nw]=S.tf[b].addr+j.offset[b];wh[nw]=wl[nw]+(uint64_t)j.args[0]*j.stride[b];nw++;}
        g_tf_ncpu++;
    } else {
        tf_pass_end();
        job=tf_prepare(vs,0,0,(uint32_t)count,primitive,dw->first,dw->count,dw->base_instance,dst);if(!job)return;
    }
    if(!g_tf_enc){rp_end();ubd_land();}
    vbuf_prepare(vs?&vs->rf:NULL,dw,NULL);   // a copy or a conversion it makes ends the open capture pass (tf_pass_end where the encoders begin)
    if(!g_tf_enc) {
        if(g_blit){enc_b_end(g_blit);g_blit=nil;}
        MTL4RenderPassDescriptor *rd=[MTL4RenderPassDescriptor new];rd.renderTargetWidth=rd.renderTargetHeight=rd.defaultRasterSampleCount=1;
        g_tf_enc=enc_r_begin(rd);g_tf_npass++;
    }
    id<MTL4RenderCommandEncoder> en=g_tf_enc;enc_r_pso(en,ps);
    {int bad=bind_vbufs(en,&vs->rf);if(bad>=0){RED_ONCE(158,"vertex stream %d's buffer %#llx is in no memory pool; draw skipped",bad,(unsigned long long)S.vbuf[bad].address);tf_pass_end();return;}}
    vconv_bind(en,vs,1,dw,NULL);
    args_dirty_all();encode_args(en,vs,0,1);enc_r_vbuf(en,14,job);
    for(int b=0;b<4;b++)if(vs->tf_stride[b])enc_r_vbuf(en,TF_SLOT+b,dst[b]);
    enc_r_draw_instanced(en,MTLPrimitiveTypePoint,dw->first,dw->count,instances,dw->base_instance);
    for(int k=0;k<nw;k++){g_tf_w[g_tf_nw].lo=wl[k];g_tf_w[g_tf_nw].hi=wh[k];g_tf_nw++;}   // what later captures of this pass must not touch (tf_joins)
    if(!cpu)tf_pass_end();
    swrite_note(pr);tf_note(vs);
    PD("T %llu tf capture vs=%s first=%d count=%d instances=%u%s",g_stat_pass,vs->key,dw->first,dw->count,instances,cpu?" (state on the CPU)":"");
}
static void tf_geometry(NvShader *reader,uint64_t indices,uint64_t args,uint32_t primitive,uint32_t base_instance) {
    if(!reader->tf_fn){RED_ONCE(104,"geometry feedback reader %s has no capture entry",reader->key);return;}
    id<MTLRenderPipelineState> ps=tf_pso(reader);if(!ps)return;
    uint64_t dst[4]={0};uint64_t job=tf_prepare(reader,args,indices,0,primitive,0,0,base_instance,dst);if(!job)return;
    tf_pass_end();
    MTL4RenderPassDescriptor *rd=[MTL4RenderPassDescriptor new];rd.renderTargetWidth=rd.renderTargetHeight=rd.defaultRasterSampleCount=1;
    id<MTL4RenderCommandEncoder> en=enc_r_begin(rd);enc_r_pso(en,ps);args_dirty_all();encode_args(en,reader,0,1);enc_r_vbuf(en,14,job);
    for(int b=0;b<4;b++)if(reader->tf_stride[b])enc_r_vbuf(en,TF_SLOT+b,dst[b]);
    enc_r_draw_indirect(en,MTLPrimitiveTypePoint,job+16);enc_r_end(en);tf_note(reader);
}
