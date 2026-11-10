// HDR output (DM_HDR=1): the window's layer, the status query's answer and the present's conversions, on the GPU with closed-form expectations (no game, no window).
// The layer (hle_vi.c dm_hdr_layer_tick, driven here as a front end drives it, with a clock of its own): the HDR layer only once a Switch 2 game has asked and the screen can show EDR,
// HDR answered once that layer exists, back to SDR after the game's SDR window. The vi answer: format 1 with HDR on (the SDK's on = format == 1), the decode
// latched once answered, the automatic peak in held steps. The present: PQ code values (an RGB10A2 window) written as they are into the PQ layer (RGB10A2 in BT.2100 PQ) or rolled off
// into the screen's current peak without metadata, SDR and float sources encoded for it at the paper white, a screenshot of an HDR frame converted to BT.709 and its highlights
// compressed into SDR white with their hue, a 10-bit window of a game that never asked for HDR read as SDR. hle_vi.c is linked beside this file; t_present_hdr_front.c stands in for the front end
#include "../nvn/nvn_mtl.m"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int dm_hdr_meta(void); float dm_hdr_source_peak(void); void dm_hdr_layer_meta_set(int on);
extern unsigned t_hdr_lo, t_hdr_hi; extern float t_hdr_headroom;
int vi_hdr_answered(void); int dm_hdr_mode(void); void dm_hdr_set_mode(int m); int dm_hdr_layer_tick(int can, float headroom, int layer_pq, int64_t now_ms);
static int checks, failed;
#define CHECK(name, ok) do { int pass_ = !!(ok); checks++; failed += !pass_; printf("%s: %s\n", name, pass_ ? "PASS" : "FAIL"); } while (0)

