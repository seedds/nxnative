// Present crop regression: colored 8×8 texture, closed-form RGBA expectations; real Metal rendering, no dependence on the game or on cross-run determinism.
// nvn_mtl.m's nvn_game_fps is a weak default and hle_nvn.c gives the strong definition; in one translation unit that is a redefinition, so the weak one is renamed (this test does not look at the frame-rate target)
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle   // both files have a static g_ft (nvn_mtl.m's fragment argument table, hle_nvn.c's textures a flush can mark CPU-written), so hle_nvn.c's is renamed too
#include "../nvn/hle_nvn.c"
#undef g_ft
#include <assert.h>
uint64_t g_t0_ns;   // process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int g_layer_crop[4];   // what the vi layer crop (hle_vi.c) answers: x, y, w, h, all 0 = none
void dm_vi_layer_crop(int c[4]) { memcpy(c, g_layer_crop, sizeof g_layer_crop); }
#ifdef CROP_LEGACY_HEADER
#include CROP_LEGACY_HEADER
#define ENCODE legacy_present_encode
#else
#define ENCODE present_encode
#endif
static int failed, cases;
#define EXPECT(n, w, v) do { int a=(w),b=(v); cases++; printf("%s expected %d → got %d %s\n",n,a,b,a==b?"✓":"✗");failed+=a!=b; } while(0)
static const uint8_t colors[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}};
#ifndef CROP_LEGACY_HEADER
extern int g_shape_w,g_shape_h;   // t_host_stubs.c: the last shape the present reported to the front end
extern int g_layer_hdr,g_layer_hdr_calls;   // t_host_stubs.c: the HDR layer the present asked for (front_layer_hdr)
#endif

#ifndef CROP_LEGACY_HEADER
// Exercise the persistent scaler used by real presents, completing each command buffer before its allocator and output leave scope.
static int check_present_resize(id<MTLTexture> src, int w, int h) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> out=[g_dev newTextureWithDescriptor:d];res_add(out);res_commit();
    id<MTL4CommandAllocator> al=[g_dev newCommandAllocator];id<MTL4CommandBuffer> cb=[g_dev newCommandBuffer];id<MTLSharedEvent> ev=[g_dev newSharedEvent];
    assert(al && ev);
    [cb beginCommandBufferWithAllocator:al];int kind=present_encode_m4(cb,src,out,NULL,g_present_uv[0],&g_fx_present);[cb endCommandBuffer];
    [g_q4 commit:&cb count:1];[g_q4 signalEvent:ev value:1];
    EXPECT("resize present completes",1,[ev waitUntilSignaledValue:1 timeoutMS:10000]);res_remove(out);return kind;
}

