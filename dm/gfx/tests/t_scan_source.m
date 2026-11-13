// The scan query needs its native-size depth/stencil source while the scene stays
// high resolution. Validate the existing production GPU resampler pixel by pixel.
#include "../nvn/nvn_mtl.m"
#include <math.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks,failed;
#define CHECK(c,label) do { checks++; if(!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
static float z_at(unsigned x,unsigned y) {
    // Include clear/background depth 1, depth 0 and float values across D24
    // rounding boundaries, rather than only a constant depth or binary fractions.
    unsigned seed=x*1664525u+y*1013904223u;
    if((seed&31)==0)return 0; if((seed&31)==2)return 1; if((seed&31)==4)return .5f;
    uint32_t bits=0x3e800000u+(seed&0xffffffu);float v;memcpy(&v,&bits,4);return v;
}
static uint8_t s_at(unsigned x,unsigned y) { return (uint8_t)(x+7*y+(x>>1)); }
static void fixture(int format,unsigned W,unsigned H,unsigned index) {
    enum { QW=1600,QH=900 };
    mtl_sub_begin();state_reset();memset(&R,0,sizeof R);
    tex_t source={.magic=MAGIC('T','E','X',0),.serial=987000u+index*4,.off=(int64_t)(index+1)<<28};
    source.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=format,.width=W,.height=H,.depth=1,.levels=1,.samples=1};
    nvn_tex_layout(&source.d,&source.L);mtl_tex_init(&source);
    tex_t query=source;query.serial++;query.off=0;query.readback_proxy=1;query.d.width=QW;query.d.height=QH;nvn_tex_layout(&query.d,&query.L);mtl_tex_init(&query);
    id<MTLTexture> src=tex_mtl(&source),dst=tex_mtl(&query);
    CHECK(src && dst,"scene and independent query source allocate");if(!src || !dst)exit(1);
    tex_t sibling=query;sibling.serial+=10000000;mtl_tex_init(&sibling);
    CHECK(tex_mtl(&sibling)!=dst,"host-owned proxies with identical descriptors have independent Metal storage");
    mtl_tex_release(&sibling);
    NSUInteger rowz=W*4,rows=(W+255)&~255ul;
    id<MTLBuffer> zs=[g_dev newBufferWithLength:rowz*H options:MTLResourceStorageModeShared];
    id<MTLBuffer> ss=[g_dev newBufferWithLength:rows*H options:MTLResourceStorageModeShared];
    CHECK(zs && ss,"literal source planes allocate");if(!zs || !ss)exit(1);
    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++) { ((float *)zs.contents)[y*W+x]=z_at(x,y);((uint8_t *)ss.contents)[y*rows+x]=s_at(x,y); }
    res_add(zs);res_add(ss);
    enc_b_copy_buffer_texture_opt(blit(),zs,0,rowz,rowz*H,MTLSizeMake(W,H,1),src,0,0,MTLOriginMake(0,0,0),MTLBlitOptionDepthFromDepthStencil);
    enc_b_copy_buffer_texture_opt(blit(),ss,0,rows,rows*H,MTLSizeMake(W,H,1),src,0,0,MTLOriginMake(0,0,0),MTLBlitOptionStencilFromDepthStencil);
    p_copy_t2t copy={.src=&source,.dst=&query,.sr={.width=W,.height=H,.depth=1},.dr={.width=QW,.height=QH,.depth=1}};
    CHECK(copy_t2t_enc(&copy)!=nil,"production copy creates native-size query source from high-resolution scene");
    NSUInteger qzr=0,qsr=0,szr=0,ssr=0;
    id<MTLBuffer> qz=kd_gpu_plane(dst,0,0,0,&qzr),qs=kd_gpu_plane(dst,0,0,1,&qsr);
    id<MTLBuffer> sz=kd_gpu_plane(src,0,0,0,&szr),st=kd_gpu_plane(src,0,0,1,&ssr);
    CHECK(qz && qs && sz && st,"both source and query planes snapshot");if(!qz || !qs || !sz || !st)exit(1);
    // The production query's next operation: a 120x100 ROI into guest-shared
    // storage, with padding and independent CPU data on the same allocation.
    void *guest=NULL;size_t bytes=65536; if(posix_memalign(&guest,getpagesize(),bytes))abort();
    memset(guest,0xa5,bytes);mtl_pool_add(guest,bytes);
    enum { RW=120,RH=100,ROW=512,BASE=64,X=713,Y=327 };
    if(format==NVN_FORMAT_DEPTH24_STENCIL8) {
        query.readback_proxy=1;
        _Alignas(8) uint8_t record[COPY_STRIDES_OFF+sizeof(p_copy_strides)]={0};
        p_copy_b2t *transfer=(void *)record;
        *transfer=(p_copy_b2t){.tex=&query,.src=(uint64_t)guest+BASE,.r={.xoffset=X,.yoffset=Y,.width=RW,.height=RH,.depth=1}};
        p_copy_strides strides={ROW,ROW*RH};memcpy(record+COPY_STRIDES_OFF,&strides,sizeof strides);
        copy_t2b(transfer,sizeof record+sizeof(op_hdr));
    }
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU resample and snapshots finish");mtl_drain();
    if(format==NVN_FORMAT_DEPTH24_STENCIL8) {
        unsigned bad=0,guards=0;
        for(unsigned y=0;y<RH;y++)for(unsigned x=0;x<RW;x++) {
            unsigned sx=((2*(X+x)+1)*W)/(2*QW),sy=((2*(Y+y)+1)*H)/(2*QH);
            uint32_t depth=(uint32_t)llrint((double)z_at(sx,sy)*16777215.0);
            uint32_t expected=(depth<<8)|s_at(sx,sy),got;
            memcpy(&got,(uint8_t *)guest+BASE+y*ROW+x*4,4);bad+=expected!=got;
        }
        for(size_t i=0;i<bytes;i++) {
            int inside=i>=BASE && i<BASE+ROW*RH && (i-BASE)%ROW<RW*4;
            if(!inside)guards+=((uint8_t *)guest)[i]!=0xa5;
        }
        CHECK(!bad,"all 12000 GPU-packed ROI words match independent depth24/stencil values");
        CHECK(!guards,"readback preserves row padding and neighboring live CPU bytes");
        printf("SCAN_PACK roi_bad=%u guard_bad=%u\n",bad,guards);
    }
    mtl_pool_del(guest);free(guest);
    CHECK(!mtl_last_commit_error(),"GPU reports no error");
    unsigned long query_bad=0,source_bad=0;
    for(unsigned y=0;y<QH;y++)for(unsigned x=0;x<QW;x++) {
        // Nearest sampling at each destination pixel centre, not filtering IDs.
        unsigned sx=((2*x+1)*W)/(2*QW),sy=((2*y+1)*H)/(2*QH);
        float z=((const float *)((const uint8_t *)qz.contents+y*qzr))[x];
        uint8_t s=((const uint8_t *)qs.contents)[y*qsr+x];
        query_bad+=z!=z_at(sx,sy)||s!=s_at(sx,sy);
    }
    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++) {
        float z=((const float *)((const uint8_t *)sz.contents+y*szr))[x];
        uint8_t s=((const uint8_t *)st.contents)[y*ssr+x];
        source_bad+=z!=z_at(x,y)||s!=s_at(x,y);
    }
    CHECK(query_bad==0,"every query depth and stencil texel equals the expected source sample");
    CHECK(source_bad==0,"every high-resolution scene texel remains unchanged");
    CHECK(src.width==W && src.height==H && dst.width==QW && dst.height==QH,"query scaling does not lower scene render-target resolution");
    printf("SCAN_SOURCE format=%#x %ux%u -> 1600x900 query_bad=%lu source_bad=%lu\n",format,W,H,query_bad,source_bad);
    res_remove(zs);res_remove(ss);mtl_tex_release(&query);mtl_tex_release(&source);
}
int main(void) { @autoreleasepool {
    mtl_init();
    fixture(NVN_FORMAT_DEPTH24_STENCIL8,3200,1800,0);fixture(NVN_FORMAT_DEPTH32F_STENCIL8,3200,1800,1);
    fixture(NVN_FORMAT_DEPTH24_STENCIL8,3840,2160,2);fixture(NVN_FORMAT_DEPTH32F_STENCIL8,3840,2160,3);
    printf("SCAN_SOURCE %d checks, %d failed\n",checks,failed);return failed!=0;
} }