// hle_vi.c's HLE entries are static: found by their mangled names in the table the loader reads (hle.h HLE_FORCE)
typedef struct { const char *name; void *fn; const char *why; const char *to; } force_ent;   // hle.h hle_force_ent
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
// The game's SDR picture as its screens show it (nvn_mtl.m present_sdr_gamma; DM_SDR_COLORSPACE unset): a power curve of 2.2, not the piecewise sRGB curve
static double sdr_dec(double c) { return pow(c, 2.2); }
static double sdr_enc(double l) { return pow(l, 1.0 / 2.2); }
static double srgb_dec(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
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
static uint8_t t_src_alpha = 255;   // src8's alpha
static id<MTLTexture> src8(const uint8_t c[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; uint8_t b[N * 4];
    for (int i = 0; i < N; i++) { b[4 * i] = c[i][0]; b[4 * i + 1] = c[i][1]; b[4 * i + 2] = c[i][2]; b[4 * i + 3] = t_src_alpha; }
    [t replaceRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0 withBytes:b bytesPerRow:sizeof b];
    return t;
}
// An N x 1 RGBA16Float source of linear values (a float window)
static id<MTLTexture> src16f(const float c[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; __fp16 h[N * 4];
    for (int i = 0; i < N; i++) { for (int k = 0; k < 3; k++) h[4 * i + k] = (__fp16)c[i][k]; h[4 * i + 3] = 1; }
    [t replaceRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0 withBytes:h bytesPerRow:sizeof h];
    return t;
}
// The present of src into an N x 1 target of the given format; out gets RGB as 0-1023 (RGB10A2, the PQ layer) or 0-255 (8-bit targets)
static double t_alpha[N];   // the last run's alpha, 0 to 1
static int run(id<MTLTexture> src, MTLPixelFormat fmt, double out[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> dst = [g_dev newTextureWithDescriptor:d]; id<MTLCommandBuffer> cb = [g_q commandBuffer];
    if (!present_encode(cb, src, dst, NULL)) return 0;
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return 0;
    if (fmt == MTLPixelFormatRGB10A2Unorm) {   // the PQ layer: 10-bit code values
        uint32_t w[N]; [dst getBytes:w bytesPerRow:sizeof w fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int i = 0; i < N; i++) { for (int k = 0; k < 3; k++) out[i][k] = w[i] >> (10 * k) & 0x3ff; t_alpha[i] = (w[i] >> 30) / 3.0; }
    } else {
        uint8_t b[N * 4]; [dst getBytes:b bytesPerRow:sizeof b fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int i = 0; i < N; i++) { for (int k = 0; k < 3; k++) out[i][k] = b[4 * i + k]; t_alpha[i] = b[4 * i + 3] / 255.0; }
    }
    return 1;
}
static int near(double a, double b, double rel, double abs_) { return fabs(a - b) <= fmax(abs_, rel * fabs(b)); }

// The front end's side of the layer (front_mac.m front_hdr_tick): a tick every 250 ms on a clock of the test's own, the layer as the tick asks
static int t_layer; static int64_t t_ms = 1000;
static int tick(int can) { t_ms += 250; t_layer = dm_hdr_layer_tick(can, t_hdr_headroom, t_layer, t_ms); return t_layer; }
static void ticks(int can, int n) { for (int i = 0; i < n; i++) tick(can); }
#define EDR(can) ticks(can, 12)   // three seconds of ticks: a change of screen held, the layer changed
// present_shoulder (present.metal) in double precision
static double shoulder(double m, double k, double t, double s) {
    if (!(m > k)) return m;
    double a = t - k, x = m - k; return fmin(k + x / (1 + x * (1 / a - 1 / fmax(s - k, a))), t);
}

int main(void) { @autoreleasepool {
    setenv("DM_HDR", "1", 1); setenv("DM_HDR_PEAK", "1000", 1); unsetenv("DM_HDR_META");   // a fixed peak (unset, automatic, is checked below); the paper white is 100
    mtl_init();
    mtl_present_layer(N, 1);   // a layer of the fixtures' N x 1 shape, so each source texel lands on its own target texel: left to the display's 16:9, the picture is a bar in the middle (present_shape)
    status_fn status = (status_fn)hle_fn("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE");
    paper_fn paper = (paper_fn)hle_fn("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE");
    range_fn range = (range_fn)hle_fn("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt");
    CHECK("the vi HLEs are registered", status && paper && range);
    if (!status || !paper || !range) return 1;
    double o[N][3];
    uint8_t on = 9; uint32_t fmt = 9; uint16_t peak = 9;
    #define Q() status(&on, &fmt, &peak, NULL)
    // A 10-bit window of a game whose status query has not answered HDR: read as stored, not as PQ
    {
        unsigned c[N][3]; for (int i = 0; i < N; i++) c[i][0] = c[i][1] = c[i][2] = 512;
        CHECK("not answered: an RGB10A2 window into an 8-bit target is its stored value (512 → 128)", run(src10(c), MTLPixelFormatRGBA8Unorm, o) && fabs(o[0][0] - 128) <= 1 && fabs(o[N - 1][2] - 128) <= 1);
        CHECK("not answered: into the PQ layer it is decoded as gamma-coded SDR and encoded at 100 nits, the screen's white (no HDR answered yet), not taken as PQ", run(src10(c), MTLPixelFormatRGB10A2Unorm, o) && fabs(o[0][1] - pq_code(sdr_dec(512 / 1023.0) * 100)) <= 1.5);
    }

    // The layer and the answer (dm_hdr_layer_tick): the SDR layer until a Switch 2 game runs (dm_hdr_boot) or asks, the HDR layer then on a screen that can show EDR, HDR answered
    // once it exists; a screen change counts once it has held half a second; back to the SDR layer once the game presents its SDR window
    {
        t_hdr_headroom = 16.0f;
        EDR(1);
        CHECK("layer: no game has asked yet: the SDR layer, whatever the screen", t_layer == 0);
        Q();
        CHECK("layer: the first query, before the HDR layer exists: SDR (on 0, format 0, peak 0), the window not taken as PQ", on == 0 && fmt == 0 && peak == 0 && vi_hdr_answered() == 0);
        tick(1);
        CHECK("layer: the next tick makes the HDR layer", t_layer == 1);
        Q(); int made = on; tick(1); Q();
        CHECK("layer: HDR (format 1, peak 1000) from the tick that sees the HDR layer (the front end has made it), not waiting for the headroom's ramp; the window taken as PQ", !made && on == 1 && fmt == 1 && peak == 1000 && vi_hdr_answered() == 1);
        tick(0); Q(); int blip = on; tick(1); Q();
        CHECK("layer: a screen without EDR for one tick (a drag along the edge between two screens) changes nothing", blip == 1 && on == 1 && t_layer == 1);
        g_present_sdr_run = 0; tick(0); tick(0); tick(0); Q();
        CHECK("layer: the window on a screen without EDR for half a second: SDR answered, the HDR window already made still decoded", on == 0 && fmt == 0 && peak == 0 && vi_hdr_answered() == 1);
        CHECK("layer: the HDR layer stays while the game still presents its HDR window", t_layer == 1);
        g_present_sdr_run = 1; tick(0); int one = t_layer; g_present_sdr_run = 2; tick(0);
        CHECK("layer: back to the SDR layer after two presents of the game's SDR window, not one", one == 1 && t_layer == 0);
        t_hdr_headroom = 16.0f; EDR(1); Q();
        CHECK("layer: back on a screen with EDR: the HDR layer again, HDR", t_layer == 1 && on == 1 && fmt == 1);
        g_present_sdr_run = 2; EDR(0); ticks(1, 2); Q(); int early = on; ticks(1, 2); Q();
        CHECK("layer: from a screen without EDR back to one with it: SDR until the change has held half a second and the HDR layer exists, then HDR", early == 0 && t_layer == 1 && on == 1);
        uint16_t pw = 0; paper(&pw, NULL);
        CHECK("the paper white is 100 nits", pw == 100);
        range(NULL, 0, 1000);
        CHECK("the declared extended range reaches the front end (0..1000)", t_hdr_lo == 0 && t_hdr_hi == 1000);
    }

    // HDR output changed while the game runs (the iOS in-game menu, dm_hdr_set_mode): the next status query follows, DM_HDR=1 having been read at start
    {
        int m0 = dm_hdr_mode();
        dm_hdr_set_mode(0); Q();
        CHECK("turned off while the game runs: the next query answers SDR (on 0, format 0, peak 0)", m0 == 1 && dm_hdr_mode() == 0 && on == 0 && fmt == 0 && peak == 0);
        g_present_sdr_run = 2; tick(1);
        CHECK("turned off: the HDR layer goes once the game presents its SDR window", t_layer == 0);
        dm_hdr_set_mode(1); Q(); int first = on; EDR(1); Q();
        CHECK("turned on again: SDR until the HDR layer exists, then HDR on, format 1, peak 1000", first == 0 && on == 1 && fmt == 1 && peak == 1000);
        dm_hdr_set_mode(2); EDR(0); Q();
        CHECK("any non-zero mode is on, by the screen: SDR on a screen without EDR", dm_hdr_mode() == 1 && on == 0 && fmt == 0);
        dm_hdr_set_mode(1); EDR(1);
    }

    // The peak (the HDR peak row): a number as it is; DM_HDR_PEAK=auto, 100 nits times the screen's current headroom (what a PQ layer shows unchanged, EDR = nits / 100), in steps
    // of about 6 % (40 to a decade), at least 1.25 times the paper white (100 here: 130) and at most 10,000, 1000 with no headroom known; a new step is taken once readings have
    // stayed within two neighboring steps for DM_HDR_PEAK_HOLD ms (0 here), the lower of them. SDR still answers peak 0. DM_HDR_PEAK is read at every query
    {
        #define Q2() do { Q(); Q(); } while (0)
        setenv("DM_HDR_PEAK", "1600", 1); Q();
        CHECK("DM_HDR_PEAK=1600: peak 1600", on == 1 && peak == 1600);
        setenv("DM_HDR_PEAK", "auto", 1); setenv("DM_HDR_PEAK_HOLD", "0", 1);
        t_hdr_headroom = 16.0f; Q2();
        CHECK("automatic, headroom 16 (the XDR screen at half brightness): peak 1580 (1600 to the step)", on == 1 && fmt == 1 && peak == 1580);
        t_hdr_headroom = 6.45f; Q2();
        CHECK("automatic, headroom 6.45: peak 630 (645 to the step)", peak == 630);
        t_hdr_headroom = 4.0f; Q2();
        CHECK("automatic, headroom 4 (full brightness): 400", peak == 400);
        t_hdr_headroom = 1.2f; Q2();
        CHECK("automatic, headroom 1.2: at least 1.25 times the paper white, 130 (the present rolls off what the screen cannot show)", peak == 130);
        t_hdr_headroom = 1000.0f; Q2();
        CHECK("automatic, a headroom of 1000: at most 10,000", peak == 10000);
        t_hdr_headroom = 0.0f; Q2(); uint16_t p0 = peak; t_hdr_headroom = NAN; Q2();
        CHECK("automatic with no headroom known (0 or not a number): 1000", p0 == 1000 && peak == 1000);
        setenv("DM_HDR_PEAK_HOLD", "100000", 1); t_hdr_headroom = 16.0f; Q2(); Q2();
        CHECK("automatic: a new step waits for the hold (100 s here): still the last automatic answer, 10,000 (no headroom known answers 1000 without moving it)", peak == 10000);
        setenv("DM_HDR_PEAK_HOLD", "0", 1); Q2();
        CHECK("automatic: then taken once it has held: 1580", peak == 1580);
        setenv("DM_HDR_PEAK_HOLD", "100000", 1); t_hdr_headroom = 4.0f; Q2(); uint16_t held = peak; t_hdr_headroom = 6.45f; Q(); uint16_t held2 = peak;
        setenv("DM_HDR_PEAK_HOLD", "0", 1); Q();
        CHECK("automatic: a reading two steps or more from the waiting one restarts the hold (1580 kept while 400, then 630, wait), and the newer one is taken (630)", held == 1580 && held2 == 1580 && peak == 630);
        // A headroom flickering between two neighboring steps above the answer (1060 and 1120 nits over 1000): the answer moves once the hold has passed, to the lower one. The old
        // rule restarted the hold at each change and never moved
        setenv("DM_HDR_PEAK_HOLD", "0", 1); t_hdr_headroom = 10.0f; Q2(); uint16_t base = peak;
        setenv("DM_HDR_PEAK_HOLD", "40", 1);
        for (int i = 0; i < 8; i++) { t_hdr_headroom = i & 1 ? 11.2f : 10.6f; Q(); usleep(10000); }
        CHECK("automatic: readings flickering between two neighboring steps (1060, 1120 over 1000) are taken after the hold, at the lower one (1060)", base == 1000 && peak == 1060);
        setenv("DM_HDR_PEAK_HOLD", "0", 1);
        setenv("DM_HDR_PEAK", "Auto", 1); t_hdr_headroom = 16.0f; Q2(); uint16_t pa = peak; setenv("DM_HDR_PEAK", "abc", 1); Q2(); uint16_t pb = peak; setenv("DM_HDR_PEAK", "-1", 1); Q2();
        CHECK("DM_HDR_PEAK that is neither nits nor auto (Auto, abc, -1) counts as unset: automatic (1580), never a peak of 0", pa == 1580 && pb == 1580 && peak == 1580);
        setenv("DM_HDR_PEAK", "0", 1); Q2(); uint16_t pz = peak; setenv("DM_HDR_PEAK", "12abc", 1); Q2(); uint16_t pj = peak; setenv("DM_HDR_PEAK", "70000", 1); Q2(); uint16_t pc = peak;
        setenv("DM_HDR_PEAK", "", 1); Q2(); uint16_t pe = peak; setenv("DM_HDR_PEAK", "1000", 1); Q2();
        CHECK("DM_HDR_PEAK 0, 12abc and empty count as unset (1580), 70000 is held to 65535, 1000 is 1000", pz == 1580 && pj == 1580 && pe == 1580 && pc == 65535 && peak == 1000);
        setenv("DM_HDR_PEAK", "auto", 1); t_hdr_headroom = 16.0f; Q2(); EDR(0); Q2(); EDR(1); Q();
        CHECK("automatic: queries on a screen without EDR leave the held peak alone (1580 again at once on the way back)", peak == 1580);
        unsetenv("DM_HDR_PEAK"); t_hdr_headroom = 1.0f; Q2(); uint16_t pu = peak; t_hdr_headroom = 0.0f; Q2();
        CHECK("unset: automatic (headroom 1: the floor, 130; no headroom known: 1000)", pu == 130 && peak == 1000);
        CHECK("the metadata is off by default (DM_HDR_META unset)", !dm_hdr_meta());
        setenv("DM_HDR_PEAK", "1000", 1); unsetenv("DM_HDR_PEAK_HOLD"); t_hdr_headroom = 16.0f; Q();
        CHECK("DM_HDR_PEAK=1000 again: peak 1000", peak == 1000);
        #undef Q2
    }

    const double nits[N] = {0, 0.1, 10, 80, 203, 500, 1000, 10000};
    unsigned gray[N][3];
    for (int i = 0; i < N; i++) gray[i][0] = gray[i][1] = gray[i][2] = pq_code(nits[i]);

    // The PQ layer (RGB10A2 in BT.2100 PQ, the HDR layer): the game's HDR10 window goes in as it is, an 8-bit sRGB source is encoded as HDR10 with its white at the paper white
    {
        int ok = run(src10(gray), MTLPixelFormatRGB10A2Unorm, o), bad = 0;   // headroom 16: the screen shows the declared 0..1000
        for (int i = 0; ok && i < N; i++) for (int k = 0; k < 3; k++) bad += fabs(o[i][k] - gray[i][k]) > 0.5;
        CHECK("PQ layer: the HDR10 window's code values are written as they are (the declared range within the screen)", ok && !bad);
        uint8_t c[N][3] = {{255, 255, 255}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 0, 0}, {64, 64, 64}, {255, 255, 255}};
        t_src_alpha = 0; ok = run(src8(c), MTLPixelFormatRGB10A2Unorm, o); t_src_alpha = 255;
        CHECK("PQ layer: SDR white is 100 nits, the screen's SDR white, black is code 0", ok && fabs(o[0][0] - pq_code(100)) <= 1.5 && fabs(o[0][2] - pq_code(100)) <= 1.5 && o[5][0] <= 1);
        CHECK("PQ layer: SDR mid gray 128 is its linear value at the paper white", ok && fabs(o[1][1] - pq_code(sdr_dec(128 / 255.0) * 100)) <= 1.5);
        { uint8_t dk[N][3]; for (int i = 0; i < N; i++) dk[i][0] = dk[i][1] = dk[i][2] = (uint8_t)(4 * (i + 1));
          int okd = run(src8(dk), MTLPixelFormatRGB10A2Unorm, o), badd = 0;
          for (int i = 0; okd && i < N; i++) badd += fabs(o[i][1] - pq_code(sdr_dec(4 * (i + 1) / 255.0) * 100)) > 1.5;
          CHECK("PQ layer: SDR shadows (codes 4 to 32) decoded with the screens' 2.2, not lifted as the piecewise sRGB curve lifts them (code 16: 2.3 times)", okd && !badd && o[3][1] < pq_code(srgb_dec(16 / 255.0) * 100) - 10); }
        CHECK("PQ layer: a converted source is opaque (alpha 0 in, 1 out)", ok && t_alpha[0] == 1.0 && t_alpha[N - 1] == 1.0);
        float f[N][3] = {{1, 1, 1}, {0.5f, 0.5f, 0.5f}, {4, 4, 4}, {0, 0, 0}, {-1, -1, -1}, {-0.05f, 1, 1}, {1, 1, 1}, {1, 1, 1}};
        ok = run(src16f(f), MTLPixelFormatRGB10A2Unorm, o);
        CHECK("PQ layer: a float source is linear light, 1.0 at the paper white, 4.0 at four times it, not decoded as sRGB", ok && fabs(o[0][1] - pq_code(100)) <= 1.5 && fabs(o[1][1] - pq_code(50)) <= 1.5 &&
              fabs(o[2][1] - pq_code(400)) <= 1.5);
        CHECK("PQ layer: a negative float value is black", ok && o[3][1] <= 1 && o[4][1] <= 1);
        // scRGB (-0.05, 1, 1), a cyan outside BT.709: in BT.2020 its red is 0.627404 * -0.05 + 0.329283 + 0.043313 = 0.341, kept, where dropping the negative first gave 0.373
        CHECK("PQ layer: a float color outside BT.709 (scRGB's negative red) is converted before negatives are dropped", ok && fabs(o[5][0] - pq_code(0.34122 * 100)) <= 1.5);
        g_present_sdr_run = 0; EDR(0); Q();   // the status answers SDR: a screen without EDR, the HDR layer still up for the game's HDR window
        ok = run(src8(c), MTLPixelFormatRGB10A2Unorm, o);
        CHECK("PQ layer: while the status answers SDR, SDR white is still 100 nits", ok && on == 0 && fabs(o[0][0] - pq_code(100)) <= 1.5 && fabs(o[1][1] - pq_code(sdr_dec(128 / 255.0) * 100)) <= 1.5);
        g_present_sdr_run = 2; EDR(1); Q();
    }

    // The PQ layer without metadata (DM_HDR_META=0): a game that may draw more than the screen shows right now (100 x the current headroom) is rolled off above the knee, three
    // quarters of that and never below the paper white, into it (present.metal mode 4), what the game may draw (the smaller of the declared range and the peak told) landing on it.
    // At headroom 4 the target is 400 and the knee 300; declared 1801 (Mario Kart World at its title), told 1000
    {
        float k = 0, t = 0, sp = 0;
        Q();   // the peak told: 1000 (DM_HDR_PEAK=1000)
        t_hdr_headroom = 4.0f; range(NULL, 0, 1801);
        setenv("DM_HDR_META", "1", 1); int with_meta = dm_hdr_rolloff(&k, &t, &sp);
        setenv("DM_HDR_META", "0", 1);
        int on1801 = dm_hdr_rolloff(&k, &t, &sp);
        CHECK("roll-off: only without metadata; knee 300, target 400, the source's peak 1000 (the peak told, under the 1801 declared)", !with_meta && on1801 && fabsf(k - 300) < 0.5f && fabsf(t - 400) < 0.5f && fabsf(sp - 1000) < 0.5f);
        const double rn[N] = {100, 203, 300, 350, 400, 1000, 1801, 10000};
        unsigned g[N][3]; for (int i = 0; i < N; i++) g[i][0] = g[i][1] = g[i][2] = pq_code(rn[i]);
        int ok = run(src10(g), MTLPixelFormatRGB10A2Unorm, o), same = 1, mono = 1, under = 1, curve = 1;
        for (int i = 0; ok && i < 2; i++) for (int c = 0; c < 3; c++) same &= o[i][c] == g[i][c];
        for (int i = 0; ok && i < N; i++) { under &= pq_nits((unsigned)lround(o[i][1])) <= 403; if (i) mono &= o[i][1] >= o[i - 1][1]; }
        for (int i = 2; ok && i < 6; i++) curve &= fabs(o[i][1] - pq_code(shoulder(pq_nits(g[i][1]), 300, 400, 1000))) <= 1.0;
        CHECK("roll-off: below the knee (100, 203 nits) the code values exactly as they are", ok && same);
        CHECK("roll-off: above it rising and at most the target (400, within one PQ code: 403)", ok && mono && under);
        CHECK("roll-off: 300, 350, 400 and 1000 nits on the shoulder (slope 1 at the knee, the source's peak 1000 onto the target)", ok && curve && fabs(pq_nits((unsigned)lround(o[5][1])) - 400) < 4);
        unsigned col[N][3]; for (int i = 0; i < N; i++) { col[i][0] = pq_code(1000); col[i][1] = pq_code(500); col[i][2] = pq_code(100); }
        ok = run(src10(col), MTLPixelFormatRGB10A2Unorm, o);
        double r = pq_nits((unsigned)lround(o[0][0])), gg = pq_nits((unsigned)lround(o[0][1])), b = pq_nits((unsigned)lround(o[0][2]));
        CHECK("roll-off: a color (1000, 500, 100 nits) keeps its hue: all channels scaled alike", ok && r <= 403 && fabs(gg / r - 0.5) < 0.03 && fabs(b / r - 0.1) < 0.01);
        // Scaled up (8 to 13 texels), the bilinear samples fall between texels: none above the knee is still the window's as it is, bit for bit the passthrough's
        {
            const double lo[N] = {0.1, 10, 80, 120, 203, 250, 280, 290};
            unsigned lw[N][3]; for (int i = 0; i < N; i++) lw[i][0] = lw[i][1] = lw[i][2] = pq_code(lo[i]);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:13 height:1 mipmapped:NO];
            d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> d4 = [g_dev newTextureWithDescriptor:d], d0 = [g_dev newTextureWithDescriptor:d], s10 = src10(lw);
            mtl_present_layer(13, 1);
            id<MTLCommandBuffer> cb = [g_q commandBuffer]; int ok4 = present_encode(cb, s10, d4, NULL); [cb commit]; [cb waitUntilCompleted];
            range(NULL, 0, 400); cb = [g_q commandBuffer]; int ok0 = present_encode(cb, s10, d0, NULL); [cb commit]; [cb waitUntilCompleted]; range(NULL, 0, 1801);
            uint32_t w4[13], w0[13]; [d4 getBytes:w4 bytesPerRow:sizeof w4 fromRegion:MTLRegionMake2D(0, 0, 13, 1) mipmapLevel:0]; [d0 getBytes:w0 bytesPerRow:sizeof w0 fromRegion:MTLRegionMake2D(0, 0, 13, 1) mipmapLevel:0];
            mtl_present_layer(N, 1);
            CHECK("roll-off: a scaled present below the knee is bit for bit the passthrough", ok4 && ok0 && !memcmp(w4, w0, sizeof w4));
        }
        t_hdr_headroom = 1.05f;
        CHECK("roll-off: none when the screen has no room above the white (headroom 1.05: the knee, 100, above 90 % of 105): clipped", !dm_hdr_rolloff(&k, &t, &sp));
        t_hdr_headroom = 4.0f;
        setenv("DM_HDR_PEAK", "390", 1); Q();
        CHECK("roll-off: none when the peak told fits (390 at headroom 4), whatever the game declares (1801; Mario Kart World declares about 1.8 x its peak)", !dm_hdr_rolloff(&k, &t, &sp));
        setenv("DM_HDR_PEAK", "1000", 1); Q();
        range(NULL, 0, 415);
        CHECK("roll-off: none when the declared range is within 5% of the screen's peak (415 at 400)", !dm_hdr_rolloff(&k, &t, &sp));
        range(NULL, 0, 440);
        CHECK("roll-off: a declared range just above that (440 at 400) is rolled off", dm_hdr_rolloff(&k, &t, &sp) && fabsf(t - 400) < 0.5f && fabsf(sp - 440) < 0.5f);
        range(NULL, 0, 0);
        CHECK("roll-off: a game that has declared nothing is taken at the peak it was told (1000 at 400)", dm_hdr_rolloff(&k, &t, &sp) && fabsf(sp - 1000) < 0.5f);
        range(NULL, 0, 1801); dm_hdr_layer_meta_set(1);
        CHECK("roll-off: none while the front end made the layer with metadata, whatever DM_HDR_META says now (0)", !dm_hdr_rolloff(&k, &t, &sp));
        dm_hdr_layer_meta_set(0); setenv("DM_HDR_META", "1", 1);
        CHECK("roll-off: on while the front end made it without, whatever DM_HDR_META says now (1)", dm_hdr_rolloff(&k, &t, &sp));
        dm_hdr_layer_meta_set(-1); setenv("DM_HDR_META", "0", 1);
        // The automatic peak: told what the screen shows, nothing to roll off; the screen dims (16 to 4) while the new step waits for its hold: the peak told (1580) rolled into 400
        setenv("DM_HDR_PEAK", "auto", 1); setenv("DM_HDR_PEAK_HOLD", "0", 1); t_hdr_headroom = 16.0f; Q(); Q();
        int steady = dm_hdr_rolloff(&k, &t, &sp);
        setenv("DM_HDR_PEAK_HOLD", "100000", 1); t_hdr_headroom = 4.0f; Q();
        int dim = dm_hdr_rolloff(&k, &t, &sp);
        CHECK("roll-off with the automatic peak: none while the screen holds still (1580 told at headroom 16), the peak told rolled into the dimmer screen while its step waits (1580 into 400)", !steady && dim && fabsf(t - 400) < 0.5f && fabsf(sp - 1580) < 0.5f);
        setenv("DM_HDR_PEAK_HOLD", "0", 1); Q();
        CHECK("roll-off with the automatic peak: none once the answer has followed (400 told)", peak == 400 && !dm_hdr_rolloff(&k, &t, &sp));
        setenv("DM_HDR_PEAK", "1000", 1); unsetenv("DM_HDR_PEAK_HOLD"); Q();
        range(NULL, 0, 350);
        CHECK("roll-off: none when the declared range fits (350 at headroom 4)", !dm_hdr_rolloff(&k, &t, &sp));
        range(NULL, 0, 1801); t_hdr_headroom = 1.0f;
        CHECK("roll-off: none on a screen without headroom, whatever the game declares (1801)", !dm_hdr_rolloff(&k, &t, &sp));
        t_hdr_headroom = 16.0f; range(NULL, 0, 350);
        ok = run(src10(gray), MTLPixelFormatRGB10A2Unorm, o); int bad = 0;
        for (int i = 0; ok && i < N; i++) for (int c = 0; c < 3; c++) bad += fabs(o[i][c] - gray[i][c]) > 0.5;
        CHECK("roll-off: then the HDR10 window's code values as they are again", ok && !bad);
        unsetenv("DM_HDR_META"); range(NULL, 0, 1000);
    }
    // A screenshot of an HDR frame (an 8-bit target): converted to BT.709 at the paper white, then what the game may draw (the peak told, 1000) compressed into SDR white above
    // three quarters of it, the brightest channel setting the scale so the hue holds
    {
        unsigned c[N][3]; for (int i = 0; i < N; i++) c[i][0] = c[i][1] = c[i][2] = pq_code(nits[i]);
        c[7][0] = pq_code(1000); c[7][1] = pq_code(300); c[7][2] = 0;
        double s = dm_hdr_source_peak() / 100.0;
        int ok = run(src10(c), MTLPixelFormatRGBA8Unorm, o), bad = 0;
        for (int i = 0; ok && i < 7; i++) bad += fabs(o[i][0] - 255.0 * sdr_enc(shoulder(pq_nits(c[i][0]) / 100.0, 0.75, 1, s))) > 1.0;
        CHECK("screenshot: gray on the shoulder: dark and mid grays as SDR (to 75 nits), brighter grays compressed into SDR white", ok && !bad && o[6][0] >= 254);
        CHECK("screenshot: opaque", ok && t_alpha[0] == 1.0);
        double v[3] = {pq_nits(c[7][0]) / 100.0, pq_nits(c[7][1]) / 100.0, 0}, e[3], m709[3];
        static const double m[3][3] = {{1.660491, -0.587641, -0.072850}, {-0.124550, 1.132900, -0.008349}, {-0.018151, -0.100579, 1.118730}};
        for (int k = 0; k < 3; k++) m709[k] = fmax(m[k][0] * v[0] + m[k][1] * v[1] + m[k][2] * v[2], 0);
        double mx = fmax(fmax(m709[0], m709[1]), m709[2]), f = shoulder(mx, 0.75, 1, s) / mx;
        for (int k = 0; k < 3; k++) e[k] = 255.0 * sdr_enc(fmin(m709[k] * f, 1));
        double rg_lin = sdr_dec(o[7][0] / 255.0) / sdr_dec(o[7][1] / 255.0);
        CHECK("screenshot: an orange highlight (BT.2020 1000, 300, 0 nits) keeps its BT.709 hue: converted first, then compressed (red to green 6.9 kept, not 4.6)",
              ok && fabs(o[7][0] - e[0]) <= 1.5 && fabs(o[7][1] - e[1]) <= 1.5 && fabs(o[7][2] - e[2]) <= 1.5 && fabs(rg_lin - m709[0] / m709[1]) < 0.4);
    }
    // The present pipeline: one per target format, kept, so a screenshot (BGRA8) beside the HDR layer (RGB10A2) makes none again
    {
        unsigned c[N][3]; for (int i = 0; i < N; i++) c[i][0] = c[i][1] = c[i][2] = 512;
        for (int i = 0; i < 4; i++) { run(src10(c), MTLPixelFormatRGB10A2Unorm, o); run(src10(c), MTLPixelFormatBGRA8Unorm, o); }
        NSUInteger before = g_blit_pso.count; id<MTLRenderPipelineState> p10 = g_blit_pso[@(MTLPixelFormatRGB10A2Unorm)];
        run(src10(c), MTLPixelFormatBGRA8Unorm, o); run(src10(c), MTLPixelFormatRGB10A2Unorm, o);
        CHECK("present pipeline: alternating formats makes none again (one per format, kept)", g_blit_pso.count == before && g_blit_pso[@(MTLPixelFormatRGB10A2Unorm)] == p10 && p10);
    }
    // The HDR screenshot beside the SDR one (shot_raw): a 10-bit HEIC tagged BT.2100 PQ that decodes to the window's code values (lossy: within 3 of 1023), and the raw words
    {
        enum { S = 64 }; uint32_t wv[S * S]; unsigned want[S][3];
        for (int x = 0; x < S; x++) { unsigned g = (unsigned)(x * 1023 / (S - 1)); want[x][0] = want[x][1] = want[x][2] = g; }
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) wv[y * S + x] = want[x][0] | want[x][1] << 10 | want[x][2] << 20 | 3u << 30;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGB10A2Unorm width:S height:S mipmapped:NO];
        d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; [t replaceRegion:MTLRegionMake2D(0, 0, S, S) mipmapLevel:0 withBytes:wv bytesPerRow:S * 4];
        char base[1024]; snprintf(base, sizeof base, "%s/t_present_hdr_shot.png", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        char heic[1024], raw[1024]; snprintf(heic, sizeof heic, "%.*s.heic", (int)strlen(base) - 4, base); snprintf(raw, sizeof raw, "%s.raw", base);
        int wrote = shot_raw(t, base), ok = 0, deep = 0, pqcs = 0, bad = -1, white = 0;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)heic, strlen(heic), false);
        CGImageSourceRef isrc = url ? CGImageSourceCreateWithURL(url, NULL) : NULL; CGImageRef img = isrc ? CGImageSourceCreateImageAtIndex(isrc, 0, NULL) : NULL;
        if (img) {
            deep = CGImageGetBitsPerComponent(img) > 8; CFStringRef nm = CGColorSpaceCopyName(CGImageGetColorSpace(img));
            pqcs = CGColorSpaceUsesITUR_2100TF(CGImageGetColorSpace(img)) && CGColorSpaceIsWideGamutRGB(CGImageGetColorSpace(img)); if (nm) CFRelease(nm);   /* decoded from the file's color description: PQ transfer, BT.2020 primaries (no name) */
            CGColorSpaceRef pq = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ); uint16_t *px = calloc(S * S * 4, 2);
            CGContextRef ctx = CGBitmapContextCreate(px, S, S, 16, S * 8, pq, kCGBitmapByteOrder16Little | (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
            if (ctx) { CGContextDrawImage(ctx, CGRectMake(0, 0, S, S), img); ok = 1; bad = 0;
                for (int x = 0; x < S; x++) for (int k = 0; k < 3; k++) bad += fabs(px[(S / 2) * S * 4 + x * 4 + k] / 65535.0 * 1023 - pq_code(fmin(pq_nits(want[x][k]) * 2.03, 10000))) > 3; }   // the paper white (100) re-encoded at 203
            CGContextRelease(ctx); CGColorSpaceRelease(pq); free(px);
            // As Apple's image path shows it (extended linear sRGB, SDR white 1.0): each level at its light over the paper white, so the game's white is SDR white, not half of it
            CGColorSpaceRef ext = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB); float *fx = calloc(S * S * 4, 4);
            CGContextRef fc = CGBitmapContextCreate(fx, S, S, 32, S * 16, ext, kCGBitmapByteOrder32Little | kCGBitmapFloatComponents | (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
            if (fc) { CGContextDrawImage(fc, CGRectMake(0, 0, S, S), img); white = 1;
                for (int x = 0; x < S; x++) { double n = pq_nits(want[x][1]); if (n > 10 && n < 2000) white &= fabs(fx[(S / 2) * S * 4 + x * 4 + 1] - n / 100.0) <= 0.05 * n / 100.0 + 0.002; } }
            CGContextRelease(fc); CGColorSpaceRelease(ext); free(fx); CGImageRelease(img);
        }
        if (isrc) CFRelease(isrc); if (url) CFRelease(url);
        FILE *rf = fopen(raw, "rb"); char hdr[64] = ""; uint32_t w0 = 0; if (rf) { if (!fgets(hdr, sizeof hdr, rf) || fread(&w0, 4, 1, rf) != 1) w0 = ~0u; fclose(rf); }
        CHECK("HDR screenshot: a 10-bit HEIC tagged BT.2100 PQ, decoding to the window's levels with the paper white at 203 (within 3 of 1023)", wrote && ok && deep && pqcs && bad == 0);
        CHECK("HDR screenshot: as Apple's image path shows it, each level at its light over the paper white (100; white = SDR white, not half of it)", white);
        CHECK("HDR screenshot: path.raw holds the words as stored", !strcmp(hdr, "rgb10a2 64 64\n") && w0 == wv[0]);
        unlink(heic); unlink(raw);
    }
    // MetalFX's spatial scaler takes no HDR source (present_hdr_source): the HDR10 window into the PQ layer or rolled off or brought to SDR, a float window into the PQ layer; an SDR window does
    CHECK("MetalFX: an HDR source is scaled bilinearly (modes 0 with the HDR10 window, 1, 3, 4); an SDR window is not one (modes 0, 2)",
          present_hdr_source(MTLPixelFormatRGB10A2Unorm, 0) && present_hdr_source(MTLPixelFormatRGB10A2Unorm, 1) && present_hdr_source(MTLPixelFormatRGBA16Float, 3) && present_hdr_source(MTLPixelFormatRGB10A2Unorm, 4) &&
          !present_hdr_source(MTLPixelFormatBGRA8Unorm, 0) && !present_hdr_source(MTLPixelFormatRGBA8Unorm, 2));
    #undef Q
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