// ---------- HDR output (DM_HDR): the present's routes, its pipelines, the headroom vi and the inverse tone map take ----------
// Reference values: a float64 port of present.metal's routes (BT.2446-A from mtld3d with P_SDR from its formula, L_HDR = min(headroom x 203, 10000), output / 203, the smooth
// maximum of SDR and Method A in ln luminance (width min(0.12, L_HDR / 203 - 1)) through one ICtCp ratio; ST 2084), run in the work folder. In SDR-white
// units, linear mid grey 0.18 -> 0.87218 at a headroom of 16, 0.83317 at 15, 0.53161 at 8, 0.37815 at 5, 0.37262 at 4.9, 0.32120 at 4, 0.25986 at 3, 0.19336 at 2, 0.18 at 1.5
// and 1.0 (Method A darker than SDR there); white -> the headroom; at 60, L_HDR 10000 (PQ's limit): grey 1.91653, white 49.26126, BT.709 blue 1.0 (0.6998, 0, 18.1979).
// L_HDR 710 / 1000 (Method A's design point) / 2000: grey 0.29102 / 0.37407 / 0.61741, white 3.49754 / 4.92611 / 9.85223. Code 118 (linear 0.181164): 0.32363 at 4.
// Y 0.01 / 0.05 / 0.41 / 0.7: 0.01387 / 0.08385 / 1.01302 / 2.27328 at L_HDR 1000, 0.02378 / 0.16243 / 2.52156 / 6.25074 at a headroom of 15. BT.709
// (0.5, 0.25, 0.125): (1.2015, 0.5590, 0.2579) at L_HDR 1000, (3.0117, 1.2866, 0.5381) at 15, its ICtCp hue angle kept.
// At a headroom of 1.25, sRGB 226.8 / 230.8 / 234.8 (16-bit 58288 / 59316 / 60344): 0.78114 / 0.82268 / 0.86974.
// PQ code 594/1023 = 202.9 nits, / 203 = 0.99958 (sRGB code 254.95). BT.709 linear (0.5, 0.25, 0.125) is BT.2020 (0.40144, 0.26585, 0.14215), PQ codes at 203 nits a unit
// (499, 458, 398), which decode to sRGB codes (187.6, 137.1, 98.9)
#define EXPECTF(n, w, v, tol) do { double a_=(w),b_=(v); int ok_=fabs(a_-b_)<=(tol); cases++; printf("%s expected %.5f → got %.5f %s\n",n,a_,b_,ok_?"✓":"✗");failed+=!ok_; } while(0)
static id<MTLTexture> hdr_src(MTLPixelFormat pf, int w, int h, const void *texel, size_t bpp) {   // a window texture of one value
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> t=[g_dev newTextureWithDescriptor:d];uint8_t *b=malloc((size_t)w*h*bpp);
    for(int i=0;i<w*h;i++)memcpy(b+i*bpp,texel,bpp);
    [t replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:b bytesPerRow:w*bpp];free(b);res_add(t);res_commit();return t;
}
// The present of src (whole) into an out_pf target of n x n, as present_offscreen_wait draws captures and, with fx, MetalFX first; the centre pixel as floats (an 8-bit target as
// codes 0-255, R G B). Returns the kind (PRESENT_*)
static int hdr_present(id<MTLTexture> src, MTLPixelFormat out_pf, int n, double rgb[3]) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:out_pf width:n height:n mipmapped:NO];d.storageMode=MTLStorageModePrivate;
    d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;   // what MetalFX needs to write it directly, which an HDR route must not do
    id<MTLTexture> out=[g_dev newTextureWithDescriptor:d];int bpp=out_pf==MTLPixelFormatRGBA16Float?8:4;
    int kind=present_offscreen_wait(src,NULL,out);
    id<MTLBuffer> rb=[g_dev newBufferWithLength:(NSUInteger)n*n*bpp options:MTLResourceStorageModeShared];
    if(!out||!rb){EXPECT("HDR route target and readback made",1,0);rgb[0]=rgb[1]=rgb[2]=-1;return PRESENT_FAILED;}
    id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
    [bl copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(n,n,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:n*bpp destinationBytesPerImage:n*n*bpp];
    [bl endEncoding];[cb commit];[cb waitUntilCompleted];
    const uint8_t *p=(const uint8_t *)rb.contents+((size_t)(n/2)*n+n/2)*bpp;
    if(bpp==8){const __fp16 *h=(const __fp16 *)p;for(int c=0;c<3;c++)rgb[c]=h[c];}
    else for(int c=0;c<3;c++)rgb[c]=out_pf==MTLPixelFormatBGRA8Unorm?p[2-c]:p[c];
    return kind;
}
static uint32_t rgb10a2(unsigned r,unsigned g,unsigned b) { return r|g<<10|b<<20|3u<<30; }
static void hdr_checks(void) {
    double v[3];
    {   // P_SDR in the shader is BT.2446-A's formula at an SDR white of 100 nits (mtld3d's 5.4395 is 87 nits): the constants as written in present.metal
        FILE *f=fopen("../msl/present.metal","r");char line[512];double p=0,lp=0;   // run from dm/gfx/tests (make check_t_present_crop), or from the repository's root
        if(!f)f=fopen("dm/gfx/msl/present.metal","r");
        EXPECT("present.metal found (the test runs from dm/gfx/tests or the repository's root)",1,f!=NULL);
        while(f&&fgets(line,sizeof line,f)){char *q;if((q=strstr(line,"float P_SDR = ")))p=atof(q+14);if((q=strstr(line,"float LOG_P_SDR = ")))lp=atof(q+18);}
        if(f)fclose(f);
        double want=1+32*pow(100.0/10000,1/2.4);
        EXPECTF("present.metal P_SDR is 1 + 32 * (100 / 10000)^(1/2.4)",want,p,1e-6);EXPECTF("present.metal LOG_P_SDR is log(P_SDR)",log(want),lp,1e-6);
    }
    // Routes by the window's format and the target's (a 10-bit window is PQ while vi reports HDR to the game)
    EXPECT("8-bit window into BGRA8: the bytes as they are",ROUTE_COPY,present_route(MTLPixelFormatRGBA8Unorm,MTLPixelFormatBGRA8Unorm));
    EXPECT("8-bit window into RGBA16Float: the inverse tone map",ROUTE_ITM,present_route(MTLPixelFormatBGRA8Unorm_sRGB,MTLPixelFormatRGBA16Float));
    EXPECT("10-bit window of a game never told HDR into BGRA8: as 8-bit values",ROUTE_COPY,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatBGRA8Unorm));
    uint32_t pq594=rgb10a2(594,594,594);id<MTLTexture> pqw=hdr_src(MTLPixelFormatRGB10A2Unorm,4,4,&pq594,4);
    hdr_present(pqw,MTLPixelFormatBGRA8Unorm,4,v);EXPECTF("a 10-bit window of a game never told HDR keeps its raw value (594 of 1023 as 148 of 255)",594.0/1023*255,v[0],1);
    atomic_store(&g_hdr_vi_on,1);
    EXPECT("PQ window into BGRA8: decoded to SDR",ROUTE_PQ_SDR,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatBGRA8Unorm));
    EXPECT("PQ window into RGBA16Float: linear",ROUTE_PQ_LINEAR,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatRGBA16Float));
    {   // vi's answer back to SDR (Game's HDR, then BT.2446 or Off): a 10-bit window is 8-bit values again. t_present_vi, which present_now sets from the frame's
        // pq_item.vi_hdr, wins over the last answer (the path from the game's queue present to the route: main, "real QueuePresent" HDR case)
        atomic_store(&g_hdr_vi_on,0);
        EXPECT("vi reports SDR again: a 10-bit window into BGRA8 is 8-bit values",ROUTE_COPY,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatBGRA8Unorm));
        EXPECT("  into RGBA16Float: the inverse tone map",ROUTE_ITM,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatRGBA16Float));
        t_present_vi=1;EXPECT("  t_present_vi 1 (the frame's answer was HDR): PQ",ROUTE_PQ_LINEAR,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatRGBA16Float));
        atomic_store(&g_hdr_vi_on,1);t_present_vi=0;
        EXPECT("vi reports HDR, t_present_vi 0 (the frame's answer was SDR): 8-bit values",ROUTE_COPY,present_route(MTLPixelFormatRGB10A2Unorm,MTLPixelFormatBGRA8Unorm));
        t_present_vi=-1;
    }
    // PQ: the paper white vi reports (203 nits, code 594) is 1.0 on the linear route and the SDR route's white
    hdr_present(pqw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("PQ 594 on the linear route: 1.0 (paper white)",0.99958,v[0],0.002);EXPECTF("  green",0.99958,v[1],0.002);
    hdr_present(pqw,MTLPixelFormatBGRA8Unorm,4,v);EXPECTF("PQ 594 on the SDR route: white",255,v[0],1);EXPECTF("  blue",255,v[2],1);
    // BT.2020 primaries: a BT.709 colour stored in BT.2020 comes back as itself on both routes: the linear route writes BT.709 values too, which the layer (the screen's own colour
    // space, as for the inverse tone map) shows in the panel's primaries, the look of the SDR present. The codes' own rounding gives (0.5005, 0.2504, 0.1244)
    uint32_t c2020=rgb10a2(499,458,398);id<MTLTexture> cw=hdr_src(MTLPixelFormatRGB10A2Unorm,4,4,&c2020,4);
    hdr_present(cw,MTLPixelFormatBGRA8Unorm,4,v);EXPECTF("BT.2020 (0.401, 0.266, 0.142) on the SDR route: BT.709 red code",187.6,v[0],1.5);EXPECTF("  green",137.1,v[1],1.5);EXPECTF("  blue",98.9,v[2],1.5);
    hdr_present(cw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("the same on the linear route: BT.709 red",0.5005,v[0],0.004);EXPECTF("  green",0.2504,v[1],0.003);EXPECTF("  blue",0.1244,v[2],0.002);
    {   // DM_CTL "shot" and regression shots of a PQ window: drawn through the SDR route into an 8-bit picture
        NSString *path=[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_present_crop_pq_%d.png",getpid()]];
        uint32_t px=rgb10a2(499,458,398);id<MTLTexture> sw=hdr_src(MTLPixelFormatRGB10A2Unorm,8,8,&px,4);
        shot(sw,path.UTF8String,1,NULL);
        CGImageSourceRef is=CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],NULL);CGImageRef img=is?CGImageSourceCreateImageAtIndex(is,0,NULL):NULL;
        EXPECT("a screenshot of a PQ window is written",1,img!=NULL);
        if(img){uint8_t o[4]={0};CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);CGContextRef cx=CGBitmapContextCreate(o,1,1,8,4,cs,kCGImageAlphaNoneSkipLast);
            CGContextDrawImage(cx,CGRectMake(-4,-4,8,8),img);EXPECT("  as SDR: BT.709 (0.5, 0.25, 0.125) as sRGB codes (188, 137, 99)",1,abs(o[0]-188)<=2&&abs(o[1]-137)<=2&&abs(o[2]-99)<=2);
            printf("  shot pixel %d %d %d\n",o[0],o[1],o[2]);CGContextRelease(cx);CGColorSpaceRelease(cs);CGImageRelease(img);}
        if(is)CFRelease(is);unlink(path.UTF8String);res_remove(sw);
    }
    uint32_t red=rgb10a2(594,0,0);id<MTLTexture> rw=hdr_src(MTLPixelFormatRGB10A2Unorm,4,4,&red,4);
    hdr_present(rw,MTLPixelFormatBGRA8Unorm,4,v);EXPECT("BT.2020 red outside BT.709 on the SDR route: clipped to red",1,v[0]>=254&&v[1]<=1&&v[2]<=1);
    hdr_present(rw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("  on the linear route: BT.709 red 1.66, above SDR white",1.6598,v[0],0.006);EXPECT("  green and blue clipped at 0",1,v[1]==0&&v[2]==0);
    // The inverse tone map of an 8-bit window: exact greys and white from a 16-bit source (the same route), then an 8-bit window. Output in SDR-white units (EDR, 1.0 = 203 nits)
    uint16_t grey16[4]={30235,30235,30235,65535},white16[4]={65535,65535,65535,65535};   // sRGB 0.461356: linear 0.18
    id<MTLTexture> gw=hdr_src(MTLPixelFormatRGBA16Unorm,4,4,grey16,8),ww=hdr_src(MTLPixelFormatRGBA16Unorm,4,4,white16,8);
    const struct { double headroom,grey,white; } itm[]={{16,0.87218,16},{15,0.83317,15},{8,0.53161,8},{5,0.37815,5},{4.9,0.37262,4.90001},{4,0.32120,4},{3,0.25986,3},{2,0.19336,2},
                                                       {1.5,0.18,1.5},{1.0,0.18,1.0}};   // white at the headroom
    for(unsigned i=0;i<sizeof itm/sizeof *itm;i++){char n[112];g_hdr_test_peak=itm[i].headroom;
        hdr_present(gw,MTLPixelFormatRGBA16Float,4,v);snprintf(n,sizeof n,"inverse tone map: mid grey 0.18 at a headroom of %.1f",itm[i].headroom);EXPECTF(n,itm[i].grey,v[0],itm[i].grey*0.004);
        hdr_present(ww,MTLPixelFormatRGBA16Float,4,v);snprintf(n,sizeof n,"  white at a headroom of %.1f",itm[i].headroom);EXPECTF(n,itm[i].white,v[1],itm[i].white*0.004);}
    // L_HDR at 710, 1000 (Method A's design point) and 2000 nits: the headroom that gives it, L_HDR / 203, white at that headroom
    g_hdr_test_peak=710.0/203;hdr_present(gw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("L_HDR 710 (a headroom of 3.50): mid grey",0.29102,v[0],0.0012);
    hdr_present(ww,MTLPixelFormatRGBA16Float,4,v);EXPECTF("  white",3.49754,v[1],0.014);
    g_hdr_test_peak=1000.0/203;hdr_present(gw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("L_HDR 1000 (Method A's design point, a headroom of 4.93): mid grey",0.37407,v[0],0.0015);
    hdr_present(ww,MTLPixelFormatRGBA16Float,4,v);EXPECTF("  white",4.92611,v[1],0.02);
    g_hdr_test_peak=2000.0/203;hdr_present(gw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("L_HDR 2000 (a headroom of 9.85): mid grey",0.61741,v[0],0.0025);
    hdr_present(ww,MTLPixelFormatRGBA16Float,4,v);EXPECTF("  white",9.85223,v[1],0.04);
    {   // other greys at L_HDR 1000 (the design point) and at a headroom of 15, never below the SDR picture (each Y as its own 16-bit sRGB code)
        const struct { uint16_t code; double lin,design,h15; } ys[]={{6544,0.01,0.01387,0.02378},{16240,0.050002,0.08385,0.16243},{44081,0.41,1.01302,2.52156},{55987,0.700002,2.27328,6.25074}};
        for(unsigned i=0;i<sizeof ys/sizeof *ys;i++){uint16_t px[4]={ys[i].code,ys[i].code,ys[i].code,65535};id<MTLTexture> t=hdr_src(MTLPixelFormatRGBA16Unorm,4,4,px,8);char n[96];
            g_hdr_test_peak=1000.0/203;hdr_present(t,MTLPixelFormatRGBA16Float,4,v);snprintf(n,sizeof n,"inverse tone map at the design point: Y %.2f",ys[i].lin);EXPECTF(n,ys[i].design,v[0],ys[i].design*0.004);
            g_hdr_test_peak=15;hdr_present(t,MTLPixelFormatRGBA16Float,4,v);snprintf(n,sizeof n,"  at a headroom of 15: Y %.2f",ys[i].lin);EXPECTF(n,ys[i].h15,v[0],ys[i].h15*0.004);res_remove(t);}
    }
    {   // a headroom of 60: L_HDR stops at PQ's 10000 nits (white 49.26 x SDR white), and a saturated primary stays finite through the PQ-domain ratio
        g_hdr_test_peak=60;hdr_present(gw,MTLPixelFormatRGBA16Float,4,v);EXPECTF("inverse tone map at a headroom of 60 (L_HDR 10000): mid grey",1.91653,v[0],1.91653*0.004);
        hdr_present(ww,MTLPixelFormatRGBA16Float,4,v);EXPECTF("  white",49.26126,v[1],49.26126*0.004);
        uint16_t px[4]={0,0,65535,65535};id<MTLTexture> t=hdr_src(MTLPixelFormatRGBA16Unorm,4,4,px,8);hdr_present(t,MTLPixelFormatRGBA16Float,4,v);res_remove(t);
        EXPECT("  BT.709 blue: every channel finite",1,isfinite(v[0])&&isfinite(v[1])&&isfinite(v[2]));
        EXPECTF("  blue's red",0.6998,v[0],0.006);EXPECT("  blue's green clipped at 0",1,v[1]==0);EXPECTF("  blue's blue",18.1979,v[2],18.1979*0.004);
    }
    {   // no crease where SDR and Method A cross: at a headroom of 1.25 they cross at sRGB 230.8; the hard maximum's slope went from 1.05 to 1.79 over 4 codes either side,
        // the smooth maximum's goes from 1.34 to 1.49, and the crossing itself is lifted 3 % above both curves (0.82268 against 0.79917)
        const uint16_t codes[3]={58288,59316,60344};const double ys[3]={0.766634,0.79755,0.829182},want[3]={0.78114,0.82268,0.86974};double o[3];g_hdr_test_peak=1.25;
        for(int i=0;i<3;i++){uint16_t px[4]={codes[i],codes[i],codes[i],65535};id<MTLTexture> t=hdr_src(MTLPixelFormatRGBA16Unorm,4,4,px,8);hdr_present(t,MTLPixelFormatRGBA16Float,4,v);o[i]=v[0];res_remove(t);
            char n[96];snprintf(n,sizeof n,"smooth maximum at a headroom of 1.25: Y %.4f",ys[i]);EXPECTF(n,want[i],o[i],0.0015);}
        double s1=(o[1]-o[0])/(ys[1]-ys[0]),s2=(o[2]-o[1])/(ys[2]-ys[1]);printf("  slopes %.4f -> %.4f\n",s1,s2);
        EXPECT("  its slope changes by under 0.3 across the crossing (the hard maximum: 0.73), x 1000",1,fabs(s2-s1)<0.3);
    }
    {   // one PQ-domain ratio on I, Ct and Cp keeps the ICtCp hue angle and Ct / I: BT.709 (0.5, 0.25, 0.125) at the design point. In linear light the channels do not scale
        // alike (2.40, 2.24, 2.06; 6.02, 5.15, 4.31 at a headroom of 15): a saturated highlight gains saturation, and a strong primary can leave the gamut and clip at 0
        uint16_t px[4]={48192,35199,25465,65535};id<MTLTexture> t=hdr_src(MTLPixelFormatRGBA16Unorm,4,4,px,8);
        g_hdr_test_peak=1000.0/203;hdr_present(t,MTLPixelFormatRGBA16Float,4,v);
        EXPECTF("inverse tone map of BT.709 (0.5, 0.25, 0.125) at the design point: red",1.2015,v[0],0.006);EXPECTF("  green",0.5590,v[1],0.003);EXPECTF("  blue",0.2579,v[2],0.0015);
        g_hdr_test_peak=15;hdr_present(t,MTLPixelFormatRGBA16Float,4,v);
        EXPECTF("  at a headroom of 15: red",3.0117,v[0],0.012);EXPECTF("  green",1.2866,v[1],0.006);EXPECTF("  blue",0.5381,v[2],0.003);res_remove(t);
    }
    {   // continuous in the headroom and never darker than SDR: mid grey from a headroom of 1 to 6 in steps of 0.05 rises in steps under 0.01 and never below 0.18
        double last=-1,worst=0,lowest=1;
        for(int k=0;k<=100;k++){g_hdr_test_peak=1+k*0.05;hdr_present(gw,MTLPixelFormatRGBA16Float,4,v);if(last>=0)worst=fmax(worst,v[0]-last<0?1:v[0]-last);last=v[0];lowest=fmin(lowest,v[0]);}
        EXPECT("inverse tone map: mid grey rises with the headroom in steps under 0.01, x 1000",1,worst<0.01);printf("  largest step %.5f\n",worst);
        EXPECTF("  and never below the SDR picture's 0.18",0.18,fmin(lowest,0.18),0.00015);
    }
    uint8_t g8[4]={118,118,118,255};id<MTLTexture> g8w=hdr_src(MTLPixelFormatRGBA8Unorm,4,4,g8,4);
    g_hdr_test_peak=4;hdr_present(g8w,MTLPixelFormatRGBA16Float,4,v);EXPECTF("inverse tone map of an 8-bit window: code 118 at a headroom of 4",0.32363,v[0],0.0013);
    // The pipelines: a capture into BGRA8 between presents into RGBA16Float makes no pipeline again
    hdr_present(g8w,MTLPixelFormatBGRA8Unorm,4,v);hdr_present(pqw,MTLPixelFormatBGRA8Unorm,4,v);hdr_present(pqw,MTLPixelFormatRGBA16Float,4,v);
    uint64_t made=g_present_pso_made;
    for(int k=0;k<4;k++){hdr_present(g8w,MTLPixelFormatRGBA16Float,4,v);hdr_present(g8w,MTLPixelFormatBGRA8Unorm,4,v);hdr_present(pqw,MTLPixelFormatBGRA8Unorm,4,v);hdr_present(pqw,MTLPixelFormatRGBA16Float,4,v);}
    EXPECT("alternating BGRA8 captures and RGBA16Float presents of both routes make no pipeline again",0,(int)(g_present_pso_made-made));
    // MetalFX: the route comes from the window's format, not from the scaler's output; MetalFX never writes an HDR target directly
    if([MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) {
        g_fx_mode=1;id<MTLTexture> pqb=hdr_src(MTLPixelFormatRGB10A2Unorm,64,64,&pq594,4),g8b=hdr_src(MTLPixelFormatRGBA8Unorm,64,64,g8,4);
        int k=hdr_present(pqb,MTLPixelFormatRGBA16Float,128,v);printf("  MetalFX of a PQ window: kind %d (%s)\n",k,k==PRESENT_METALFX?"MetalFX":"standard: the scaler refused the format");
        // the scaler moves a flat PQ value by about 0.1 % of its code (0.989 measured on an M5 Max), 1 % in linear; the inverse tone map of the PQ code would give about 0.3
        EXPECTF("PQ window upscaled into RGBA16Float: still the linear route, paper white 1.0",0.99958,v[1],0.02);
        k=hdr_present(g8b,MTLPixelFormatRGBA16Float,128,v);EXPECT("8-bit window upscaled into RGBA16Float goes through MetalFX",PRESENT_METALFX,k);
        EXPECTF("  and then the inverse tone map, not a direct write of its bytes",0.32363,v[0],0.004);
        k=hdr_present(pqb,MTLPixelFormatBGRA8Unorm,128,v);EXPECTF("PQ window upscaled into BGRA8: the SDR route's white",255,v[0],2);
        g_fx_mode=0;res_remove(pqb);res_remove(g8b);
    }
    g_hdr_test_peak=0;
    // The screen's headroom as vi and the inverse tone map take it: capability debounced by a second, the live headroom falling at once and rising over half a second, 5 % steps
    pthread_mutex_lock(&g_hdr_mu);
    uint64_t t0=now_ns();g_edr_pushed=0;g_hdr_cap=-1;g_hdr_s_at=0;
    EXPECT("no front end yet: no EDR headroom",0,hdr_capable_locked(t0));
    g_edr_pot=1;g_edr_live=1;g_edr_pushed=1;EXPECT("an SDR screen: none",0,hdr_capable_locked(t0));
    g_edr_pot=16;g_edr_live=4;EXPECT("an EDR screen half a second later: still none",0,hdr_capable_locked(t0+500000000ull));
    EXPECT("a second after the change: headroom",1,hdr_capable_locked(t0+1500000000ull));
    g_edr_pot=1;EXPECT("a moment without it: still on",1,hdr_capable_locked(t0+1600000000ull));g_edr_pot=16;EXPECT("back: on",1,hdr_capable_locked(t0+1700000000ull));
    EXPECT("no change counted from the moment",1,hdr_capable_locked(t0+3000000000ull));
    EXPECTF("live headroom 4: taken in 5 % steps, 1.05^28",pow(1.05,28),hdr_headroom_locked(t0),1e-9);
    g_edr_live=2;EXPECTF("live headroom 2: falls at once, 1.05^14",pow(1.05,14),hdr_headroom_locked(t0+1000000ull),1e-9);
    g_edr_live=4;EXPECTF("back to 4: after 0.1 s part of the way, 1.05^23",pow(1.05,23),hdr_headroom_locked(t0+101000000ull),1e-9);
    EXPECTF("after 0.6 s all of it",pow(1.05,28),hdr_headroom_locked(t0+601000000ull),1e-9);
    g_edr_live=0.5f;EXPECTF("below 1.0: 1.0",1,hdr_headroom_locked(t0+602000000ull),1e-9);
    g_edr_live=4;g_hdr_s_at=0;hdr_headroom_locked(t0);g_hdr_s_at=0;   // at 4 again, and the next real-time call takes the clock afresh
    pthread_mutex_unlock(&g_hdr_mu);
    uint16_t peak=0;
    unsetenv("DM_HDR");EXPECT("DM_HDR unset: vi reports SDR",0,mtl_hdr_vi(&peak));EXPECT("  and the present takes it",0,atomic_load(&g_hdr_vi_on));
    setenv("DM_HDR","2",1);
    EXPECT("DM_HDR=2 (Game's HDR) on an EDR screen: vi reports HDR",1,mtl_hdr_vi(&peak));EXPECT("  peak: headroom 1.05^28 x 203 nits",(int)round(pow(1.05,28)*203),peak);
    EXPECT("  and the present takes it",1,atomic_load(&g_hdr_vi_on));
    g_hdr_test_peak=0;EXPECTF("  the inverse tone map's L_HDR: the headroom x 203",pow(1.05,28)*203,hdr_itm_lhdr(),1e-6);
    g_hdr_test_peak=15;EXPECTF("  at a headroom of 15: 3045",3045,hdr_itm_lhdr(),1e-6);
    g_hdr_test_peak=60;EXPECTF("  at a headroom of 60: PQ's 10000",10000,hdr_itm_lhdr(),1e-6);g_hdr_test_peak=0;
    setenv("DM_HDR","1",1);EXPECT("DM_HDR=1 (BT.2446 for every game): vi reports SDR",0,mtl_hdr_vi(&peak));EXPECT("  and the present follows it",0,atomic_load(&g_hdr_vi_on));
    setenv("DM_HDR","3",1);EXPECT("DM_HDR=3 is off",0,hdr_mode());setenv("DM_HDR","2",1);
    pthread_mutex_lock(&g_hdr_mu);g_edr_pot=1;g_edr_live=1;g_hdr_cap=0;g_hdr_cap_raw=0;g_hdr_s_at=0;pthread_mutex_unlock(&g_hdr_mu);
    EXPECT("DM_HDR=2 on an SDR screen: vi reports SDR",0,mtl_hdr_vi(&peak));
    // The layer the present asks the front end for: one HDR layer (the screen's own colour space) for both HDR routes, once it has been wanted at two presents in a row
    int calls=g_layer_hdr_calls;
    hdr_layer_update(MTLPixelFormatRGBA8Unorm);hdr_layer_update(MTLPixelFormatRGBA8Unorm);EXPECT("an SDR screen: no new layer",calls,g_layer_hdr_calls);
    pthread_mutex_lock(&g_hdr_mu);g_edr_pot=16;g_edr_live=4;g_hdr_cap=1;g_hdr_cap_raw=1;pthread_mutex_unlock(&g_hdr_mu);
    hdr_layer_update(MTLPixelFormatRGBA8Unorm);EXPECT("an EDR screen, one present: not yet",calls,g_layer_hdr_calls);
    hdr_layer_update(MTLPixelFormatRGBA8Unorm);EXPECT("two presents of an 8-bit window: the HDR layer",HDR_LAYER_HDR,g_layer_hdr);
    hdr_layer_update(MTLPixelFormatRGB10A2Unorm);hdr_layer_update(MTLPixelFormatRGB10A2Unorm);EXPECT("a PQ window: the same layer, no new request",calls+1,g_layer_hdr_calls);
    setenv("DM_HDR","1",1);hdr_layer_update(MTLPixelFormatRGBA8Unorm);hdr_layer_update(MTLPixelFormatRGBA8Unorm);EXPECT("DM_HDR=1 (BT.2446): the same HDR layer",calls+1,g_layer_hdr_calls);
    EXPECT("  still asked as HDR",HDR_LAYER_HDR,g_layer_hdr);
    unsetenv("DM_HDR");hdr_layer_update(MTLPixelFormatRGB10A2Unorm);hdr_layer_update(MTLPixelFormatRGB10A2Unorm);EXPECT("DM_HDR off: the SDR layer again",HDR_LAYER_SDR,g_layer_hdr);
    // The modes a menu offers (the Mac's Window > HDR): Game's HDR for a game without HDR runs as BT.2446; a screen without EDR greys out both HDR modes and presents SDR,
    // the chosen mode kept; the cycling shortcut steps through the allowed ones
    EXPECT("Game's HDR asked for, a game without HDR: BT.2446 shown and run",1,mtl_hdr_mode_shown(2,0));EXPECT("Game's HDR, a game with HDR: Game's HDR",2,mtl_hdr_mode_shown(2,1));
    EXPECT("BT.2446: shown as itself",1,mtl_hdr_mode_shown(1,1));EXPECT("  for a game without HDR too",1,mtl_hdr_mode_shown(1,0));EXPECT("Off: Off",0,mtl_hdr_mode_shown(0,0));
    EXPECT("Game's HDR allowed: a game with HDR on an EDR screen",1,mtl_hdr_mode_allowed(2,1,1));EXPECT("  not for a game without HDR",0,mtl_hdr_mode_allowed(2,0,1));
    EXPECT("BT.2446 allowed for a game without HDR",1,mtl_hdr_mode_allowed(1,0,1));
    EXPECT("  BT.2446 not on a screen without EDR",0,mtl_hdr_mode_allowed(1,1,0));EXPECT("  Off always",1,mtl_hdr_mode_allowed(0,0,0));
    EXPECT("cycle Off -> BT.2446",1,mtl_hdr_mode_next(0,1,1));EXPECT("cycle BT.2446 -> Game's HDR",2,mtl_hdr_mode_next(1,1,1));EXPECT("cycle Game's HDR -> Off",0,mtl_hdr_mode_next(2,1,1));
    EXPECT("cycle without HDR in the game: Off -> BT.2446",1,mtl_hdr_mode_next(0,0,1));EXPECT("  and back to Off",0,mtl_hdr_mode_next(1,0,1));
    EXPECT("cycle on a screen without EDR: not offered, Off stays",0,mtl_hdr_mode_next(0,1,0));EXPECT("  and Game's HDR is kept",2,mtl_hdr_mode_next(2,1,0));
    EXPECT("  and BT.2446 is kept",1,mtl_hdr_mode_next(1,0,0));
    atomic_store(&g_hdr_game_native,1);EXPECT("the game's HDR support as decided at load",1,mtl_hdr_game_native());
    pthread_mutex_lock(&g_hdr_mu);g_edr_pot=1;g_edr_live=1;g_hdr_cap=0;g_hdr_cap_raw=0;pthread_mutex_unlock(&g_hdr_mu);
    setenv("DM_HDR","2",1);hdr_layer_update(MTLPixelFormatRGB10A2Unorm);hdr_layer_update(MTLPixelFormatRGB10A2Unorm);
    EXPECT("Game's HDR on a screen without EDR: the SDR layer",HDR_LAYER_SDR,g_layer_hdr);EXPECT("  the mode kept",2,mtl_hdr_requested());EXPECT("  vi reports SDR",0,mtl_hdr_vi(&peak));
    pthread_mutex_lock(&g_hdr_mu);g_edr_pot=16;g_edr_live=4;g_hdr_cap=1;g_hdr_cap_raw=1;pthread_mutex_unlock(&g_hdr_mu);
    hdr_layer_update(MTLPixelFormatRGB10A2Unorm);hdr_layer_update(MTLPixelFormatRGB10A2Unorm);EXPECT("  back on an EDR screen: the HDR layer again",HDR_LAYER_HDR,g_layer_hdr);
    unsetenv("DM_HDR");hdr_layer_update(MTLPixelFormatRGBA8Unorm);hdr_layer_update(MTLPixelFormatRGBA8Unorm);atomic_store(&g_hdr_game_native,-1);
    atomic_store(&g_hdr_vi_on,0);res_remove(pqw);res_remove(cw);res_remove(rw);res_remove(gw);res_remove(ww);res_remove(g8w);
}
#endif

static void check(id<MTLTexture> src, const char *name, const int crop[4], int w, int h, int color) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> dst=[g_dev newTextureWithDescriptor:d];id<MTLCommandBuffer> cb=[g_q commandBuffer];
    EXPECT("encode succeeds",1,ENCODE(cb,src,dst,crop));[cb commit];[cb waitUntilCompleted];
    EXPECT("GPU done",MTLCommandBufferStatusCompleted,cb.status);
    uint8_t *out=malloc(w*h*4);[dst getBytes:out bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    int bad=0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        int q=color;
        if(q==4) { // square valid area onto a 2:1 target: 4 black columns on each side, the middle 8 columns all red.
            static const uint8_t black[4]={0,0,0,255};
            const uint8_t *want=x<4||x>=12?black:colors[0];bad+=memcmp(out+4*(y*w+x),want,4)!=0;continue;
        }
        if(q==-2) { // crop=(1,1,4,4): 3×3 red, last column green, last row blue, bottom-right 1 cell yellow.
            q=(x+1>=4)+2*(y+1>=4);
        } else if(q<0) { // same-size default whole frame: the boundary is dead center.
            q=(x>=w/2)+2*(y>=h/2);
        }
        bad+=memcmp(out+4*(y*w+x),colors[q],4)!=0;
    }
    EXPECT(name,0,bad);free(out);
}

// This fixture is the only present producer. A window now holds a logical queue receipt;
// wait for replay, then for the display ticket (snapshots do not return that ticket).
static uint64_t display_ticket(uint64_t receipt, uint64_t before) {
    EXPECT("present returned a logical receipt",1,receipt!=0);
    if (!receipt) return 0;
    nvn_queue_present_wait(receipt);
    uint64_t queued, done; mtl_present_progress(&queued,&done);
    EXPECT("one frame reached the display queue",1,queued==before+1);
    if (queued!=before+1) return 0;
    mtl_present_wait(queued);
    return queued;
}

#ifndef CROP_LEGACY_HEADER
static void check_external_png(const char *path) {
    NSURL *url = [NSURL fileURLWithPath:@(path)];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, NULL);
    CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
    int green = 0;
    if (image) {
        uint8_t pixels[4 * 4 * 4];
        CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(pixels, 4, 4, 8, 16, color, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
        if (ctx) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, 4, 4), image);
            for (int i = 0; i < 16; i++)
                green += pixels[4*i] == 0 && pixels[4*i+1] == 255 && pixels[4*i+2] == 0;
            CGContextRelease(ctx);
        }
        CGColorSpaceRelease(color);
        CGImageRelease(image);
    }
    if (source) CFRelease(source);
    EXPECT(path, 16, green);
}

