// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
#include "nvn/nvn_mtl.h"
#include <stdarg.h>
#include <stdatomic.h>

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
// A frame is shown once the present queue has handed it on (nvn_mtl.m g_pq_done); its present time is the drawable's presented time when Metal reports one, else (headless, before
// the handler runs, or a drawable the Mac never put on screen) the time it was first seen handed on. A Switch's compositor shows every frame it is handed, in order, so a frame the
// Mac dropped (presentedTime 0) still counts as shown: listed as queued, the newest ones stayed so for ever when the game presented nothing more until their flip came (Orbitals,
// main+0x1540460: right after both players ready up the load stopped presenting, and its FFrameEndSync waited for the flip of a frame Metal had dropped, until Unreal aborted: "GameThread timed out waiting for …")
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
        double pt = e->ticket <= d ? mtl_presented_time(e->ticket) : 0;
        if (pt > 0) { uint64_t ago = hnow > (uint64_t)(pt * 1e9) ? hnow - (uint64_t)(pt * 1e9) : 0; shown = gnow > ago ? gnow - ago : 1; }
        else if (e->ticket <= d) { if (!e->seen_ns) e->seen_ns = gnow; shown = e->seen_ns; }
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
// Switch 2 BufferMaxResolution tiers. Handheld and an unknown docked display report 1080p; a known docked display gets the largest tier it holds.
__attribute__((weak)) int front_display_pixels(int *w, int *h) { (void)w; (void)h; return 0; }   // front_mac.m, front_ios.m; graphics unit tests have none
static const int k_buf_max_res[4][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static uint32_t buf_max_res(int *w, int *h) {
    *w = *h = 0;
    if (dm_opmode() != 1 || !front_display_pixels(w, h)) return 1;
    if (*w < *h) { int t = *w; *w = *h; *h = t; }
    uint32_t r = 0;
    for (uint32_t i = 1; i < 4; i++) if (*w >= k_buf_max_res[i][0] && *h >= k_buf_max_res[i][1]) r = i;
    return r;
}
// The Great Ace Attorney Chronicles (SDK 10) lists up to 10 displays before opening the default one (main+0x808f08) and does not read the list. DisplayInfo is 0x60 bytes:
//   name[0x40], bool has a layer limit at +0x40, then u64 layer count limit, largest layer width and height. One display, "Default", as Horizon lists for an application.
// Star Fox sizes its docked window from the largest layer and clamps it to QueryDisplayBufferMaxResolution. Both replies use the same tier for Switch 2.
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list the Default display and its largest layer", int, h_vi_ListDisp, void *out, int n) {
    if (n <= 0 || !out) return 0;
    int w, h;
    uint32_t r = dm_sdk_ounce() ? buf_max_res(&w, &h) : 1;
    uint8_t *p = out; memset(p, 0, 0x60); strcpy((char *)p, "Default");
    p[0x40] = 1; uint64_t lim[3] = {1, (uint64_t)k_buf_max_res[r][0], (uint64_t)k_buf_max_res[r][1]}; memcpy(p + 0x48, lim, sizeof lim);
    static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::ListDisplays: \"Default\", largest layer %dx%d", k_buf_max_res[r][0], k_buf_max_res[r][1]);
    return 1;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}

