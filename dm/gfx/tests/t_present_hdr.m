// HDR output (DM_HDR=1): the status query's answer and the present's decode, on the GPU with closed-form expectations (no game, no window).
// The vi answer: HDR on only while the window's screen can show EDR, format 1 with it (the SDK's on = format == 1), the decode latched once answered. The present: PQ code values
// (an RGB10A2 window) written as they are into the PQ layer (RGB10A2 in BT.2100 PQ), rolled off above the screen's peak without metadata, and SDR and float sources encoded
// for it at the paper white; a screenshot of an HDR frame brought to SDR white with its hue, and a 10-bit window of a game that never asked for HDR read as SDR. hle_vi.c is linked beside this file; t_present_hdr_front.c stands in for the front end
#include "../nvn/nvn_mtl.m"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
int dm_hdr_meta(void);
extern int t_hdr_screen; extern unsigned t_hdr_lo, t_hdr_hi; extern float t_hdr_headroom;
int vi_hdr_answered(void); int dm_hdr_mode(void); void dm_hdr_set_mode(int m);
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
static int run(id<MTLTexture> src, MTLPixelFormat fmt, double out[N][3]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:N height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> dst = [g_dev newTextureWithDescriptor:d]; id<MTLCommandBuffer> cb = [g_q commandBuffer];
    if (!present_encode(cb, src, dst, NULL)) return 0;
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) return 0;
    if (fmt == MTLPixelFormatRGB10A2Unorm) {   // the PQ layer: 10-bit code values
        uint32_t w[N]; [dst getBytes:w bytesPerRow:sizeof w fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int i = 0; i < N; i++) for (int k = 0; k < 3; k++) out[i][k] = w[i] >> (10 * k) & 0x3ff;
    } else {
        uint8_t b[N * 4]; [dst getBytes:b bytesPerRow:sizeof b fromRegion:MTLRegionMake2D(0, 0, N, 1) mipmapLevel:0];
        for (int i = 0; i < N; i++) for (int k = 0; k < 3; k++) out[i][k] = b[4 * i + k];
    }
    return 1;
}
static int near(double a, double b, double rel, double abs_) { return fabs(a - b) <= fmax(abs_, rel * fabs(b)); }