static void check_external_screenshots(void) {
    char dir[] = "/tmp/nxnative-external-shot.XXXXXX";
    if (!mkdtemp(dir)) { EXPECT("screenshot test directory", 1, 0); return; }
    char scheduled[sizeof dir + 16], queued[sizeof dir + 16], spec[sizeof scheduled + 3];
    snprintf(scheduled, sizeof scheduled, "%s/scheduled.png", dir);
    snprintf(queued, sizeof queued, "%s/queued.png", dir);
    snprintf(spec, sizeof spec, "1:%s", scheduled);
    setenv("DM_NVN_SHOT", spec, 1);
    unsetenv("DM_NVN_SHOT_OUT");

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td];
    const uint8_t black[4 * 4 * 4] = {0};
    [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:black bytesPerRow:16];

    id<MTLSharedEvent> ready = [g_dev newSharedEvent];
    assert(ready);
    id<MTLCommandQueue> producer = [g_dev newCommandQueue];
    id<MTLCommandBuffer> cb = [producer commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = tex;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
    [cb encodeSignalEvent:ready value:1];
    // Leave the producer unsubmitted while the external present first reaches readback.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{ [cb commit]; });
    uint64_t serial = mtl_ext_tex((__bridge void *)tex);
    dm_gfx_shot_at(1, queued);
    uint64_t ticket = mtl_present_ext(serial, 1.0 / 60, (__bridge void *)ready, 1, NULL);
    EXPECT("external present returns a ticket", 1, ticket != 0);
    if (ticket) mtl_present_wait(ticket);
    check_external_png(scheduled);
    check_external_png(queued);
    EXPECT("producer completes", 1, [ready waitUntilSignaledValue:1 timeoutMS:5000]);
    mtl_ext_tex_release(serial);
    unlink(scheduled);
    unlink(queued);
    rmdir(dir);
}
#endif

