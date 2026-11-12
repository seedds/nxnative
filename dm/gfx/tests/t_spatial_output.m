// Actual spatial-scaler and final-copy path under Metal API Validation.
#include "../nvn/nvn_mtl.m"
#include <signal.h>
static void no_crash_dialog(int sig) { _Exit(128+sig); }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c,label) do { checks++; if(!(c)){ failed++;fprintf(stderr,"FAIL: %s\n",label); } } while(0)
static id<MTLBuffer> readback(id<MTLCommandBuffer> cb,id<MTLTexture> texture,NSUInteger pitch) {
    id<MTLBuffer> b=[g_dev newBufferWithLength:pitch*texture.height options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
    [e copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
           sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:b destinationOffset:0
           destinationBytesPerRow:pitch destinationBytesPerImage:pitch*texture.height];
    [e endEncoding];return b;
}
static int equal_pixels(id<MTLBuffer> a,id<MTLBuffer> b,NSUInteger pitch,NSUInteger width,NSUInteger height) {
    for(NSUInteger y=0;y<height;y++)if(memcmp((uint8_t *)a.contents+y*pitch,(uint8_t *)b.contents+y*pitch,width*4))return 0;
    return 1;
}
int main(void) { @autoreleasepool {
    signal(SIGABRT,no_crash_dialog);
    setenv("DM_AUDIO","0",1);setenv("DM_PRESENT_UPSCALE","metalfx",1);setenv("DM_PRESENT_UPSCALE_DIRECT","1",1);
    mtl_init();atomic_store(&g_origin_ll,0);
    enum { IW=1536,IH=864,OW=3024,OH=1701 };
    id<MTLCommandQueue> q=[g_dev newCommandQueue];CHECK(q!=nil,"real Metal queue exists");
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:IW height:IH mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> input=[g_dev newTextureWithDescriptor:d];CHECK(input!=nil,"real input texture exists");
    if(!q || !input)return 1;
    uint32_t *pixels=malloc(IW*IH*4);
    for(int y=0;y<IH;y++)for(int x=0;x<IW;x++)pixels[y*IW+x]=((x/32+y/32)&1)?0xffe0a020u:0xff2030e0u;
    [input replaceRegion:MTLRegionMake2D(0,0,IW,IH) mipmapLevel:0 withBytes:pixels bytesPerRow:IW*4];free(pixels);
    int crop[4]={0,0,IW,IH};NSUInteger pitch=(OW*4+255)&~255u;id<MTLBuffer> reference=nil;
    MTLStorageMode modes[]={MTLStorageModeShared,MTLStorageModeManaged,MTLStorageModePrivate};
    for(unsigned k=0;k<sizeof modes/sizeof*modes;k++) {
        d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:OW height:OH mipmapped:NO];
        d.storageMode=modes[k];d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
        id<MTLTexture> destination=[g_dev newTextureWithDescriptor:d];CHECK(destination!=nil,"test destination exists");if(!destination)continue;
        id<MTLCommandBuffer> cb=[q commandBuffer];NSArray *keep=nil;
        id<MTLTexture> output=temporal_async_upscale(cb,input,crop,destination,&keep);
        CHECK(output!=nil,"production MetalFX path encodes");if(!output)return 1;
        CHECK(output.storageMode==MTLStorageModePrivate,"MetalFX always receives a private output");
        CHECK((output==destination)==(destination.storageMode==MTLStorageModePrivate),"direct output only uses a private destination");
        if(output!=destination)CHECK(temporal_async_present_copy(cb,output,destination),"non-private destination receives the exact full-size copy");
        id<MTLBuffer> a=readback(cb,output,pitch),b=readback(cb,destination,pitch);
        [cb commit];[cb waitUntilCompleted];(void)keep.count;
        CHECK(cb.status==MTLCommandBufferStatusCompleted && !cb.error,"actual GPU work completes without validation/error");
        CHECK(equal_pixels(a,b,pitch,OW,OH),"final copy preserves every encoded pixel bit-for-bit");
        CHECK(((uint8_t *)a.contents)[0] || ((uint8_t *)a.contents)[1] || ((uint8_t *)a.contents)[2],"reference contains actual rendered color");
        if(reference)CHECK(equal_pixels(reference,b,pitch,OW,OH),"private direct and copied outputs are identical");else reference=a;
        printf("storage_requested=%lu storage_actual=%lu scaler_output=%lu direct=%d\n",(unsigned long)modes[k],(unsigned long)destination.storageMode,(unsigned long)output.storageMode,output==destination);
    }
    printf("t_spatial_output: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
