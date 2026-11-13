// Optional GPU data for the existing bounded KEYDUMP selection. These are GPU copies at
// the selected draw, not reads of live guest memory in a later completion callback.
// No guest bytes are written. Ending the selected pass can perturb timing: diagnostic only.
static NvProg *g_kd_gpu_pending;
static int kd_gpu_enabled(void) {
    static _Atomic int on=-1;
    int value=atomic_load(&on);
    if(value<0) { const char *e=getenv("DM_NVN_KEYREADBACK"); value=e && !strcmp(e,"1"); atomic_store(&on,value); }
    return value;
}
static id<MTLBuffer> kd_gpu_buffer(id<MTLBuffer> source, NSUInteger offset, NSUInteger size) {
    if(!source || !size || size>4096 || ((offset|size)&3) || offset>source.length || size>source.length-offset) return nil;
    id<MTLBuffer> out=[g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    if(!out) return nil;
    res_add(out);
    enc_b_copy_buffer(blit(),source,offset,out,0,size);
    defer_obj_release(out);
    return out;
}
// Read the two physical planes separately. D24S8 is hosted as D32FS8: neither its
// native eight-byte representation nor a four-byte packed blit is guest D24S8.
static id<MTLBuffer> kd_gpu_plane(id<MTLTexture> source, NSUInteger level, NSUInteger slice, int stencil, NSUInteger *row) {
    if(!source || !row || source.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8 || source.sampleCount!=1 ||
       level>=source.mipmapLevelCount || source.textureType!=MTLTextureType2D) return nil;
    NSUInteger w=MAX(source.width>>level,1ul),h=MAX(source.height>>level,1ul);
    if(w>4096 || h>4096 || slice) return nil;
    *row=(w*(stencil?1:4)+255)&~255ul;
    id<MTLBuffer> out=[g_dev newBufferWithLength:*row*h options:MTLResourceStorageModeShared];
    if(!out) return nil;
    res_add(out);
    enc_b_copy_texture_buffer_opt(blit(),source,slice,level,MTLOriginMake(0,0,0),MTLSizeMake(w,h,1),out,0,*row,*row*h,
        stencil?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil);
    defer_obj_release(out);
    return out;
}
static void kd_gpu_readback(NvProg *program) {
    if(!program || !program->sh[1]) return;
    rp_end();
    NvShader *sh=program->sh[1];
    unsigned long long frame=atomic_load(&g_cur_frame),pass=g_stat_pass;
    NSString *key=@(sh->key);
    for(int i=0;i<sh->rf.nb;i++) {
        const nvn_rbind *b=&sh->rf.b[i]; int reg=b->reg;
        if(b->kind==NVN_RB_SBUFP && b->rw && reg<16) {
            p_bind_buf u=S.ssbo[1][reg]; void *mb; uint64_t off;
            if(!u.addr || !u.size || u.size>4096 || !mtl_addr(u.addr,u.size,&mb,&off)) continue;
            id<MTLBuffer> out=kd_gpu_buffer((__bridge id<MTLBuffer>)mb,off,u.size);
            if(!out) { MLOG("[RED] KEYREADBACK cannot snapshot storage slot %d",reg); continue; }
            [g_commit_blocks addObject:^{
                const uint32_t *v=out.contents; char values[16384]; size_t used=0;
                for(NSUInteger n=0;n<out.length/4 && used<sizeof values-32;n++)
                    used+=snprintf(values+used,sizeof values-used," %lu:%u",(unsigned long)n,v[n]);
                values[used]=0;
                MLOG("[kr] F%llu pass=%llu key=%s sbuf%d addr=%#llx bytes=%llu words%s",frame,pass,key.UTF8String,reg,
                    (unsigned long long)u.addr,(unsigned long long)u.size,values);
            }];
        }
        if(b->kind!=NVN_RB_TEX || reg>=NTEX) continue;
        int tid=(int)(tex_slot_h(S.tex[1],S.img[1],reg)&0xfffff);
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> view=tid?tex_by_id(tid):nil;
        uint64_t serial=tid>0 && tid<g_ntreg && g_treg[tid].ok?g_treg[tid].t.serial:0;
        os_unfair_lock_unlock(&g_rmu);
        if(view.pixelFormat!=MTLPixelFormatX32_Stencil8) continue;
        NSUInteger level=0,slice=0; id<MTLTexture> source=view;
        while(source.parentTexture) { level+=source.parentRelativeLevel; slice+=source.parentRelativeSlice; source=source.parentTexture; }
        NSUInteger sr=0,dr=0,w=view.width,h=view.height;
        id<MTLBuffer> st=kd_gpu_plane(source,level,slice,1,&sr),dz=kd_gpu_plane(source,level,slice,0,&dr);
        if(!st || !dz) { MLOG("[RED] KEYREADBACK unsupported stencil texture slot %d",reg); continue; }
        [g_commit_blocks addObject:^{
            uint64_t hist[256]={0}; float lo=INFINITY,hi=-INFINITY; uint64_t nonfinite=0;
            for(NSUInteger y=0;y<h;y++) {
                const uint8_t *s=(const uint8_t *)st.contents+y*sr;
                const float *d=(const float *)((const uint8_t *)dz.contents+y*dr);
                for(NSUInteger x=0;x<w;x++) { hist[s[x]]++; if(isfinite(d[x])) { lo=fminf(lo,d[x]); hi=fmaxf(hi,d[x]); } else nonfinite++; }
            }
            char values[8192]; size_t used=0;
            for(int n=0;n<256;n++) if(hist[n]) used+=snprintf(values+used,sizeof values-used," %02x:%llu",n,(unsigned long long)hist[n]);
            values[used]=0;
            MLOG("[kr] F%llu pass=%llu key=%s tex%d ser=%llu %lux%lu depth=%g..%g nonfinite=%llu stencil%s",frame,pass,key.UTF8String,reg,
                (unsigned long long)serial,(unsigned long)w,(unsigned long)h,lo,hi,(unsigned long long)nonfinite,values);
        }];
    }
}