int main(int argc, char **argv) { @autoreleasepool {
    mtl_init();
#ifndef CROP_LEGACY_HEADER
    if (argc == 2 && !strcmp(argv[1], "--external-screenshots")) {
        check_external_screenshots();
        return failed != 0;
    }
    mtl_present_layer(1,1);   // square fixtures: a sized square layer, so the checks below see the crop's texels and the bars of that shape; the display's own shape is checked at the end
#endif
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> src=[g_dev newTextureWithDescriptor:d];uint8_t px[8*8*4];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)memcpy(px+4*(y*8+x),colors[(x>=4)+2*(y>=4)],4);
    [src replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:px bytesPerRow:32];
    int zero[4]={0},tl[4]={0,0,4,4},tr[4]={4,0,4,4},bl[4]={0,4,4,4},br[4]={4,4,4,4},one[4]={3,3,1,1},inset[4]={1,1,4,4};
    check(src,"default whole frame 64 pixels",zero,8,8,-1);
    check(src,"NULL default whole frame 64 pixels",NULL,8,8,-1);
    check(src,"top-left crop scaled up, 256 pixels all red",tl,16,16,0);
    check(src,"non-zero x crop scaled up, 256 pixels all green",tr,16,16,1);
    check(src,"non-zero y crop scaled up, 256 pixels all blue",bl,16,16,2);
    check(src,"bottom-right crop at the edge, 256 pixels all yellow",br,16,16,3);
    check(src,"single texel scaled up without crossing edges, 256 pixels all red",one,16,16,0);
    check(src,"non-zero x and y offset, 16 pixels byte-identical to a hand-written sub-rectangle",inset,4,4,-2);
    check(src,"aspect of the valid area kept, 128 pixels red in the middle / black on both sides",tl,16,8,4);
#ifndef CROP_LEGACY_HEADER
    {
        // sRGB swapchain: present sends the raw bytes (as the Switch's scan-out does) without decoding first; unfixed, 128/64/200 decode to about 55/13/148
        MTLTextureDescriptor *sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:8 height:8 mipmapped:NO];sd.storageMode=MTLStorageModeShared;sd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> s8=[g_dev newTextureWithDescriptor:sd];uint8_t g[8*8*4];
        for(int i=0;i<64;i++){g[4*i]=128;g[4*i+1]=64;g[4*i+2]=200;g[4*i+3]=255;}
        [s8 replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:g bytesPerRow:32];
        MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:od];id<MTLCommandBuffer> cb=[g_q commandBuffer];
        EXPECT("sRGB source encode succeeds",1,ENCODE(cb,s8,dst,zero));[cb commit];[cb waitUntilCompleted];
        uint8_t o[8*8*4];[dst getBytes:o bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
        int bad=0;for(int i=0;i<64;i++)bad+=abs(o[4*i]-128)>1||abs(o[4*i+1]-64)>1||abs(o[4*i+2]-200)>1;
        EXPECT("sRGB swapchain present sends raw bytes, 64 pixels",0,bad);
    }