int main(void) { @autoreleasepool {
    setenv("DM_HDR", "1", 1); setenv("DM_HDR_PAPER_WHITE", "203", 1); setenv("DM_HDR_PEAK", "1000", 1); unsetenv("DM_HDR_META");   // the checks below are worked out at the paper white 203 (read once per run; the default, 100,
    // is the settings row's: test_settings.py) and a fixed peak (unset, automatic on the Mac, is checked below)
    mtl_init();
    mtl_present_layer(N, 1);   // a layer of the fixtures' N x 1 shape, so each source texel lands on its own target texel: left to the display's 16:9, the picture is a bar in the middle (present_shape)
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
        CHECK("not answered: into the PQ layer it is decoded as gamma-coded SDR and encoded at 100 nits, the screen's white (no HDR answered yet), not taken as PQ", run(src10(c), MTLPixelFormatRGB10A2Unorm, o) && fabs(o[0][1] - pq_code(srgb_dec(512 / 1023.0) * 100)) <= 1.5);
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

    // HDR output changed while the game runs (the iOS in-game menu, dm_hdr_set_mode): the next status query follows, DM_HDR=1 having been read at start
    {
        uint8_t on = 9; uint32_t fmt = 9; uint16_t peak = 9;
        int m0 = dm_hdr_mode();
        dm_hdr_set_mode(0); status(&on, &fmt, &peak, NULL);
        CHECK("turned off while the game runs: the next query answers SDR (on 0, format 0, peak 0)", m0 == 1 && dm_hdr_mode() == 0 && on == 0 && fmt == 0 && peak == 0);
        dm_hdr_set_mode(1); status(&on, &fmt, &peak, NULL);
        CHECK("turned on again: HDR on, format 1, peak 1000", on == 1 && fmt == 1 && peak == 1000);
        t_hdr_screen = 0; dm_hdr_set_mode(2); status(&on, &fmt, &peak, NULL);
        CHECK("any non-zero mode is on, by the screen: SDR on a screen without EDR", dm_hdr_mode() == 1 && on == 0 && fmt == 0);
        dm_hdr_set_mode(1); t_hdr_screen = 1;
    }

    // The peak (the HDR peak row): a number as it is; DM_HDR_PEAK=auto, 100 nits times the screen's current headroom (what a PQ layer shows unchanged, EDR = nits / 100), in steps
    // of about 6 % (40 to a decade), at least twice the paper white (203 here) and at most 10,000, 1000 with no headroom known; a new step is taken once it has held for
    // DM_HDR_PEAK_HOLD ms (0 here, so on the second query that sees it). SDR still answers peak 0. DM_HDR_PEAK is read at every query, so the cases can follow one another
    {
        uint8_t on = 9; uint32_t fmt = 9; uint16_t peak = 9;
        #define Q2() do { status(&on, &fmt, &peak, NULL); status(&on, &fmt, &peak, NULL); } while (0)
        setenv("DM_HDR_PEAK", "1600", 1); status(&on, &fmt, &peak, NULL);
        CHECK("DM_HDR_PEAK=1600: peak 1600", on == 1 && peak == 1600);
        setenv("DM_HDR_PEAK", "auto", 1); setenv("DM_HDR_PEAK_HOLD", "0", 1);
        t_hdr_headroom = 16.0f; Q2();
        CHECK("automatic, headroom 16 (the XDR screen at half brightness): peak 1580 (1600 to the step)", on == 1 && fmt == 1 && peak == 1580);
        t_hdr_headroom = 6.45f; Q2();
        CHECK("automatic, headroom 6.45: peak 630 (645 to the step)", peak == 630);
        t_hdr_headroom = 4.0f; Q2();
        CHECK("automatic, headroom 4 (full brightness): at least twice the paper white, 410", peak == 410);
        t_hdr_headroom = 1000.0f; Q2();
        CHECK("automatic, a headroom of 1000: at most 10,000", peak == 10000);
        t_hdr_headroom = 0.0f; Q2(); uint16_t p0 = peak; t_hdr_headroom = NAN; Q2();
        CHECK("automatic with no headroom known (0 or not a number): 1000", p0 == 1000 && peak == 1000);
        setenv("DM_HDR_PEAK_HOLD", "100000", 1); t_hdr_headroom = 16.0f; Q2(); Q2();
        CHECK("automatic: a new step waits for the hold (100 s here): still the last automatic answer, 10,000 (no headroom known answers 1000 without moving it)", peak == 10000);
        setenv("DM_HDR_PEAK_HOLD", "0", 1); Q2();
        CHECK("automatic: then taken once it has held: 1580", peak == 1580);
        setenv("DM_HDR_PEAK_HOLD", "100000", 1); t_hdr_headroom = 4.0f; Q2(); uint16_t held = peak; t_hdr_headroom = 6.45f; status(&on, &fmt, &peak, NULL); uint16_t held2 = peak;
        setenv("DM_HDR_PEAK_HOLD", "0", 1); status(&on, &fmt, &peak, NULL);
        CHECK("automatic: a step that changes before its hold ends restarts the hold (1580 kept while 410, then 630, wait), and the newer one is taken (630)", held == 1580 && held2 == 1580 && peak == 630);
        setenv("DM_HDR_PEAK", "Auto", 1); t_hdr_headroom = 16.0f; Q2(); uint16_t pa = peak; setenv("DM_HDR_PEAK", "abc", 1); Q2(); uint16_t pb = peak; setenv("DM_HDR_PEAK", "-1", 1); Q2();
        CHECK("DM_HDR_PEAK that is neither nits nor auto (Auto, abc, -1) counts as unset: automatic on the Mac (1580), never a peak of 0", pa == 1580 && pb == 1580 && peak == 1580);
        setenv("DM_HDR_PEAK", "0", 1); Q2(); uint16_t pz = peak; setenv("DM_HDR_PEAK", "12abc", 1); Q2(); uint16_t pj = peak; setenv("DM_HDR_PEAK", "70000", 1); Q2(); uint16_t pc = peak;
        setenv("DM_HDR_PEAK", "", 1); Q2(); uint16_t pe = peak; setenv("DM_HDR_PEAK", "1000", 1); Q2();
        CHECK("DM_HDR_PEAK 0, 12abc and empty count as unset (1580), 70000 is held to 65535, 1000 is 1000", pz == 1580 && pj == 1580 && pe == 1580 && pc == 65535 && peak == 1000);
        t_hdr_headroom = 16.0f; setenv("DM_HDR_PEAK", "auto", 1); Q2(); t_hdr_screen = 0; t_hdr_headroom = 1.0f; Q2(); Q2(); t_hdr_screen = 1; t_hdr_headroom = 16.0f; status(&on, &fmt, &peak, NULL);
        CHECK("automatic: queries on a screen without EDR (headroom 1) leave the held peak alone (1580 again at once on the way back)", peak == 1580);
        setenv("DM_HDR_PEAK", "auto", 1);
        t_hdr_headroom = 2.0f; t_hdr_screen = 0; status(&on, &fmt, &peak, NULL);
        CHECK("automatic on a screen without EDR: SDR, peak 0", on == 0 && peak == 0);
        t_hdr_screen = 1; t_hdr_headroom = 1.0f; unsetenv("DM_HDR_PEAK");
        Q2(); uint16_t pu = peak; t_hdr_headroom = 0.0f; Q2();
        CHECK("unset: automatic on the Mac (headroom 1: the floor, twice the paper white, 410; no headroom known: 1000)", pu == 410 && peak == 1000);
        CHECK("the metadata is off by default on the Mac (DM_HDR_META unset)", !dm_hdr_meta());
        setenv("DM_HDR_PEAK", "1000", 1); unsetenv("DM_HDR_PEAK_HOLD"); t_hdr_headroom = 1.0f;
        status(&on, &fmt, &peak, NULL);
        CHECK("DM_HDR_PEAK=1000 again: peak 1000", peak == 1000);
        #undef Q2
    }

    const double nits[N] = {0, 0.1, 10, 80, 203, 500, 1000, 10000};
    unsigned grey[N][3]; double want[N];
    for (int i = 0; i < N; i++) { grey[i][0] = grey[i][1] = grey[i][2] = pq_code(nits[i]); want[i] = pq_nits(grey[i][0]) / 203.0; }

    // The PQ layer (RGB10A2 in BT.2100 PQ, the HDR layer): the game's HDR10 window goes in as it is, an 8-bit sRGB source is encoded as HDR10 with its white at the paper white
    {
        // a screen that shows the declared 0..1000 (without metadata, the Mac's default, a range above the screen's held peak is rolled off; checked below), its peak settled
        int dm_hdr_rolloff(float *knee, float *target); float k, t;
        t_hdr_headroom = 16.0f; setenv("DM_HDR_PEAK_HOLD", "0", 1); dm_hdr_rolloff(&k, &t); dm_hdr_rolloff(&k, &t);
        int ok = run(src10(grey), MTLPixelFormatRGB10A2Unorm, o), bad = 0;
        t_hdr_headroom = 1.0f; unsetenv("DM_HDR_PEAK_HOLD");
        for (int i = 0; ok && i < N; i++) for (int k = 0; k < 3; k++) bad += fabs(o[i][k] - grey[i][k]) > 0.5;
        CHECK("PQ layer: the HDR10 window's code values are written as they are (the declared range within the screen)", ok && !bad);
        uint8_t c[N][3] = {{255, 255, 255}, {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 0, 0}, {64, 64, 64}, {255, 255, 255}};
        ok = run(src8(c), MTLPixelFormatRGB10A2Unorm, o);
        CHECK("PQ layer: SDR white is the paper white (203 nits), black is code 0", ok && fabs(o[0][0] - pq_code(203)) <= 1.5 && fabs(o[0][2] - pq_code(203)) <= 1.5 && o[5][0] <= 1);
        CHECK("PQ layer: SDR mid grey 128 is its linear value at the paper white", ok && fabs(o[1][1] - pq_code(srgb_dec(128 / 255.0) * 203)) <= 1.5);
        float f[N][3] = {{1, 1, 1}, {0.5f, 0.5f, 0.5f}, {4, 4, 4}, {0, 0, 0}, {-1, -1, -1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}};
        ok = run(src16f(f), MTLPixelFormatRGB10A2Unorm, o);
        CHECK("PQ layer: a float source is linear light, 1.0 at the paper white, 4.0 at four times it, not decoded as sRGB", ok && fabs(o[0][1] - pq_code(203)) <= 1.5 && fabs(o[1][1] - pq_code(101.5)) <= 1.5 &&
              fabs(o[2][1] - pq_code(812)) <= 1.5);
        CHECK("PQ layer: a negative float value is black", ok && o[3][1] <= 1 && o[4][1] <= 1);
        { uint8_t on; uint32_t fmt; uint16_t pk; t_hdr_screen = 0; status(&on, &fmt, &pk, NULL);   // the status answers SDR: a screen without EDR
          ok = run(src8(c), MTLPixelFormatRGB10A2Unorm, o);
          CHECK("PQ layer: while the status answers SDR (a screen without EDR), SDR white is 100 nits, the screen's white, not the paper white (203)", ok && on == 0 && fabs(o[0][0] - pq_code(100)) <= 1.5 && fabs(o[1][1] - pq_code(srgb_dec(128 / 255.0) * 100)) <= 1.5);
          t_hdr_screen = 1; status(&on, &fmt, &pk, NULL); }
    }
    // The PQ layer without metadata (DM_HDR_META=0): a game that may draw more than the screen shows is rolled off above the knee, three quarters of the screen's peak, smoothly
    // into that peak (present.metal mode 4), keyed on the smaller of the declared range and the peak told. The screen's peak is held as the automatic answer holds it: at
    // headroom 4 that is 410 (400 to the step, at least twice the paper white 203), the knee 307.5; declared 1801 (Mario Kart World at its title), told 1000
    {
        int dm_hdr_rolloff(float *knee, float *target); void dm_hdr_layer_meta_set(int on);
        float k = 0, t = 0;
        #define RO() (dm_hdr_rolloff(&k, &t), dm_hdr_rolloff(&k, &t))   // twice: the held peak takes a new step on its second reading (DM_HDR_PEAK_HOLD=0)
        setenv("DM_HDR_PEAK_HOLD", "0", 1);
        { uint8_t on; uint32_t fmt; uint16_t pk; status(&on, &fmt, &pk, NULL); }   // the peak told: 1000 (DM_HDR_PEAK=1000)
        t_hdr_headroom = 4.0f; range(NULL, 0, 1801);
        setenv("DM_HDR_META", "1", 1); int with_meta = RO();
        setenv("DM_HDR_META", "0", 1);
        int on1801 = RO();
        CHECK("roll-off: only without metadata, knee 307.5 and peak 410 for a declared 1801 at headroom 4", !with_meta && on1801 && fabsf(k - 307.5f) < 0.5f && fabsf(t - 410) < 0.5f);
        const double rn[N] = {100, 203, 300, 350, 400, 1000, 1801, 10000};
        unsigned g[N][3]; for (int i = 0; i < N; i++) g[i][0] = g[i][1] = g[i][2] = pq_code(rn[i]);
        int ok = run(src10(g), MTLPixelFormatRGB10A2Unorm, o), same = 1, mono = 1, under = 1;
        for (int i = 0; ok && i < 3; i++) for (int c = 0; c < 3; c++) same &= fabs(o[i][c] - g[i][c]) <= 0.5;
        for (int i = 0; ok && i < N; i++) { under &= pq_nits((unsigned)lround(o[i][1])) <= 413; if (i) mono &= o[i][1] >= o[i - 1][1]; }
        double n350 = pq_nits((unsigned)lround(o[3][1])), n400 = pq_nits((unsigned)lround(o[4][1]));
        CHECK("roll-off: below the knee (100, 203, 300 nits) the code values as they are", ok && same);
        CHECK("roll-off: above it rising and at most the peak (410, within one PQ code: 413)", ok && mono && under);
        CHECK("roll-off: 350 nits to about 342 and 400 to about 368 (slope 1 at the knee, towards the peak)", ok && fabs(n350 - 342.3) < 7 && fabs(n400 - 368.4) < 7);
        unsigned col[N][3]; for (int i = 0; i < N; i++) { col[i][0] = pq_code(1000); col[i][1] = pq_code(500); col[i][2] = pq_code(100); }
        ok = run(src10(col), MTLPixelFormatRGB10A2Unorm, o);
        double r = pq_nits((unsigned)lround(o[0][0])), gg = pq_nits((unsigned)lround(o[0][1])), b = pq_nits((unsigned)lround(o[0][2]));
        CHECK("roll-off: a colour (1000, 500, 100 nits) keeps its hue: all channels scaled alike", ok && r <= 413 && fabs(gg / r - 0.5) < 0.03 && fabs(b / r - 0.1) < 0.01);
        t_hdr_headroom = 1.2f;
        CHECK("roll-off: with a fixed peak while the headroom ramps up (1.2): the target stays the held floor, 410, the knee above the game's 203-nit white", RO() && fabsf(t - 410) < 0.5f && k > 203);
        t_hdr_headroom = 4.0f;
        setenv("DM_HDR_PEAK", "390", 1); { uint8_t on; uint32_t fmt; uint16_t pk; status(&on, &fmt, &pk, NULL); }
        CHECK("roll-off: none when the peak told fits (390 at headroom 4), whatever the game declares (1801; Mario Kart World declares about 1.8 x its peak)", !RO());
        setenv("DM_HDR_PEAK", "1000", 1); { uint8_t on; uint32_t fmt; uint16_t pk; status(&on, &fmt, &pk, NULL); }
        range(NULL, 0, 420);
        CHECK("roll-off: none when the declared range is within 5% of the screen's peak (420 at 410)", !RO());
        range(NULL, 0, 440);
        CHECK("roll-off: a declared range just above that (440 at 410) is rolled off", RO() && fabsf(t - 410) < 0.5f);
        range(NULL, 0, 0);
        CHECK("roll-off: a game that has declared nothing is taken at the peak it was told (1000 at 410)", RO());
        range(NULL, 0, 1801); dm_hdr_layer_meta_set(1);
        CHECK("roll-off: none while the front end made the layer with metadata, whatever DM_HDR_META says now (0)", !RO());
        dm_hdr_layer_meta_set(0); setenv("DM_HDR_META", "1", 1);
        CHECK("roll-off: on while the front end made it without, whatever DM_HDR_META says now (1)", RO());
        dm_hdr_layer_meta_set(-1); setenv("DM_HDR_META", "0", 1);
        setenv("DM_HDR_PEAK", "auto", 1); t_hdr_headroom = 1.2f;
        { uint8_t on; uint32_t fmt; uint16_t pk; status(&on, &fmt, &pk, NULL); }
        CHECK("roll-off: none with the automatic peak, while the headroom ramps (1.2) and the game declares more (1801)", !RO());
        setenv("DM_HDR_PEAK", "1000", 1); t_hdr_headroom = 4.0f; { uint8_t on; uint32_t fmt; uint16_t pk; status(&on, &fmt, &pk, NULL); }
        range(NULL, 0, 350);
        CHECK("roll-off: none when the declared range fits (350 at headroom 4)", !RO());
        range(NULL, 0, 1801); t_hdr_screen = 0;
        CHECK("roll-off: none on a screen without EDR, whatever the game declares (1801)", !RO());
        t_hdr_screen = 1; range(NULL, 0, 350);
        ok = run(src10(grey), MTLPixelFormatRGB10A2Unorm, o); int bad = 0;
        for (int i = 0; ok && i < N; i++) for (int c = 0; c < 3; c++) bad += fabs(o[i][c] - grey[i][c]) > 0.5;
        CHECK("roll-off: then the HDR10 window's code values as they are again", ok && !bad);
        unsetenv("DM_HDR_META"); unsetenv("DM_HDR_PEAK_HOLD"); range(NULL, 0, 1000); t_hdr_headroom = 1.0f;
        #undef RO
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
