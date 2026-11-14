// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
#include <math.h>
#include <stdatomic.h>
#include <time.h>

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h_vi_Init, void) {}
// The Switch 2 SDKs' variant with low frame rate compensation for a variable refresh rate display: nn::vi::Initialize's work through vi2::InitializeWithVrrLfcEnabled, whose
// VRR hooks ask the graphics system's user session, which the host never opens (Resident Evil 4 v131072, SDK 21.4.3: main+0xb180288 → sdk+0x5d8cf0, a null read in SdkMutexType::Lock).
// The host has no vi service and no VRR display (QueryDisplayVrrSupported below): the same as nn::vi::Initialize
HLE_FORCE("_ZN2nn2vi27InitializeWithVrrLfcEnabledEv", "display service init with VRR low frame rate compensation; the host has no vi service and no VRR", void, h_vi_InitVrrLfc, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
// A display returned by ListDisplays can also be opened by name. The SDK's OpenDisplay still reaches the absent vi service even though Initialize is taken over
// (Alien: Isolation, SDK 8.2.0: null read at sdk+0x3b65e4). Share the default display object with OpenDefaultDisplay.
HLE_FORCE("_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc", "open the named Default display; the host has no vi service", Result, h_vi_OpenNamedDisp, void **out, const char *name) {
    if (!name || strcmp(name, "Default")) return RES_FAIL;
    return h_vi_OpenDisp(out);
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer; the host returns a static object", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
// The overload with the layer's size (SDL3's Switch video driver, Absolum: 1920x1080 docked); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0xe4).
// An NVN or Vulkan window's textures decide what is presented; an EGL window surface takes the layer's size (dm/gfx/gl/hle_gl.m: Digimon Story Cyber Sleuth sizes its layer
// itself and draws into the surface at that size)
// The present shows the picture in the layer's shape (nvn_mtl.m present_shape)
void mtl_present_layer(uint32_t w, uint32_t h);
static _Atomic uint32_t g_layer_wh;   // w | h << 16 of the last sized CreateLayer, 0 = none
void dm_vi_layer_size(int *w, int *h) { uint32_t v = g_layer_wh; *w = (int)(v & 0xffff); *h = (int)(v >> 16); }
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "create layer of a given size; the host returns a static object", Result, h_vi_CreateLayerWH, void **out, void *disp, int w, int h) {
    (void)disp; LOG("vi::CreateLayer %dx%d", w, h); *out = &g_layer;
    if (w > 0 && h > 0 && w < 0x10000 && h < 0x10000) { g_layer_wh = (uint32_t)w | (uint32_t)h << 16; mtl_present_layer((uint32_t)w, (uint32_t)h); }
    return RES_OK;
}
// SDK 22's overload with a LayerCreationSettings, which its constructor LayerCreationSettings(int w, int h) fills as {w, h, 2} (sdk+0x4526a0 in EA SPORTS FC 27; the SDK's own
// constructor runs). The SDK's CreateLayer goes to am::GetSelfController(), which the host never made (SIGSEGV at the load from it, sdk+0x44ea94)
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEPKNS0_21LayerCreationSettingsE", "create layer from creation settings (SDK 22); the host returns a static object",
          Result, h_vi_CreateLayerSettings, void **out, void *disp, const int32_t *settings) {
    return h_vi_CreateLayerWH(out, disp, settings ? settings[0] : 0, settings ? settings[1] : 0);
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64).
// The crop is kept for the present: the Vulkan present (hle_vk.m), where Super Mario Galaxy keeps a 1920x1080 swapchain in handheld mode, draws into its top-left 1280x720 and
// crops the layer to that every frame; and the NVN present when the game sets no window crop of its own (nvnWindowSetCrop; present_crop in hle_nvn.c), where Link's Awakening
// draws 1280x720 into its 1920x1080 window textures in handheld mode and crops the layer to that. It only picks texels: the present stretches them to the layer's shape, as the
// Switch's compositor does (nvn_mtl.m present_shape). Logged only when it changes
static _Atomic uint64_t g_crop;   // x | y << 16 | w << 32 | h << 48, 0 = none
void dm_vi_layer_crop(int c[4]) { uint64_t v = g_crop; c[0] = (int)(v & 0xffff); c[1] = (int)(v >> 16 & 0xffff); c[2] = (int)(v >> 32 & 0xffff); c[3] = (int)(v >> 48); }
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "layer crop; the host records it and the present crops by it", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer;
    uint64_t v = x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xffff || y > 0xffff || w > 0xffff || h > 0xffff ? 0 : (uint64_t)x | (uint64_t)y << 16 | (uint64_t)w << 32 | (uint64_t)h << 48;
    if (atomic_exchange(&g_crop, v) != v) LOG("vi::SetLayerCrop %d,%d %dx%d", x, y, w, h);
    return RES_OK;
}
// Super Mario Odyssey sets its layer's scaling mode; the SDK's version uses the vi service objects the host never made
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; LOG("vi::SetLayerScalingMode %d (ignored)", mode); return RES_OK;
}
// Unreal Engine paces its frames by the layer's frame number (Octopath Traveler 0, main+0x10140e8); the SDK's versions read the Android surface of the vi service the host never made
// (SIGSEGV in android::Surface::getQueuedFrameNumber). The host's frame number is the count of frames the game has presented
uint64_t dm_nvn_frame_count(void);
HLE_FORCE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", "the layer's latest frame number: the frames presented so far", Result, h_vi_FrameNo, uint64_t *out, const void *layer) {
    (void)layer; *out = dm_nvn_frame_count(); return RES_OK;
}
// The layer's frame history: the last 8 frames presented, newest first, each with whether it reached the screen. Unreal Engine 5's Switch RHI tracks its flips by it: its
// "Viewport Thread" (Minecraft Dungeons II main+0x142afb0) waits for each vsync, asks how many entries there are (a null buffer: the SDK answers 8, the history's size), reads them and
// takes the first one shown as the latest flip, which completes the frame-end fences that wait for the swap chain (r.GTSyncType 2). With no history no flip ever came, and the first
// FlushRenderingCommands, which waits for every fence (FFrameEndSync, main+0x15015f0), waited for ever right after the opening movie: the game thread polled its fence each ms, nothing
// was presented again. FrameInfo is 40 bytes, as the SDK's ListFrameHistory (sdk 20.5.15, sdk+0x4510a0) fills it from the compositor's records: status (2 shown with a present
// time, 1 queued and not shown yet), frame number (the numbering of GetLatestFrameNumber), the queue tick and the present tick twice; sorted by frame number, newest first.
// A frame is shown once the present queue has handed it on (nvn_mtl.m g_pq_done) and the system did not drop it; its present time is the drawable's presented time when Metal
// reports one, else (headless, or before the handler runs) the time it was first seen handed on
#define VFH 8
typedef struct { uint64_t frame, ticket, queued_ns, seen_ns; } vfh_ent;
static vfh_ent g_vfh[VFH]; static pthread_mutex_t g_vfh_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_present_progress(uint64_t *queued, uint64_t *done);   // nvn_mtl.m
double mtl_presented_time(uint64_t n);                         // nvn_mtl.m: CACurrentMediaTime seconds (the now_ns clock), 0 not yet, -1 dropped by the system
void dm_vi_frame_queued(uint64_t frame) {   // hle_nvn.c, at each present the game queues: frame = GetLatestFrameNumber's count with it
    uint64_t q, d; mtl_present_progress(&q, &d);
    pthread_mutex_lock(&g_vfh_mu); g_vfh[frame % VFH] = (vfh_ent){frame, q, guest_ns(), 0}; pthread_mutex_unlock(&g_vfh_mu);
}
static uint64_t vfh_ticks(uint64_t ns) { return dm_sdk_ounce() ? ns / 32 : ns * 12 / 625; }   // as guest_ticks (hle_os.c)
HLE_FORCE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", "the layer's frame history: the last 8 presents, newest first, shown once the present queue handed them on", int, h_vi_FrameHist, void *out, int n, const void *layer) {
    (void)layer;
    if (!out || n <= 0) return VFH;
    uint64_t q, d; mtl_present_progress(&q, &d); (void)q;
    uint64_t gnow = guest_ns(), hnow = now_ns(), latest = dm_nvn_frame_count();
    int k = 0;
    pthread_mutex_lock(&g_vfh_mu);
    for (uint64_t f = latest; f > 0 && latest - f < VFH && k < n; f--) {
        vfh_ent *e = &g_vfh[f % VFH]; if (e->frame != f) continue;
        uint64_t *o = (uint64_t *)out + 5 * k++, shown = 0;
        double pt = e->ticket <= d ? mtl_presented_time(e->ticket) : -1;
        if (pt > 0) { uint64_t ago = hnow > (uint64_t)(pt * 1e9) ? hnow - (uint64_t)(pt * 1e9) : 0; shown = gnow > ago ? gnow - ago : 1; }
        else if (pt == 0) { if (!e->seen_ns) e->seen_ns = gnow; shown = e->seen_ns; }
        o[0] = shown ? 2 : 1; o[1] = f; o[2] = vfh_ticks(e->queued_ns); o[3] = o[4] = shown ? vfh_ticks(shown) : 0;
    }
    pthread_mutex_unlock(&g_vfh_mu);
    static int once; if (k && !once++) LOG("vi::ListFrameHistory: %d of the last %d presents returned (up to frame %llu, the present queue at %llu)", k, VFH, (unsigned long long)latest, (unsigned long long)d);
    return k;
}
// Rhythm Heaven Groove (SDK 21) asks for the built-in panel's identity at start (main+0xa9a4); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0x20).
//   The game reads a u32 kind and, when it is 2 or more, 0x14 more bytes that it hands on; any other answer makes it hand on zeros. The host has no panel: an all-zero identifier, kind 0
HLE_FORCE("_ZN2nn2vi32GetDefaultDisplayPanelIdentifierEPNS0_15PanelIdentifierE", "built-in panel identity; the host has no panel: all zero", Result, h_vi_PanelId, void *out) {
    memset(out, 0, 0x18); return RES_OK;
}
// The Great Ace Attorney Chronicles (SDK 10) lists up to 10 displays before opening the default one (main+0x808f08) and does not read the list. DisplayInfo is 0x60 bytes:
//   name[0x40], bool has a layer limit at +0x40, then u64 layer count limit, largest layer width and height. One display, "Default", as Horizon lists for an application
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list displays; the host has one, \"Default\"", int, h_vi_ListDisp, void *out, int n) {
    if (n <= 0 || !out) return 0;
    uint8_t *p = out; memset(p, 0, 0x60); strcpy((char *)p, "Default");
    p[0x40] = 1; uint64_t lim[3] = {1, 1920, 1080}; memcpy(p + 0x48, lim, sizeof lim);
    return 1;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}