#endif
#ifndef CROP_LEGACY_HEADER
    {   // DM_PRESENT_UPSCALE: which content MetalFX scales, and to which size (the picture's rectangle on the canvas)
        const struct { unsigned cw,ch,dw,dh,w,h; int upscale; } sizes[] = {
            {1280,720,2868,1320,2347,1320,1},   // handheld, letterboxed on an iPhone
            {1952,900,2868,1320,2868,1320,1},   // ratios within 1 % fill the canvas
            {1280,720,2560,1440,2560,1440,1},   // Retina window
            {1280,720,1280,720,1280,720,0},     // native size
            {1600,900,1280,720,1280,720,0},     // downscale
            {8,8,16,8,8,8,0},                  // larger canvas, native-size picture
        };
        for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
            MTLViewport vp=present_viewport(sizes[i].cw,sizes[i].ch,sizes[i].dw,sizes[i].dh);
            EXPECT("MetalFX only upscales the picture",sizes[i].upscale,fx_wanted(vp,sizes[i].cw,sizes[i].ch));
            EXPECT("picture width",sizes[i].w,(int)vp.width);EXPECT("picture height",sizes[i].h,(int)vp.height);
        }
        EXPECT("empty content is not upscaled",0,fx_wanted((MTLViewport){0,0,2868,1320,0,1},0,720));
    }
    if([MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) {   // the present pass through MetalFX into an offscreen canvas (present_offscreen_m4, as DM_NVN_SHOT_OUT uses it)
        g_fx_mode=1;
        const int N=64;MTLTextureDescriptor *bd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:N height:N mipmapped:NO];bd.storageMode=MTLStorageModeShared;bd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> big=[g_dev newTextureWithDescriptor:bd];uint8_t *bp=malloc(N*N*4);
        for(int y=0;y<N;y++)for(int x=0;x<N;x++)memcpy(bp+4*(y*N+x),colors[(x>=N/2)+2*(y>=N/2)],4);
        for(int y=N/2;y<N;y++)for(int x=0;x<N/2;x++){uint8_t g[4]={128,64,200,255};memcpy(bp+4*(y*N+x),g,4);}   // lower-left quadrant: a mid grey-violet the sRGB bytes must keep
        [big replaceRegion:MTLRegionMake2D(0,0,N,N) mipmapLevel:0 withBytes:bp bytesPerRow:N*4];free(bp);
        res_add(big);res_commit();
        const int W=4*N,H=2*N;MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        od.storageMode=MTLStorageModePrivate;od.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;id<MTLTexture> out=[g_dev newTextureWithDescriptor:od];
        const int crops[4][4]={{0},{8,8,48,48},{8,8,48,48},{8,8,48,48}};
        for (int mode=0;mode<4;mode++) {
            g_fx_mode=mode==3?0:1;atomic_store(&g_origin_ll,mode==2);
            EXPECT("offscreen present reports the scaler actually used",mode==3?PRESENT_STANDARD:PRESENT_METALFX,present_offscreen_wait(big,crops[mode],out));
            id<MTLBuffer> rb=[g_dev newBufferWithLength:W*H*4 options:MTLResourceStorageModeShared];id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
            [bl copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:W*4 destinationBytesPerImage:W*H*4];
            [bl endEncoding];[cb commit];[cb waitUntilCompleted];
            const uint8_t *o=rb.contents;   // BGRA
        #define PX(x,y,c) o[4*((y)*W+(x))+(c)]
            int bars=0;for(int y=0;y<H;y++)for(int x=0;x<W;x++)if(x<64||x>=192)bars+=PX(x,y,0)||PX(x,y,1)||PX(x,y,2);
            EXPECT("  black bars beside the square picture, 0 lit pixels",0,bars);
            int bad=0;const int cxs[2]={64+32,64+96},cys[2]={32,96};
            for(int q=0;q<4;q++){int x=cxs[q&1],y=cys[q>>1],sq=mode==2?q^2:q;const uint8_t *want=sq==2?(const uint8_t[4]){128,64,200,255}:colors[sq];
                bad+=abs(PX(x,y,2)-want[0])>3||abs(PX(x,y,1)-want[1])>3||abs(PX(x,y,0)-want[2])>3;}
            EXPECT("  quadrant centres keep their colours, raw bytes of the sRGB source (4 checked)",0,bad);
        #undef PX
        }
        atomic_store(&g_origin_ll,0);g_fx_mode=1;
        MTLTextureDescriptor *smallD=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:32 mipmapped:NO];
        smallD.storageMode=MTLStorageModePrivate;smallD.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> small=[g_dev newTextureWithDescriptor:smallD];int whole[4]={0},invalidCrop[4]={63,0,2,2};
        EXPECT("MetalFX requested but downscaling uses standard",PRESENT_STANDARD,present_offscreen_wait(big,whole,small));
        EXPECT("offscreen rejects an invalid crop before submission",PRESENT_FAILED,present_offscreen_wait(big,invalidCrop,out));

        uint64_t savedPresent=g_present_n;g_present_n=1;
        EXPECT("initial present makes its scaler",PRESENT_METALFX,check_present_resize(big,W,H));
        id<MTL4FXSpatialScaler> first=g_fx_present.sc;
        g_present_n=2;
        EXPECT("resizing within 30 presents falls back to standard",PRESENT_STANDARD,check_present_resize(big,192,96));
        EXPECT("resizing keeps the previous scaler until replacement",1,g_fx_present.sc==first);
        g_present_n=31;
        EXPECT("stable new size makes its scaler",PRESENT_METALFX,check_present_resize(big,192,96));
        EXPECT("replacement has the new picture size",96,(int)g_fx_present.sc.outputWidth);
        EXPECT("exactly two scalers made",2,g_fx_present.made);
        g_present_n=32;
        EXPECT("same size reuses its scaler",PRESENT_METALFX,check_present_resize(big,192,96));
        EXPECT("reuse makes no new scaler",2,g_fx_present.made);
        // DM_FX_INPLACE and DM_FX_DIRECT: the scaler reads a private window texture in place and writes a canvas it can write, while the frame that draws that texture is
        // still running on the queue (committed just before, as a present follows its frame). The scaler must still see the whole frame: the encoder before it orders it after
        // the queue's earlier work only around work of its own (an encoder of barriers alone ordered nothing, and the scaler read the frame before, in every round)
        {
            const int S=256,ROUNDS=8;
            MTLTextureDescriptor *wd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:S height:S mipmapped:NO];
            wd.storageMode=MTLStorageModePrivate;wd.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;id<MTLTexture> win=[g_dev newTextureWithDescriptor:wd];
            MTLTextureDescriptor *cd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:2*S height:2*S mipmapped:NO];
            cd.storageMode=MTLStorageModePrivate;cd.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;id<MTLTexture> canvas=[g_dev newTextureWithDescriptor:cd];
            id<MTLBuffer> args=[g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared],rb=[g_dev newBufferWithLength:4*S*S*4 options:MTLResourceStorageModeShared];
            res_add(win);res_add(canvas);res_add(args);res_commit();
            NSError *err=nil;id<MTL4Compiler> comp=[g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
            id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct A { uint iters; uint tag; };\n"
                "kernel void frame(constant A &a [[buffer(0)]], texture2d<float, access::write> t [[texture(0)]], uint2 i [[thread_position_in_grid]]) {\n"
                "  float x = float(i.x); for (uint k = 0; k < a.iters; k++) x = fma(x, 1.0000001f, 0.5f); float g = x > -1.0f ? float(a.tag) / 255.0f : 0.0f; t.write(float4(g, g, g, 1), i); }\n" options:nil error:&err];
            MTL4LibraryFunctionDescriptor *fd=[MTL4LibraryFunctionDescriptor new];fd.name=@"frame";fd.library=lib;
            MTL4ComputePipelineDescriptor *pd=[MTL4ComputePipelineDescriptor new];pd.computeFunctionDescriptor=fd;
            id<MTLComputePipelineState> ps=lib&&comp?[comp newComputePipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&err]:nil;
            EXPECT("the slow frame kernel compiles",1,ps!=nil);
            MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new];ad.maxBufferBindCount=1;ad.maxTextureBindCount=1;
            id<MTL4ArgumentTable> at=[g_dev newArgumentTableWithDescriptor:ad error:&err];
            id<MTL4CommandAllocator> fal=[g_dev newCommandAllocator],pal=[g_dev newCommandAllocator];id<MTLSharedEvent> ev=[g_dev newSharedEvent];
            g_present_n=1000;int stale=0,kinds=0;
            for (int round=1;ps&&round<=ROUNDS;round++) {
                uint32_t tag=40+round*23;((uint32_t *)args.contents)[0]=50000;((uint32_t *)args.contents)[1]=tag;
                [at setAddress:args.gpuAddress atIndex:0];[at setTexture:win.gpuResourceID atIndex:0];[fal reset];[pal reset];
                id<MTL4CommandBuffer> fcb=[g_dev newCommandBuffer];[fcb beginCommandBufferWithAllocator:fal];
                id<MTL4ComputeCommandEncoder> ce=[fcb computeCommandEncoder];[ce setComputePipelineState:ps];[ce setArgumentTable:at];
                [ce dispatchThreads:MTLSizeMake(S,S,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];[ce endEncoding];[fcb endCommandBuffer];
                id<MTL4CommandBuffer> pcb=[g_dev newCommandBuffer];[pcb beginCommandBufferWithAllocator:pal];
                kinds+=present_encode_m4(pcb,win,canvas,NULL,g_present_uv[0],&g_fx_present)==PRESENT_METALFX;[pcb endCommandBuffer];
                [g_q4 commit:&fcb count:1];[g_q4 commit:&pcb count:1];[g_q4 signalEvent:ev value:round];
                if (![ev waitUntilSignaledValue:round timeoutMS:10000]) { EXPECT("in-place present completes",1,0); break; }
                id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
                [bl copyFromTexture:canvas sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(2*S,2*S,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:2*S*4 destinationBytesPerImage:4*S*S*4];
                [bl endEncoding];[cb commit];[cb waitUntilCompleted];
                const uint8_t *o=rb.contents;int bad=0;
                for (int y=4;y<2*S-4;y+=9) for (int x=4;x<2*S-4;x+=9) bad+=abs(o[4*(y*2*S+x)+1]-(int)tag)>3;
                stale+=bad!=0;
            }
            EXPECT("in-place presents go through MetalFX",ROUNDS,kinds);
            EXPECT("the scaler read the window texture in place: no input copy made",1,g_fx_present.in==nil&&g_fx_present.in_place_n>=ROUNDS);
            EXPECT("the scaler wrote the canvas: no present pass, no output texture made",1,g_fx_present.out==nil&&g_fx_present.direct_n>=ROUNDS);
            EXPECT("the scaler saw the whole frame still drawing at the present, rounds stale",0,stale);
            res_remove(win);res_remove(canvas);res_remove(args);
        }
        g_present_n=savedPresent;

        // Whole-window capture uses the same upscale and remains asynchronous; its completion includes the final BGRA readback.
        dispatch_semaphore_t grabbed=dispatch_semaphore_create(0);
        mtl_grab_next_present(^(CGImageRef img) {
            EXPECT("whole-window capture produces an image",1,img!=NULL);
            if (img) {
                EXPECT("whole-window image width",W,(int)CGImageGetWidth(img));
                CFDataRef data=CGDataProviderCopyData(CGImageGetDataProvider(img));const uint8_t *p=CFDataGetBytePtr(data);
                size_t off=96*CGImageGetBytesPerRow(img)+96*4;
                EXPECT("whole-window capture preserves sRGB bytes",1,abs(p[off]-200)<=3&&abs(p[off+1]-64)<=3&&abs(p[off+2]-128)<=3);
                CFRelease(data);
            }
            dispatch_semaphore_signal(grabbed);
        });
        grab_present(big,whole,CGSizeMake(W,H));
        EXPECT("whole-window capture callback completes",0,(int)dispatch_semaphore_wait(grabbed,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)));
        res_remove(big);g_fx_mode=0;
    } else printf("no Metal 4 MetalFX spatial scaler on this GPU: the MetalFX present is not checked\n");
    hdr_checks();
