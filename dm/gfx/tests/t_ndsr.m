// Real MetalFX dispatches through direct and deferred encoding. Constant HDR
// colours provide an independent oracle; outside the output rectangle must stay intact.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, why) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL %s\n",why); } } while(0)
static id<MTLTexture> texture(NSUInteger w, NSUInteger h, MTLPixelFormat pf) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModeShared; d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
    id<MTLTexture> t=[g_dev newTextureWithDescriptor:d]; res_add(t); return t;
}
static void fill(id<MTLTexture> t, const float value[4]) {
    _Float16 *p=malloc(t.width*t.height*8);
    for(NSUInteger k=0;k<t.width*t.height;k++) for(int j=0;j<4;j++) p[4*k+j]=value[j];
    [t replaceRegion:MTLRegionMake2D(0,0,t.width,t.height) mipmapLevel:0 withBytes:p bytesPerRow:t.width*8]; free(p);
}
int main(void) { @autoreleasepool {
    mtl_init();
    CHECK([MTLFXSpatialScalerDescriptor supportsDevice:g_dev],"device supports MetalFX spatial");
    if(failed) return 1;
    NVNcopyRegion r;
    CHECK(ndsr_region((uint32_t[]){3,4,7,8},32,32,&r)&&r.width==8&&r.height==9&&r.xoffset==3,"inclusive relative bounds");
    CHECK(!ndsr_region((uint32_t[]){0,0,UINT32_MAX,1},32,32,&r),"overflow refuses replacement");
    CHECK(!ndsr_region((uint32_t[]){30,4,7,8},32,32,&r),"out-of-bounds region refuses replacement");
    g_ndsr_loaded=1; g_ndsr_nkeys=1; g_ndsr_keys[0]=0x14a8aaf3e0a9edbdull;
    CHECK(ndsr_known("14a8aaf3e0a9edbd_b"),"reviewed kernel accepted");
    CHECK(!ndsr_known("14a8aaf3e0a9edbc_b")&&!ndsr_known("14a8aaf3e0a9edbd_bad"),"unknown kernels and variants rejected");
    const float colors[2][4]={{2.5f,0.5f,0.25f,1},{0.125f,1.5f,0.75f,1}}, sentinel[4]={0.25f,0.75f,0.125f,1};
    id<MTLTexture> input[2], output[2];
    for(int i=0;i<2;i++) { input[i]=texture(96,96,MTLPixelFormatRGBA16Float); output[i]=texture(192,160,MTLPixelFormatRGBA16Float); fill(input[i],colors[i]); fill(output[i],sentinel); }
    NVNcopyRegion sr={7,9,0,64,60,1}, dr={13,17,0,128,120,1};
    mtl_sub_begin();
    for(int i=0;i<2;i++) CHECK(ndsr_encode(input[i],output[i],&sr,&dr),"encode non-origin HDR upscale");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU work completed");
    CHECK(!mtl_last_commit_error(),"GPU feedback has no error");
    for(int i=0;i<2;i++) {
        _Float16 *p=malloc(output[i].width*output[i].height*8);
        [output[i] getBytes:p bytesPerRow:output[i].width*8 fromRegion:MTLRegionMake2D(0,0,output[i].width,output[i].height) mipmapLevel:0];
        unsigned outside=0,inside=0;
        for(NSUInteger y=0;y<output[i].height;y++) for(NSUInteger x=0;x<output[i].width;x++) {
            int in=x>=(NSUInteger)dr.xoffset&&x<(NSUInteger)(dr.xoffset+dr.width)&&y>=(NSUInteger)dr.yoffset&&y<(NSUInteger)(dr.yoffset+dr.height);
            for(int j=0;j<3;j++) { float value=p[4*(y*output[i].width+x)+j],want=in?colors[i][j]:sentinel[j];
                // MetalFX HDR includes reversible tone mapping and approximate
                // filtering (2.5 becomes about 2.44 on M1 Pro). Require colour
                // fidelity within 3%, while rejecting clipping and stale data.
                float tolerance=in?0.03f+0.03f*fabsf(want):0.001f;
                if(!isfinite(value)||fabsf(value-want)>tolerance) { if(in) { if(inside<4) fprintf(stderr,"image%d x%lu y%lu channel%d value%g want%g\n",i,(unsigned long)x,(unsigned long)y,j,value,want); inside++; } else outside++; }
            }
        }
        CHECK(!outside,"pixels outside output region preserved");
        CHECK(!inside,"HDR pixels preserved, with no clipping or stale bindings"); free(p);
    }
    printf("t_ndsr: %d checks, %d failed\n",checks,failed); return failed!=0;
}}