// ---------- Switch 2 SDKs: HDR, VRR and extended swap intervals ----------
// The SDK's versions check the display against its own default display object and forward the layer to vi2, whose layer object the host never made (Donkey Kong Bananza: a call
// through null from SetLayerNextHdrContentGamutMetadata, main+0x2036f4). The host has no VRR, and the layer metadata has nothing to apply to: the present's HDR layer takes no
// metadata (nvn_mtl.m, HDR output). HdrOutputFormat 0 is SDR; paper white is the BT.2408 reference, MTL_HDR_PAPER_WHITE nits, read only when HDR is on.
// The SDK's DefaultDisplay::QueryHdrStatus (SDK 22.2.8, sdk+0x5de630) writes the format as a u32, the flag as format == 1 (a format above 1 fails with 0x272) and the peak as a u16 in nits.
// The answer is the present's (mtl_hdr_vi): HDR output (format 1) with DM_HDR=2 (Game's HDR) on a screen with EDR headroom, the peak being the screen's live headroom x the paper
// white, so a game that renders HDR when the display has it takes that path and tone maps to the range the screen has; SDR otherwise, and with DM_HDR=1 (BT.2446, the inverse tone
// map for every game).
// Ocarina of Time queries it every frame and follows a new peak on the same frame. Read at every call, so DM_CTL's env changes DM_HDR while the game runs. With DM_HDR set, the
// answer is logged when it changes, at most once a second (vi_hdr_log, below); with DM_HDR off nothing is logged
typedef struct { pthread_mutex_t mu; uint32_t v[3]; int seen; uint64_t at_ns; unsigned skipped; } vi_hdr_track;
static void vi_hdr_log(vi_hdr_track *t, const uint32_t v[3], const char *what, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#define VI_HDR_TRACK {PTHREAD_MUTEX_INITIALIZER, {0}, 0, 0, 0}
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR status: HDR with DM_HDR=2 on a screen with EDR headroom, else SDR", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp; static _Atomic uint64_t calls; static vi_hdr_track t = VI_HDR_TRACK;
    uint16_t p; int hdr = mtl_hdr_vi(&p); uint64_t n = ++calls; uint32_t v[3] = {(uint32_t)hdr, p};
    vi_hdr_log(&t, v, "QueryDisplayHdrStatus", "%s, format %d, peak %u nits, call %llu", hdr ? "HDR" : "SDR", hdr, p, (unsigned long long)n);
    if (on) *on = (uint8_t)hdr; if (fmt) *fmt = (uint32_t)hdr; if (peak) *peak = p; return RES_OK; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display HDR paper white: 203 nits (BT.2408), read when HDR is reported on", Result, h_vi_PaperWhite, uint16_t *nits, const void *disp) {
    (void)disp; if (nits) *nits = MTL_HDR_PAPER_WHITE; return RES_OK; }
HLE_FORCE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", "display VRR: not supported (fixed refresh rate)", Result, h_vi_VrrSupported, uint8_t *on, const void *disp) {
    (void)disp; if (on) *on = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", "display VRR capability: the host has no VRR", Result, h_vi_SetVrr, void *disp, unsigned on) {
    (void)disp; (void)on; return RES_OK; }