#endif
    int invalid[4][4]={{-1,0,4,4},{0,-1,4,4},{0,0,0,4},{7,0,4,4}};
    for(int i=0;i<4;i++) {
        MTLTextureDescriptor *dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];dd.usage=MTLTextureUsageRenderTarget;dd.storageMode=MTLStorageModeShared;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:dd];
        EXPECT("invalid crop is not encoded and does not fake success by stretching the whole image",0,ENCODE([g_q commandBuffer],src,dst,invalid[i]));
    }
    // Real QueuePresent entry: set crop and enqueue first, then immediately change the window; the present ticket must keep that frame's copy.
    PFNNVNDEVICEGETPROCADDRESSPROC get=(PFNNVNDEVICEGETPROCADDRESSPROC)h_nvnBootstrapLoader("nvnDeviceGetProcAddress");
    NVNT_nvnWindowSetCrop set=(NVNT_nvnWindowSetCrop)get(NULL,"nvnWindowSetCrop");
    NVNT_nvnQueuePresentTexture present=(NVNT_nvnQueuePresentTexture)get(NULL,"nvnQueuePresentTexture");
    NVNqueueBuilder builder={0}; NVNqueue queue={0};
    h_nvnQueueBuilderSetDefaults(&builder);
    EXPECT("fixture queue initialized",1,h_nvnQueueInitialize(&queue,&builder));
    uint64_t before, done;
    tex_t tex={.magic=MAGIC('T', 'E', 'X', 0), .serial =10001};tex.d.width=tex.d.height=8;tex.d.depth=tex.d.levels=1;tex.d.format=NVN_FORMAT_RGBA8;tex.d.target=NVN_TEXTURE_TARGET_2D;serial_put(tex.serial,src);
    win_t win={.ntex=1};win.tex[0]=(NVNtexture *)&tex;set((NVNwindow *)&win,4,4,4,4);
    mtl_present_progress(&before,&done);
    // Replay is held until the window changes: QueuePresent must take this frame's crop when it queues the frame, not when replay presents it
    nvn_queue_test_hold(1);
    present(&queue,(NVNwindow *)&win,0);set((NVNwindow *)&win,0,0,8,8);
    {   // without a working hold the crop check below would pass whenever replay ran first
        uint64_t held; usleep(20000); mtl_present_progress(&held,&done);
        EXPECT("the held replay queues no frame for the display",1,held==before);
    }
    nvn_queue_test_hold(0);
    uint64_t tk=display_ticket(win.ticket[0],before);
