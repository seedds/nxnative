// HDR output (DM_HDR=1): the status query's answer and the present's decode, on the GPU with closed-form expectations (no game, no window).
// The vi answer: HDR on only while the window's screen can show EDR, format 1 with it (the SDK's on = format == 1), the decode latched once answered. The present: PQ code values
// (an RGB10A2 window) decoded with 1.0 at the 203-nit paper white and written as they are above it (Core Animation maps them by the layer's HDR10 metadata), an 8-bit sRGB
// source converted to BT.2020, a screenshot of an HDR frame brought to SDR white with its hue, and a 10-bit window of a game
// that never asked for HDR read as stored. hle_vi.c is linked beside this file; t_present_hdr_front.c stands in for the front end
#include "../nvn/nvn_mtl.m"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
extern int t_hdr_screen; extern unsigned t_hdr_lo, t_hdr_hi;
int vi_hdr_answered(void);
static int checks, failed;
#define CHECK(name, ok) do { int pass_ = !!(ok); checks++; failed += !pass_; printf("%s: %s\n", name, pass_ ? "PASS" : "FAIL"); } while (0)

// hle_vi.c's HLE entries are static: found by their mangled names in the table the loader reads (hle.h HLE_FORCE)
typedef struct { const char *name; void *fn; const char *why; } force_ent;
static void *hle_fn(const char *name) {
    unsigned long n = 0; const force_ent *e = (const force_ent *)(const void *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &n);
    for (unsigned long i = 0; e && i < n / sizeof *e; i++) if (!strcmp(e[i].name, name)) return e[i].fn;
    return NULL;
}
typedef uint32_t (*status_fn)(uint8_t *, uint32_t *, uint16_t *, const void *);
typedef uint32_t (*paper_fn)(uint16_t *, const void *);
typedef uint32_t (*range_fn)(void *, uint32_t, uint32_t);