// The largest display buffer (vi2 through the SDK, sdk+0x5e10c0, which the host does not serve: Result 0x272 and the output left as the game set it). BufferMaxResolution is
// 0 1280x720, 1 1920x1080, 2 2560x1440, 3 3840x2160 (Fire Emblem Fortune's Weave's own table, main+0x1abc2c0 / +0x1abc050). Fire Emblem Fortune's Weave sets 0 first, so the failed call
// told it 720p: it chose its 1280x720 DLSS network and rendered at 640x360, and the host scaled that to the window.
HLE_FORCE("_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", "largest display buffer: the Switch 2's screen handheld, the host display's docked",
          Result, h_vi_BufMaxRes, uint32_t *out, const void *disp) {
    (void)disp; int w, h; uint32_t r = buf_max_res(&w, &h);
    static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::QueryDisplayBufferMaxResolution: %dx%d (%s, display %dx%d)", k_buf_max_res[r][0], k_buf_max_res[r][1], dm_opmode() == 1 ? "docked" : "handheld", w, h);
    if (out) *out = r; return RES_OK;
}
// The extended swap interval asks for a presentation rate between whole vsync intervals (VRR); without VRR the swap interval NVN sets stays in effect
HLE_FORCE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", "layer extended swap interval: needs VRR, which the host has not; NVN's swap interval stays", Result, h_vi_ExtSwap,
          void *layer, int a, int b) {
    (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerExtendedSwapInterval %d %d: no VRR on the host, the NVN swap interval stays", a, b); return RES_OK; }
HLE_FORCE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", "layer extended swap interval cleared", Result, h_vi_ExtSwapClear, void *layer) { (void)layer; return RES_OK; }
// With DM_HDR set the HDR status answer and the layer HDR metadata setters are logged when their values change (games call them every frame). Each logs at most one line a second, with
// the number of changes left out since its last line; a value that changed while it could not log is logged at the next call after that second. The luminance ranges are (u16 min, u16 max) in nits:
// Ocarina of Time (v131072, main+0x570410) passes min 0 and its standard and extended maximums. The gamut and the tone mapping are enums (u32); the Bezier curve's floats are
// compared by their bits, so a NaN the game passes again is no change
static void vi_hdr_log(vi_hdr_track *t, const uint32_t v[3], const char *what, const char *fmt, ...) {
    if (!mtl_hdr_requested()) return;
    unsigned skipped;
    pthread_mutex_lock(&t->mu);
    uint64_t now = now_ns();   // inside the lock: a time read before it could be older than at_ns, and the difference would wrap
    int changed = !t->seen || memcmp(t->v, v, sizeof t->v), due = !t->at_ns || now - t->at_ns >= 1000000000ull;
    if (changed) { t->seen = 1; memcpy(t->v, v, sizeof t->v); }
    if (!due || (!changed && !t->skipped)) { t->skipped += (unsigned)changed; pthread_mutex_unlock(&t->mu); return; }
    skipped = t->skipped - (changed ? 0 : 1); t->skipped = 0; t->at_ns = now;   // an unchanged call that logs reports the last skipped change itself
    pthread_mutex_unlock(&t->mu);
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (skipped) LOG("vi::%s: %s (frame %llu; %u changes not logged since the last line)", what, b, (unsigned long long)dm_nvn_frame_count(), skipped);
    else LOG("vi::%s: %s (frame %llu)", what, b, (unsigned long long)dm_nvn_frame_count());
}
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "layer HDR metadata: nothing to apply (the HDR layer takes none); logged with DM_HDR set", Result, h_vi_HdrGamut,
          void *layer, uint32_t g) {
    (void)layer; static vi_hdr_track t = VI_HDR_TRACK; uint32_t v[3] = {g}; vi_hdr_log(&t, v, "SetLayerNextHdrContentGamutMetadata", "gamut %u", g); return RES_OK; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "layer HDR metadata: nothing to apply (the HDR layer takes none); logged with DM_HDR set", Result, h_vi_HdrToneMap,
          void *layer, uint32_t m) {
    (void)layer; static vi_hdr_track t = VI_HDR_TRACK; uint32_t v[3] = {m}; vi_hdr_log(&t, v, "SetLayerNextHdrToSdrToneMappingMetadata", "tone mapping %u", m); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: nothing to apply (the HDR layer takes none); logged with DM_HDR set", Result, h_vi_HdrExtLum,
          void *layer, uint32_t lo, uint32_t hi) {
    (void)layer; static vi_hdr_track t = VI_HDR_TRACK; uint32_t v[3] = {lo & 0xffff, hi & 0xffff};
    vi_hdr_log(&t, v, "SetLayerNextHdrExtendedContentLuminanceRangeMetadata", "min %u, max %u nits", v[0], v[1]); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: nothing to apply (the HDR layer takes none); logged with DM_HDR set", Result, h_vi_HdrStdLum,
          void *layer, uint32_t lo, uint32_t hi) {
    (void)layer; static vi_hdr_track t = VI_HDR_TRACK; uint32_t v[3] = {lo & 0xffff, hi & 0xffff};
    vi_hdr_log(&t, v, "SetLayerNextHdrStandardContentLuminanceRangeMetadata", "min %u, max %u nits", v[0], v[1]); return RES_OK; }
// The rest of SDK 22's layer HDR metadata calls (the Bezier tone curve and the clears): the SDK forwards them through a table its graphics system fills, which is null on the host, so
// unanswered they call through null (Mario Kart World 1.8.0 sets the curve every frame, by pull request #1116)
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "layer HDR metadata: nothing to apply (the HDR layer takes none); logged with DM_HDR set", Result, h_vi_HdrBezier,
          void *layer, float a, float b, float c) {
    (void)layer; static vi_hdr_track t = VI_HDR_TRACK; float f[3] = {a, b, c}; uint32_t v[3]; memcpy(v, f, sizeof v);
    vi_hdr_log(&t, v, "SetLayerNextHdrAdjustableBezierMetadata", "%g %g %g", (double)a, (double)b, (double)c); return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", "layer HDR metadata cleared: nothing to apply", Result, h_vi_HdrBezierClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", "layer HDR metadata cleared: nothing to apply", Result, h_vi_HdrGamutClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", "layer HDR metadata cleared: nothing to apply", Result, h_vi_HdrToneMapClear, void *layer) { (void)layer; return RES_OK; }