// ---------- Switch 2 SDKs: HDR, VRR and extended swap intervals ----------
// The SDK's versions check the display against its own default display object and forward the layer to vi2, whose layer object the host never made (Donkey Kong Bananza: a call
// through null from SetLayerNextHdrContentGamutMetadata, main+0x2036f4). No VRR on the host, and SDR unless DM_HDR=1. Paper white 100 nits, the screen's own white (DM_HDR_PAPER_WHITE overrides it while HDR is on)
// DM_HDR=1 is the HDR mode (Switch 2 games; the HDR setting of the Mac and iOS launchers sets it): while the window's screen can show EDR (front_hdr_screen), the display reports HDR on, so the
// game takes its HDR path (its window becomes HDR10: RGB10A2 with PQ values in BT.2020). On a screen without EDR it reports SDR, and the game keeps its own SDR picture.
// The HDR layer (RGB10A2 in BT.2100 PQ, front_mac.m and front_ios.m) takes the PQ window as it is; with HDR10 metadata (DM_HDR_META=1) Core Animation maps it into the screen's headroom
// The format is 1 with HDR on and 0 with it off: the SDK's own answer is on = (format == 1), and a format above 1 is an error, 0x272 (Donkey Kong Bananza v458752,
// nn::vi::detail::DefaultDisplay::QueryHdrStatus, sdk+0x55bd60; found in pull request #1249). The peak (DM_HDR_PEAK, automatic by default; vi_hdr_peak below) bounds the extended luminance range the game declares (its metadata call, below). It and the paper white are the launcher's HDR peak and HDR paper white rows: what a console's HDR calibration would set
// Donkey Kong Bananza boots its SDR window (0x25), queries the status, then makes an RGB10A2 window (0x3d) of PQ code values with SDR white at the paper white (202.9 nits told 203).
// Neither it nor Mario Kart World imports the SDK's HDR status event (CreateHdrStatusEvent and the rest), so a game learns of another screen only by asking again: the queries are counted in the log
// A decimal number, clamped to 16 bits (the answers are 16-bit)
static uint32_t vi_env_u32(const char *name, uint32_t def) { const char *e = getenv(name); if (!e || !*e) return def; unsigned long v = strtoul(e, NULL, 10); return v > 65535 ? 65535 : (uint32_t)v; }
// The HDR mode: 0 off, 1 HDR while the window's screen can show EDR. DM_HDR is read the way the front ends read it (atoi: any non-zero number is 1) when first asked; the front end changes it while the game runs (dm_hdr_set_mode: the iOS in-game menu), and the game, which asks for its status every frame,
// switches its window on its next query, as when the window moves to another screen. For Switch 2 games only (dm_sdk_ounce): a Switch 1 game answers SDR
static _Atomic int g_vi_hdr_mode = -1;   // -1 until DM_HDR is read
static _Atomic int g_vi_hdr_live;        // the mode was changed while the game runs: its SDR answers are logged too
int dm_hdr_mode(void) {
    int m = atomic_load(&g_vi_hdr_mode);
    if (m >= 0) return m;
    const char *e = getenv("DM_HDR"); int v = e && *e ? atoi(e) : 0, unread = -1;
    atomic_compare_exchange_strong(&g_vi_hdr_mode, &unread, v != 0);   // a mode set meanwhile by the front end wins
    return atomic_load(&g_vi_hdr_mode);
}
void dm_hdr_set_mode(int m) {
    m = m != 0;
    if (atomic_exchange(&g_vi_hdr_mode, m) != m) { atomic_store(&g_vi_hdr_live, 1); LOG("[nvn2-hdr] HDR output %s while the game runs", m ? "on" : "off"); }
}
static int vi_hdr_on(void) { return dm_hdr_mode() && dm_sdk_ounce(); }
// Whether the window's screen can show EDR (its potential headroom is above 1): the front end reads it before the guest starts and whenever the window's screen changes (front_mac.m, front_ios.m).
// No front end (a windowless run): taken as yes, so DM_HDR alone decides
__attribute__((weak)) int front_hdr_screen(void) { return 1; }
// The game's declared extended luminance range in nits, for the HDR layer's HDR10 metadata (front_mac.m and front_ios.m): Core Animation maps the picture into the screen's headroom by it; none without a front end
__attribute__((weak)) void front_hdr_content_range(unsigned lo, unsigned hi) { (void)lo; (void)hi; }
// The window's screen's current EDR headroom (front_mac.m: NSScreen.maximumExtendedDynamicRangeColorComponentValue; front_ios.m: UIScreen.currentEDRHeadroom; read four times a
// second beside the potential headroom): how many times the SDR white the screen can show right now, which falls as the screen gets brighter and depends on its mode. Unknown (0)
// without a front end (a windowless run): the automatic peak is then 1000, and nothing is rolled off
__attribute__((weak)) float front_hdr_headroom(void) { return 0.0f; }   // 0: unknown (no front end that reads it)
// The paper white in nits, read once (the present and the front ends take it from here too; nvn_mtl.m present_white, and dm_hdr_sdr_white below): a number from 1 to 65535, else 100. 100 nits is the
// screen's own white (macOS shows PQ at EDR = nits / 100), so the game's white matches SDR and the desktop and the headroom is left to its highlights; at 203 (BT.2408's broadcast
// reference) the whole picture is twice as bright as SDR (Mario Kart World and Ocarina of Time, by eye). iOS takes the same
// value: the same Core Animation
static float vi_paper_white_v;
static void vi_paper_white_read(void) { const char *e = getenv("DM_HDR_PAPER_WHITE"); double v = e && *e ? strtod(e, NULL) : 100.0; vi_paper_white_v = (v >= 1.0 && v <= 65535.0) ? (float)v : 100.0f; }
float dm_hdr_paper_white(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, vi_paper_white_read); return vi_paper_white_v; }   // pthread_once: every guest thread sees the value written before
// The peak the status query answers, in nits. DM_HDR_PEAK=<n> is that number (16-bit), read at every query. DM_HDR_PEAK=auto (the HDR peak row's Automatic, the default on the Mac) is
// what the window's screen can show right now: 100 nits times its current EDR headroom (front_hdr_headroom). macOS shows a PQ layer relative to the screen's SDR white, 100 nits
// at EDR 1.0 at every brightness (WWDC21 10161, and measured with an extended-range screen capture), and the headroom is how far above it the
// screen goes, so 100 x headroom is the highest PQ level the screen shows unchanged. A game told that tone-maps into it: Donkey Kong Bananza declares the peak (400, 1000, up to its
// own 1400) and re-declares at its next query when it changes; Ocarina of Time declares min(peak, 3.5 x paper white). The answer moves in steps of about 6 % (40 to a decade) and
// only once a new step has held for a second, so auto-brightness drift and a drag of the slider do not re-tone-map the game at every frame; at least twice the paper white (the
// headroom starts near 1 when the layer appears and ramps up) and at most 10,000 (the top of PQ). With no headroom known (no front end): 1000.
// Unset: automatic (the row's default, dm/games/settings.tsv)
// vi_screen_peak is that held value, the screen's peak as the host uses it: the automatic answer, and the roll-off's target for a fixed peak (dm_hdr_rolloff)
static pthread_mutex_t vi_peak_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vi_screen_peak(void) {
    float h = front_hdr_headroom();
    if (!(isfinite(h) && h > 0.0f)) return 1000;
    double pw = dm_hdr_paper_white(), v = 100.0 * h;
    v = pow(10.0, round(log10(fmax(v, 1.0)) * 40.0) / 40.0);   // the step
    v = fmin(fmax(v, 2.0 * pw), 10000.0);
    uint32_t cand = (uint32_t)(round(v / 10.0) * 10.0);
    if (cand < 10) cand = 10;   // a paper white under 5 nits: never a peak of 0
    static uint32_t answered, pending; static int64_t pending_since;
    pthread_mutex_lock(&vi_peak_mu);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;   // under the lock: the times stay in order
    if (!answered) answered = cand;   // the first answer is the first reading
    else if (cand == answered) pending = 0;
    else if (cand != pending) { pending = cand; pending_since = now; }
    else if (now - pending_since >= vi_env_u32("DM_HDR_PEAK_HOLD", 1000)) { answered = cand; pending = 0; }   // the hold in ms (DM_HDR_PEAK_HOLD; 0 in t_present_hdr)
    uint32_t r = answered; pthread_mutex_unlock(&vi_peak_mu);
    return r;
}
// DM_HDR_PEAK that is neither a number of nits (1 to 65535; larger is 65535) nor auto counts as unset, with one red line: "Auto" or "abc" told the game a peak of 0
static uint32_t vi_hdr_peak(int *automatic) {
    const char *e = getenv("DM_HDR_PEAK"); char *end = NULL; unsigned long v = 0;
    int set = e && *e, aut = set && !strcmp(e, "auto"), num = set && !aut && e[0] >= '0' && e[0] <= '9' && (v = strtoul(e, &end, 10)) > 0 && !*end;
    if (set && !aut && !num) { static atomic_int said; if (!atomic_exchange(&said, 1)) LOG("[RED] DM_HDR_PEAK=%s is neither a peak in nits (1 to 65535) nor auto: taken as unset", e); }
    *automatic = aut || !num;
    return *automatic ? vi_screen_peak() : v > 65535 ? 65535 : (uint32_t)v;
}
// Whether the HDR layer carries HDR10 metadata of the game's declared range (CAEDRMetadata; front_mac.m, front_ios.m). Without it (the default) the system shows the PQ values
// as they are (EDR = nits / 100) and clips above the headroom: the automatic peak keeps the game within the screen, and the present rolls off a game that declares more than the
// screen shows (dm_hdr_rolloff). With it (DM_HDR_META=1) Core Animation maps the range: it passes PQ through while the range fits in 100 x headroom and compresses the whole
// picture, mid-tones too, when it does not (measured, not documented). Off by default because in fullscreen on the Mac the layer with metadata, whatever its range, showed
// rectangles of older frames under GPU load (docs/reference/rendering.md), and the metadata's mapping made the game's white change with the screen's brightness
int dm_hdr_meta(void) { const char *e = getenv("DM_HDR_META"); return e && *e && strcmp(e, "0") != 0; }   // read at each use (the front ends read it when the layer is made)
// Whether the HDR layer has the metadata, as the front end made it (dm_hdr_layer_meta_set, when it makes the layer): what the roll-off and the present's order follow, so a
// DM_HDR_META changed while the game runs (DM_CTL env) cannot leave them out of step with the layer. Before a front end says (the tests): dm_hdr_meta
static _Atomic int g_vi_layer_meta = -1;
void dm_hdr_layer_meta_set(int on) { atomic_store(&g_vi_layer_meta, on < 0 ? -1 : on != 0); }   // -1: no front end's record (t_present_hdr)
int dm_hdr_layer_meta(void) { int m = atomic_load(&g_vi_layer_meta); return m >= 0 ? m : dm_hdr_meta(); }
static _Atomic uint32_t g_vi_declared_hi;   // the extended range's top the game last declared, in nits (0 before it says)
static _Atomic uint32_t g_vi_told_peak;     // the peak the status query last answered with HDR on, in nits
static _Atomic int g_vi_told_auto;          // and whether it was the automatic peak (DM_HDR_PEAK=auto)
// The present's roll-off (present.metal mode 4), without metadata only: when the game may draw more than the screen shows, its pixels above *knee are compressed into *target,
// the brightest channel smoothly towards the target, the hue kept; everything below the knee is copied as it is. The target is the screen's peak as vi_screen_peak holds it (100 x
// headroom in steps, held for a second, at least twice the paper white), not the headroom of the moment: that ramps up for about 2 s after an EDR layer appears (1.2 to 16), and
// following it compressed the game's white towards 120 nits at the start of Ocarina of Time and lifted it again. The knee is three quarters of the target, so at least 1.5 times
// the paper white: the game's white is never touched. Returns 0 when nothing is to be rolled off. Never with the automatic peak: the game was told that same peak and keeps its
// pixels within it
int dm_hdr_rolloff(float *knee, float *target) {
    if (dm_hdr_layer_meta() || atomic_load(&g_vi_told_auto)) return 0;
    if (!front_hdr_screen()) return 0;   // a screen without EDR (headroom 1): nothing to roll into, and its headroom must not move the held peak
    // What the game may draw: the smaller of the range it declares and the peak it was told. Mario Kart World keeps its pixels within the peak (its brightest about 280 nits
    // told 400, 357 told 1000 or 4000, at its title) but declares about 1.6 to 1.8 times it (628, 1801, 7206), so the declared range alone would compress pixels it never draws
    // A game that has declared nothing yet may draw up to the peak it was told
    float h = front_hdr_headroom(); uint32_t d = atomic_load(&g_vi_declared_hi), told = atomic_load(&g_vi_told_peak);
    if (told && (!d || told < d)) d = told;
    if (!(isfinite(h) && h > 0.0f) || !d) return 0;
    float t = (float)vi_screen_peak();
    if ((float)d <= 1.05f * t) return 0;   // a range within 5% of the screen's peak is clipped instead: the peak told is rounded to 10 nits
    *knee = 0.75f * t; *target = t; return 1;
}
// Set once the game's status query has answered HDR on: its RGB10A2 window then holds PQ code values (present_mode, nvn_mtl.m); an RGB10A2 window of a game that never asked for HDR is not decoded.
// It stays set when the window later moves to a screen without EDR: the game keeps its HDR window until it asks again, and the present still decodes it. It also stays set when
// the iOS in-game menu turns HDR off, so the game's last HDR frames are still converted for the SDR layer; a game whose SDR window were RGB10A2 would then be decoded as PQ.
// None of the games here is: their SDR windows are RGBA8 (Donkey Kong Bananza, Mario Kart World, Ocarina of Time)
static _Atomic int g_vi_hdr_answered;
int vi_hdr_answered(void) { return atomic_load(&g_vi_hdr_answered); }
// The white an SDR picture (the game's SDR window, or a float one) is encoded at in the PQ layer: the paper white while the status query answers HDR, so the game's SDR window
// before it switches matches its HDR white, and else 100 nits, the screen's SDR white (EDR 1.0): the paper white is an HDR calibration, and on a screen without EDR, or with HDR
// output off while the HDR layer stays, a paper white of 203 made the SDR picture twice as bright and clipped it
static _Atomic int g_vi_hdr_now;   // the status query's last answer was HDR on
float dm_hdr_sdr_white(void) { return atomic_load(&g_vi_hdr_now) ? dm_hdr_paper_white() : 100.0f; }
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR status: SDR output, or HDR on with DM_HDR=1 on a screen that can show EDR", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp;
    int mode = dm_sdk_ounce() ? dm_hdr_mode() : 0;
    if (!mode && !atomic_load(&g_vi_hdr_live)) { if (on) *on = 0; if (fmt) *fmt = 0; if (peak) *peak = 0; return RES_OK; }   // off from the start: SDR, not logged
    // On a screen without EDR the game draws its own SDR picture, as a Switch 2 does on a TV without HDR10, rather than Core Animation mapping an HDR picture down
    int hdr = mode && front_hdr_screen() != 0;
    int automatic = 0; uint32_t p = hdr ? vi_hdr_peak(&automatic) : 0;   // only with HDR on: an SDR screen's headroom (1) must not move the held automatic peak
    // Logged when the answer changes, and at the 1st, 2nd, 10th, 100th and 1000th query: whether the game asks again (and so can follow the window to another screen)
    static atomic_int queries, last = -1;
    int n = atomic_fetch_add(&queries, 1) + 1, was = atomic_exchange(&last, hdr);
    if (was != hdr || n == 1 || n == 2 || n == 10 || n == 100 || n == 1000)
        LOG("[nvn2-hdr] vi::QueryDisplayHdrStatus (query %d): %s", n, hdr ? "HDR on, format 1"
            : mode ? "SDR (DM_HDR is on, but the window's screen cannot show EDR)" : "SDR (HDR output turned off while the game runs)");
    if (hdr) atomic_store(&g_vi_hdr_answered, 1);
    // The automatic peak (DM_HDR_PEAK=auto) is logged when the answer differs from the last one logged and a second has passed since that line: the settled value of a brightness
    // ramp is logged within a second of the ramp's end (the game asks every frame), and the answers between are counted rather than written
    static _Atomic uint32_t peak_last, peak_logged; static _Atomic int64_t peak_logged_at; static atomic_int peak_changes;
    if (hdr && automatic) {
        if (atomic_exchange(&peak_last, p) != p) atomic_fetch_add(&peak_changes, 1);
        if (p != atomic_load(&peak_logged)) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, at = atomic_load(&peak_logged_at);
            if ((at == 0 || now - at >= 1000) && atomic_compare_exchange_strong(&peak_logged_at, &at, now)) {   // one thread logs when two ask at once
                uint32_t was_p = atomic_exchange(&peak_logged, p); int c = atomic_exchange(&peak_changes, 0);
                if (!was_p) LOG("[nvn2-hdr] vi::QueryDisplayHdrStatus: peak %u nits (automatic: 100 × the screen's headroom %.2f)", p, front_hdr_headroom());
                else LOG("[nvn2-hdr] vi::QueryDisplayHdrStatus: peak now %u nits (automatic: the screen's headroom %.2f; was %u, %d answer%s since that line)",
                         p, front_hdr_headroom(), was_p, c, c == 1 ? "" : "s");
            }
        }
    }
    if (hdr) { atomic_store(&g_vi_told_peak, p); atomic_store(&g_vi_told_auto, automatic); }
    atomic_store(&g_vi_hdr_now, hdr);
    if (on) *on = (uint8_t)hdr; if (fmt) *fmt = (uint32_t)hdr; if (peak) *peak = hdr ? (uint16_t)p : 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display HDR paper white: 100 nits (the screen's white) unless DM_HDR_PAPER_WHITE", Result, h_vi_PaperWhite, uint16_t *nits, const void *disp) {
    (void)disp; uint16_t v = vi_hdr_on() ? (uint16_t)lroundf(dm_hdr_paper_white()) : 203;   // dm_hdr_paper_white is 1 to 65535, so the cast is in range; 203, BT.2408's reference, while HDR is off: the games read it only with HDR on
    static atomic_int n; if (vi_hdr_on() && atomic_fetch_add(&n, 1) < 3) LOG("[nvn2-hdr] vi::QueryDisplayHdrPaperWhite: %u nits", v);
    if (nits) *nits = v; return RES_OK; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "display VRR: not supported (fixed refresh rate)", Result, h_vi_VrrSupported, uint8_t *on, const void *disp) {
    (void)disp; if (on) *on = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "display VRR capability: the host has no VRR", Result, h_vi_SetVrr, void *disp, unsigned on) {
    (void)disp; (void)on; return RES_OK; }