// SMPTE ST 2084: nits to a 10-bit code value, and a 10-bit code value back to nits (what the shader's decode must give)
static unsigned pq_code(double nits) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128, c3 = 2392.0 / 128;
    double y = pow(nits / 10000.0, m1);
    return (unsigned)lround(pow((c1 + c2 * y) / (1 + c3 * y), m2) * 1023.0);
}
static double pq_nits(unsigned code) {
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128, c3 = 2392.0 / 128;
    double e = pow(code / 1023.0, 1.0 / m2);
    return 10000.0 * pow(fmax(e - c1, 0) / (c2 - c3 * e), 1.0 / m1);
}
static double srgb_dec(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double srgb_enc(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }
#define N 8
// An N x 1 RGB10A2 source of code values (r, g, b per pixel)
static id<MTLTexture> src10(const unsigned c[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; uint32_t w[N];
    for (int i = 0; i < N; i++) w[i] = c[i][0] | c[i][1] << 10 | c[i][2] << 20 | 3u << 30;
    [t replaceRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0 withBytes:w bytesPerRow:sizeof w];
    return t;
}
static id<MTLTexture> src8(const uint8_t c[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; uint8_t b[N * 4];
    for (int i = 0; i < N; i++) { b[4 * i] = c[i][0]; b[4 * i + 1] = c[i][1]; b[4 * i + 2] = c[i][2]; b[4 * i + 3] = 255; }
    [t replaceRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0 withBytes:b bytesPerRow:sizeof b];
    return t;
}
// The present of src into an N x 1 target of the given format; out gets RGB as floats (16-bit float targets) or 0-255 (8-bit targets)
static int run(id<MTLTexture> src, MTLPixelFormat fmt, double out[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> dst = [g_dev newTextureWithDescriptor:d]; id<MTLCommandBuffer> cb = [g_q commandBuffer];
    if (!present_encode(cb, src, dst, NULL)) return 0;
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return 0;
    if (fmt == MTLPixelFormatRGBA16Float) {
        __fp16 h[N * 4]; [dst getBytes:h bytesPerRow:sizeof h fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int i = 0; i < N; i++) for (int k = 0; k < 3; k++) out[i][k] = h[4 * i + k];
    } else {
        uint8_t b[N * 4]; [dst getBytes:b bytesPerRow:sizeof b fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int i = 0; i < N; i++) for (int k = 0; k < 3; k++) out[i][k] = b[4 * i + k];
    }
    return 1;
}
static int near(double a, double b, double rel, double abs_) { return fabs(a - b) <= fmax(abs_, rel * fabs(b)); }

int main(void) { @autoreleasepool {
    setenv("DM_HDR", "1", 1); unsetenv("DM_HDR_PAPER_WHITE"); unsetenv("DM_HDR_PEAK");
    mtl_init();
    status_fn status = (status_fn)hle_fn("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE");
    paper_fn paper = (paper_fn)hle_fn("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE");
    range_fn range = (range_fn)hle_fn("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt");
    CHECK("the vi HLEs are registered", status && paper && range);
    if (!status || !paper || !range) return 1;
    double o[N][3];

    // A 10-bit window of a game whose status query has not answered HDR: read as stored, not as PQ
    {
        unsigned c[N][3]; for (int i = 0; i < N; i++) c[i][0] = c[i][1] = c[i][2] = 512;
        CHECK("not answered: an RGB10A2 window into an 8-bit target is its stored value (512 → 128)", run(src10(c), MTLPixelFormatRGBA8Unorm, o) && fabs(o[0][0] - 128) <= 1 && fabs(o[N - 1][2] - 128) <= 1);
        CHECK("not answered: into the HDR layer it is decoded as gamma-coded SDR, not PQ", run(src10(c), MTLPixelFormatRGBA16Float, o) && near(o[0][1], srgb_dec(512 / 1023.0), 0.01, 0.002));
    }

    // The status query: SDR on a screen without EDR, HDR (format 1, peak 1000) on one with it; the decode stays on once answered
    {
        uint8_t on = 9; uint32_t fmt = 9; uint16_t peak = 9;
        t_hdr_screen = 0; status(&on, &fmt, &peak, NULL);
        CHECK("a screen without EDR: the status answers SDR (on 0, format 0, peak 0)", on == 0 && fmt == 0 && peak == 0);
        CHECK("a screen without EDR: the window is not taken as PQ", vi_hdr_answered() == 0);
        t_hdr_screen = 1; status(&on, &fmt, &peak, NULL);
        CHECK("a screen with EDR: HDR on, format 1, peak 1000", on == 1 && fmt == 1 && peak == 1000);
        CHECK("a screen with EDR: the window is taken as PQ", vi_hdr_answered() == 1);
        t_hdr_screen = 0; status(&on, &fmt, &peak, NULL);
        CHECK("the window moves to a screen without EDR: SDR is answered, and the HDR window already made is still decoded", on == 0 && fmt == 0 && vi_hdr_answered() == 1);
        t_hdr_screen = 1;
        uint16_t pw = 0; paper(&pw, NULL);
        CHECK("the paper white is 203 nits", pw == 203);
        range(NULL, 0, 1000);
        CHECK("the declared extended range reaches the front end (0..1000)", t_hdr_lo == 0 && t_hdr_hi == 1000);
    }

    const double nits[N] = {0, 0.1, 10, 80, 203, 500, 1000, 10000};
    unsigned grey[N][3]; double want[N];
    for (int i = 0; i < N; i++) { grey[i][0] = grey[i][1] = grey[i][2] = pq_code(nits[i]); want[i] = pq_nits(grey[i][0]) / 203.0; }

    // The HDR layer holds linear light, 1.0 at 203 nits, highlights as they are: Core Animation maps them into the screen's headroom by the layer's HDR10 metadata
    {
        int ok = run(src10(grey), MTLPixelFormatRGBA16Float, o), bad = 0;
        for (int i = 0; ok && i < N; i++) bad += !near(o[i][0], want[i], 0.002, 1e-5) || o[i][0] != o[i][1] || o[i][1] != o[i][2];
        CHECK("PQ 0, 0.1, 10, 80, 203, 500, 1000 and 10000 nits decode to nits / 203, grey stays grey", ok && !bad);
        CHECK("203 nits is 1.0, SDR white", ok && near(o[4][0], 1.0, 0.002, 0));
        CHECK("10000 nits is written as it is (about 49), not limited by the host", ok && o[7][0] > 45.0);
    }
    {
        unsigned c[N][3]; for (int i = 0; i < N; i++) { c[i][0] = pq_code(1000); c[i][1] = pq_code(500); c[i][2] = pq_code(100); }
        int ok = run(src10(c), MTLPixelFormatRGBA16Float, o);
        CHECK("a coloured highlight keeps each channel's own luminance (1000, 500 and 100 nits)", ok && near(o[0][0], pq_nits(c[0][0]) / 203.0, 0.002, 0) && near(o[0][1], pq_nits(c[0][1]) / 203.0, 0.002, 0) && near(o[0][2], pq_nits(c[0][2]) / 203.0, 0.002, 0));
    }
    // An 8-bit sRGB source in the HDR layer (the game's SDR window before it switches): decoded and converted to BT.2020, its white at 1.0
    {
        uint8_t c[N][3] = {{255, 255, 255}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 0, 0}, {64, 64, 64}, {255, 255, 255}};
        int ok = run(src8(c), MTLPixelFormatRGBA16Float, o);
        CHECK("SDR white is 1.0 in the HDR layer, the paper white the HDR window has", ok && near(o[0][0], 1.0, 0.002, 0) && near(o[0][2], 1.0, 0.002, 0));
        CHECK("SDR mid grey 128 decodes to its linear value", ok && near(o[1][1], srgb_dec(128 / 255.0), 0.003, 1e-4));
        CHECK("SDR red is BT.709 red in BT.2020 (0.6274, 0.0691, 0.0164)", ok && near(o[2][0], 0.627404, 0.003, 0) && near(o[2][1], 0.069097, 0.01, 1e-4) && near(o[2][2], 0.016391, 0.02, 1e-4));
    }
    // A screenshot of an HDR frame (an 8-bit target): as an SDR screen shows it, highlights to SDR white with their hue, then BT.709
    {
        unsigned c[N][3]; for (int i = 0; i < N; i++) c[i][0] = c[i][1] = c[i][2] = pq_code(nits[i]);
        c[7][0] = pq_code(1000); c[7][1] = pq_code(500); c[7][2] = pq_code(100);
        int ok = run(src10(c), MTLPixelFormatRGBA8Unorm, o), bad = 0;
        for (int i = 0; ok && i < 7; i++) bad += fabs(o[i][0] - 255.0 * srgb_enc(fmin(want[i], 1.0))) > 1.0;
        CHECK("screenshot: grey to 203 nits as SDR, brighter grey at SDR white (255)", ok && !bad && o[4][0] == 255 && o[6][0] == 255);
        double r = pq_nits(c[7][0]), v[3] = {1.0, pq_nits(c[7][1]) / r, pq_nits(c[7][2]) / r}, e[3];
        static const double m[3][3] = {{1.660491, -0.587641, -0.072850}, {-0.124550, 1.132900, -0.008349}, {-0.018151, -0.100579, 1.118730}};
        for (int k = 0; k < 3; k++) { double x = m[k][0] * v[0] + m[k][1] * v[1] + m[k][2] * v[2]; e[k] = 255.0 * srgb_enc(fmin(fmax(x, 0), 1)); }
        CHECK("screenshot: a coloured highlight keeps its hue (1000, 500 and 100 nits as 1, 0.5 and 0.1 of SDR white, then BT.709)", ok && fabs(o[7][0] - e[0]) <= 1.5 && fabs(o[7][1] - e[1]) <= 1.5 && fabs(o[7][2] - e[2]) <= 1.5);
    }
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