#ifdef CROP_LEGACY_HEADER
    EXPECT("old queue struct has no room for a four-value copy (struct check)",1,sizeof(pq_item)>=sizeof(tex_t)+sizeof(double)+sizeof(uint64_t)+sizeof br);
#else
    pthread_mutex_lock(&g_pq_mu);
    int saved=tk && memcmp(g_pq[(tk-1)%PQ].crop,br,sizeof br)==0;pthread_mutex_unlock(&g_pq_mu);
    EXPECT("real QueuePresent: the frame's crop copy does not change with the window",1,saved);
#endif
    h_nvnWindowFinalize((NVNwindow *)&win);
#ifndef CROP_LEGACY_HEADER
    {   // A window without a crop of its own presents by the vi layer crop (Link's Awakening in handheld mode: 1280x720 of its 1920x1080 window textures, cropped through vi::SetLayerCrop)
        int none[4]={0},lay[4]={0,4,4,4},over[4]={0,0,16,16},lc[4];
        memset(g_layer_crop,0,sizeof g_layer_crop);EXPECT("no window or layer crop: the whole texture",1,present_crop(none,8,8,lc)==none);
        memcpy(g_layer_crop,lay,sizeof lay);const int *r=present_crop(none,8,8,lc);EXPECT("no window crop: the layer crop",1,r==lc&&!memcmp(lc,lay,sizeof lay));
        EXPECT("the window's own crop wins over the layer crop",1,present_crop(br,8,8,lc)==br);
        memcpy(g_layer_crop,over,sizeof over);EXPECT("a layer crop larger than the texture: the whole texture",1,present_crop(none,8,8,lc)==none);
        memcpy(g_layer_crop,lay,sizeof lay);win_t w2={.ntex=1};w2.tex[0]=(NVNtexture *)&tex;
        mtl_present_progress(&before,&done);
        nvn_queue_test_hold(1);
        present(&queue,(NVNwindow *)&w2,0);memset(g_layer_crop,0,sizeof g_layer_crop);
        nvn_queue_test_hold(0);
        uint64_t t2=display_ticket(w2.ticket[0],before);
        pthread_mutex_lock(&g_pq_mu);int got=t2 && memcmp(g_pq[(t2-1)%PQ].crop,lay,sizeof lay)==0;pthread_mutex_unlock(&g_pq_mu);
        EXPECT("real QueuePresent: a window without a crop queues the layer crop",1,got);
        h_nvnWindowFinalize((NVNwindow *)&w2);
    }
    {   // The crop only picks texels; the picture has the layer's shape (Super Mario Galaxy 2's storybook cutscenes: 1200x960 of a 1920x1080 swapchain, shown at 16:9, issue #949)
        const int W=64,H=36,TW=64,TH=36;MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:TW height:TH mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> sw=[g_dev newTextureWithDescriptor:td];uint8_t *sp=malloc(TW*TH*4);
        const int crop54[4]={0,0,40,32};   // 5:4 in the top-left corner: red inside, green outside
        for(int y=0;y<TH;y++)for(int x=0;x<TW;x++)memcpy(sp+4*(y*TW+x),colors[x>=crop54[2]||y>=crop54[3]],4);
        [sw replaceRegion:MTLRegionMake2D(0,0,TW,TH) mipmapLevel:0 withBytes:sp bytesPerRow:TW*4];free(sp);res_add(sw);res_commit();
        double sa,sb;
        mtl_present_layer(0,0);present_shape(sw,NULL,&sa,&sb);EXPECT("no sized layer: the display's width",1920,(int)sa);EXPECT("no sized layer: the display's height",1080,(int)sb);
        for(int m4=0;m4<2;m4++) {
            int outH[2]={H,40};   // 16:9 fills; a 16:10 canvas keeps the bars above and below
            for(int k=0;k<2;k++) {
                MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:outH[k] mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
                id<MTLTexture> ot=[g_dev newTextureWithDescriptor:od];res_add(ot);res_commit();
                if(m4){EXPECT("Metal 4 present of a 5:4 crop",1,present_offscreen_wait(sw,crop54,ot)!=PRESENT_FAILED);}
                else{id<MTLCommandBuffer> cb=[g_q commandBuffer];EXPECT("classic present of a 5:4 crop",1,present_encode(cb,sw,ot,crop54));[cb commit];[cb waitUntilCompleted];}
                uint8_t *o=malloc(W*outH[k]*4);[ot getBytes:o bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,outH[k]) mipmapLevel:0];
                int bad=0,top=(outH[k]-H)/2;static const uint8_t black[4]={0,0,0,255};
                for(int y=0;y<outH[k];y++)for(int x=0;x<W;x++)bad+=memcmp(o+4*(y*W+x),y<top||y>=top+H?black:colors[0],4)!=0;
                EXPECT(k?"5:4 crop on a 16:10 canvas: 16:9 of red, black above and below":"5:4 crop on a 16:9 canvas: all red, no side bars",0,bad);free(o);res_remove(ot);
            }
        }
        mtl_present_layer(4,3);present_shape(sw,NULL,&sa,&sb);EXPECT("a sized layer gives its own shape",4*(int)sb,3*(int)sa);mtl_present_layer(0,0);
        MTLTextureDescriptor *td2=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1920 height:1344 mipmapped:NO];td2.storageMode=MTLStorageModePrivate;td2.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> tall=[g_dev newTextureWithDescriptor:td2];
        present_shape(tall,NULL,&sa,&sb);EXPECT("a taller texture without DM_SCREEN_ASPECT: 16:9",1080,(int)sb);
        mtl_present_screen(1920,1344);present_shape(tall,NULL,&sa,&sb);EXPECT("DM_SCREEN_ASPECT: a texture that follows it keeps the taller shape",1344,(int)sb);
        present_shape(sw,NULL,&sa,&sb);EXPECT("DM_SCREEN_ASPECT: a 16:9 texture of a build without stubs stays 16:9",1080,(int)sb);
        // A crop whose ratio matches the screen shape takes that shape inside a larger swapchain texture; a crop of another shape stays 16:9
        const int wide[4]={0,0,64,29}, tallc[4]={0,0,48,36};   // ~2.21:1 and 4:3 inside 64x36
        mtl_present_screen(2207,1000);present_shape(sw,wide,&sa,&sb);EXPECT("DM_SCREEN_ASPECT: a matching wider crop gives its width",64,(int)sa);EXPECT("DM_SCREEN_ASPECT: a matching wider crop gives its height",29,(int)sb);
        present_shape(sw,tallc,&sa,&sb);EXPECT("DM_SCREEN_ASPECT: a non-matching crop stays 16:9",1080,(int)sb);
        mtl_present_screen(4,3);present_shape(sw,tallc,&sa,&sb);EXPECT("DM_SCREEN_ASPECT: a matching taller crop gives its width",48,(int)sa);EXPECT("DM_SCREEN_ASPECT: a matching taller crop gives its height",36,(int)sb);
        mtl_present_screen(5,4);
        for(int m4=0;m4<2;m4++) {
            MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:40 height:32 mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> ot=[g_dev newTextureWithDescriptor:od];res_add(ot);res_commit();
            if(m4){EXPECT("Metal 4 present of a matching 5:4 crop",1,present_offscreen_wait(sw,crop54,ot)!=PRESENT_FAILED);}
            else{id<MTLCommandBuffer> cb=[g_q commandBuffer];EXPECT("classic present of a matching 5:4 crop",1,present_encode(cb,sw,ot,crop54));[cb commit];[cb waitUntilCompleted];}
            uint8_t *o=malloc(40*32*4);[ot getBytes:o bytesPerRow:40*4 fromRegion:MTLRegionMake2D(0,0,40,32) mipmapLevel:0];
            int bad=0; for(int i=0;i<40*32;i++) bad+=memcmp(o+4*i,colors[0],4)!=0;
            EXPECT(m4?"Metal 4: 5:4 crop fills a 5:4 canvas with no bars":"classic: 5:4 crop fills a 5:4 canvas with no bars",0,bad);free(o);res_remove(ot);
        }
        mtl_present_screen(0,0);
        // The front end's layout and the mouse follow the same shape, not the crop's (front_picture_shape): a real present of the 5:4 crop
        tex_t st={.magic=MAGIC('T', 'E', 'X', 0), .serial =10002};st.d.width=TW;st.d.height=TH;st.d.depth=st.d.levels=1;st.d.format=NVN_FORMAT_RGBA8;st.d.target=NVN_TEXTURE_TARGET_2D;serial_put(st.serial,sw);
        win_t w3={.ntex=1};w3.tex[0]=(NVNtexture *)&st;set((NVNwindow *)&w3,crop54[0],crop54[1],crop54[2],crop54[3]);
        mtl_present_progress(&before,&done);
        present(&queue,(NVNwindow *)&w3,0);display_ticket(w3.ticket[0],before);
        h_nvnWindowFinalize((NVNwindow *)&w3);
        EXPECT("the shape reported to the front end is the layer's, width",1920,g_shape_w);EXPECT("the shape reported to the front end is the layer's, height",1080,g_shape_h);
        res_remove(sw);
    }
    {   // HDR: a PQ frame queued through nvnQueuePresentTexture while vi's last answer was HDR keeps the PQ decode when the game gets an SDR answer before the queue worker replays
        // it (Game's HDR, then BT.2446 or Off). A shot of that frame, taken in present_now through the present's route (dm_gfx_shot_at, as DM_CTL shot), shows the route: PQ 594
        // decoded is white (255), the raw code as an 8-bit value 148
        MTLTextureDescriptor *pd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:8 height:8 mipmapped:NO];pd.storageMode=MTLStorageModeShared;pd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> pw=[g_dev newTextureWithDescriptor:pd];uint32_t px[64];for(int i=0;i<64;i++)px[i]=594u|594u<<10|594u<<20|3u<<30;
        [pw replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:px bytesPerRow:8*4];res_add(pw);res_commit();
        tex_t pt={.magic=MAGIC('T', 'E', 'X', 0), .serial =10003};pt.d.width=pt.d.height=8;pt.d.depth=pt.d.levels=1;pt.d.format=NVN_FORMAT_RGB10A2;pt.d.target=NVN_TEXTURE_TARGET_2D;serial_put(pt.serial,pw);
        win_t w4={.ntex=1};w4.tex[0]=(NVNtexture *)&pt;
        NSString *path=[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"t_present_crop_vi_%d.png",getpid()]];
        mtl_present_progress(&before,&done);
        atomic_store(&g_hdr_vi_on,1);   // vi told the game HDR output
        nvn_queue_test_hold(1);
        present(&queue,(NVNwindow *)&w4,0);
        atomic_store(&g_hdr_vi_on,0);   // the game's next query: SDR, before the worker replays the present
        dm_gfx_shot_at(1,path.UTF8String);
        nvn_queue_test_hold(0);
        uint64_t t4=display_ticket(w4.ticket[0],before);
        pthread_mutex_lock(&g_pq_mu);int kept=t4 && g_pq[(t4-1)%PQ].vi_hdr==1;pthread_mutex_unlock(&g_pq_mu);
        EXPECT("real QueuePresent: the frame carries vi's answer from when the game queued it (HDR)",1,kept);
        CGImageSourceRef is=CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],NULL);CGImageRef img=is?CGImageSourceCreateImageAtIndex(is,0,NULL):NULL;
        uint8_t o[4]={0};
        if(img){CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);CGContextRef cx=CGBitmapContextCreate(o,1,1,8,4,cs,kCGImageAlphaNoneSkipLast);
            CGContextDrawImage(cx,CGRectMake(-4,-4,8,8),img);CGContextRelease(cx);CGColorSpaceRelease(cs);CGImageRelease(img);}
        if(is)CFRelease(is);unlink(path.UTF8String);
        printf("  queued PQ frame shot pixel %d %d %d\n",o[0],o[1],o[2]);
        EXPECT("  and the present decodes it as PQ: its shot is white, not the raw code 148",1,img!=NULL&&o[0]>=253&&o[1]>=253&&o[2]>=253);
        h_nvnWindowFinalize((NVNwindow *)&w4);res_remove(pw);
    }
#endif
    h_nvnQueueFinalize(&queue);
    printf("== %d checks, %d failed ==\n",cases,failed);return failed!=0;
} }