// The largest display buffer (vi2 through the SDK, sdk+0x5e10c0, which the host does not serve: Result 0x272 and the output left as the game set it). BufferMaxResolution is
// 0 1280x720, 1 1920x1080, 2 2560x1440, 3 3840x2160 (Fire Emblem Fortune's Weave's own table, main+0x1abc2c0 / +0x1abc050). Fire Emblem Fortune's Weave sets 0 first, so the failed call
// told it 720p: it chose its 1280x720 DLSS network and rendered at 640x360, and the host scaled that to the window. Answered as a Switch 2 would: in handheld mode its own 1920x1080
// screen; docked, the largest of the four the display it is shown on holds (front_display_pixels), 1920x1080 when that is not known
__attribute__((weak)) int front_display_pixels(int *w, int *h) { (void)w; (void)h; return 0; }   // front_mac.m, front_ios.m; graphics unit tests have none
HLE_FORCE("_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", "largest display buffer: the Switch 2's screen handheld, the host display's docked",
          Result, h_vi_BufMaxRes, uint32_t *out, const void *disp) {
    (void)disp; static const int k[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    uint32_t r = 1; int w = 0, h = 0;
    if (dm_opmode() == 1 && front_display_pixels(&w, &h)) {
        if (w < h) { int t = w; w = h; h = t; }
        r = 0; for (uint32_t i = 0; i < 4; i++) if (w >= k[i][0] && h >= k[i][1]) r = i;
    }
    static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::QueryDisplayBufferMaxResolution: %dx%d (%s, display %dx%d)", k[r][0], k[r][1], dm_opmode() == 1 ? "docked" : "handheld", w, h);
    if (out) *out = r; return RES_OK;
}
// The extended swap interval asks for a presentation rate between whole vsync intervals (VRR); without VRR the swap interval NVN sets stays in effect
HLE_FORCE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", "layer extended swap interval: needs VRR, which the host has not; NVN's swap interval stays", Result, h_vi_ExtSwap,
          void *layer, int a, int b) {
    (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerExtendedSwapInterval %d %d: no VRR on the host, the NVN swap interval stays", a, b); return RES_OK; }
HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "layer extended swap interval cleared", Result, h_vi_ExtSwapClear, void *layer) { (void)layer; return RES_OK; }
// The SDK's HDR metadata calls for the layer (the gamut the game declares, its tone mapping to SDR, its luminance ranges): the host sends none of them to the screen but the extended range, which becomes the layer's HDR10 metadata.
// The first three of each are logged: the game's own statement of the gamut and luminance it targets, as it passed them (the luminance ranges are in nits: 0..203 and 0..1000 on Donkey Kong Bananza and Mario Kart World)
#define VI_META_LOG(...) do { static atomic_int n_; if (vi_hdr_on() && atomic_fetch_add(&n_, 1) < 3) LOG(__VA_ARGS__); } while (0)   // logged only while HDR is on
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "layer HDR metadata: dropped (the host sends none); the gamut is logged", Result, h_vi_HdrGamut, void *layer, int g) {
    (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrContentGamutMetadata: gamut %d (dropped)", g); return RES_OK; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "layer HDR metadata: dropped (the host sends none); the tone mapping is logged", Result, h_vi_HdrToneMap, void *layer, int m) {
    (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrToSdrToneMappingMetadata: tone mapping %d (dropped)", m); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: the extended luminance range bounds the roll-off and is the HDR layer's HDR10 metadata with DM_HDR_META; logged", Result, h_vi_HdrExtLum,
          void *layer, uint32_t lo, uint32_t hi) {   // passed on to the HDR layer's metadata (front_hdr_content_range)
    (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata: %u..%u", lo & 0xffff, hi & 0xffff);
    // Each change after that is logged too, the first 20 (Mario Kart World declares 0..1000 nits while it makes its HDR window, then 0..1801 at its title)
    static _Atomic uint32_t last; static atomic_int changes; uint32_t r = (lo & 0xffff) | (hi & 0xffff) << 16, was = atomic_exchange(&last, r);
    if (vi_hdr_on() && was && was != r && atomic_fetch_add(&changes, 1) < 20) LOG("[nvn2-hdr] vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata: now %u..%u (was %u..%u)", r & 0xffff, r >> 16, was & 0xffff, was >> 16);
    if (vi_hdr_on()) { atomic_store(&g_vi_declared_hi, hi & 0xffff); front_hdr_content_range(lo & 0xffff, hi & 0xffff); } return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: dropped (the host sends none); the luminance range is logged", Result, h_vi_HdrStdLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrStandardContentLuminanceRangeMetadata: %u..%u (dropped)", lo & 0xffff, hi & 0xffff); return RES_OK; }
// The rest of SDK 22's layer HDR metadata calls (the Bezier tone curve and the clears): the SDK forwards them through a table its graphics system fills, which is null on the host, so
// unanswered they call through null (Mario Kart World 1.8.0 sets the curve every frame, by pull request #1116)
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrBezier,
          void *layer, float a, float b, float c) { (void)layer; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrBezierClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrGamutClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrToneMapClear, void *layer) { (void)layer; return RES_OK; }
