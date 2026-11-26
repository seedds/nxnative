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
// A frame is shown once the present queue has handed it on (nvn_mtl.m g_pq_done); its present time is the drawable's presented time when Metal reports one. Else (headless, before
// the handler runs, or a drawable the Mac never put on screen), when the host pacer drives a vsync grid, it is the first tick of that grid at or after both its present and the end
// of its GPU work, latched once reached so a frame never reverts to queued or shifts its flip tick. A game that times its frames by these flips (Star Fox's throttle,
// docs/reference/frame-rate.md) sees a fixed vsync grid, as on the Switch, instead of its own first-read time. Without a pacer grid (DM_DETCLOCK, display-link pacing, or unpaced
// presents), the time it was first seen handed on stays. A Switch's compositor shows every frame it is handed, in order, so a frame the Mac dropped (presentedTime 0) still counts
// as shown: listed as queued, the newest ones stayed so for ever when the game presented nothing more until their flip came (Orbitals, main+0x1540460: right after both players
// ready up the load stopped presenting, and its FFrameEndSync waited for the flip of a frame Metal had dropped, until Unreal aborted: "GameThread timed out waiting for …")
#define VFH 8
typedef struct { uint64_t frame, ticket, queued_ns, seen_ns, at, grid, tick, vsync_ns; uint8_t queued, enqueued; } vfh_ent;   // at, grid, tick, vsync_ns: present, pacer vsync, one vsync, latched flip (now_ns clock, ns); queued: game thread recorded it; enqueued: nq_worker assigned its physical ticket
static vfh_ent g_vfh[VFH]; static pthread_mutex_t g_vfh_mu = PTHREAD_MUTEX_INITIALIZER;
void mtl_present_progress(uint64_t *queued, uint64_t *done);   // nvn_mtl.m
double mtl_presented_time(uint64_t n);                         // nvn_mtl.m: CACurrentMediaTime seconds (the now_ns clock), 0 not yet, -1 dropped by the system
int mtl_gpu_done(uint64_t f, uint64_t *t1);                    // nvn_mtl.m: 1 the frame's GPU work completed (latest end, now_ns clock), 0 running, -1 no record
void dm_vi_frame_queued(uint64_t frame, uint64_t at, uint64_t grid, uint64_t tick) {   // hle_nvn.c, at each present the game queues: frame = GetLatestFrameNumber's count with it
    pthread_mutex_lock(&g_vfh_mu);
    vfh_ent *e = &g_vfh[frame % VFH];
    uint64_t tk = e->frame == frame && e->enqueued ? e->ticket : 0;
    uint8_t enq = e->frame == frame ? e->enqueued : 0;
    *e = (vfh_ent){frame, tk, guest_ns(), 0, at, grid, tick, 0, 1, enq};
    pthread_mutex_unlock(&g_vfh_mu);
}
void dm_vi_frame_ticket(uint64_t frame, uint64_t ticket) {   // nvn_queue.inc (nq_worker), when mtl_present assigns this frame's physical display ticket (0 if discarded/unsubmitted)
    pthread_mutex_lock(&g_vfh_mu);
    vfh_ent *e = &g_vfh[frame % VFH];
    if (e->frame == frame) { e->ticket = ticket; e->enqueued = 1; }
    else if (e->frame < frame) *e = (vfh_ent){frame, ticket, 0, 0, 0, 0, 0, 0, 0, 1};
    pthread_mutex_unlock(&g_vfh_mu);
}
// The vsync a display would show the frame at (now_ns clock): 1 with it, 0 not yet (its GPU work still running, or that vsync still to come), -1 no pacer grid.
// Once reached, e->vsync_ns latches the flip time so later reads stay monotonic. The frame's GPU work is tagged with the frame count before its present
// (nq_job.frame = dm_nvn_frame_count() when queued), one less than its number here.
static int vfh_vsync(vfh_ent *e, uint64_t hnow, uint64_t *at) {
    if (e->vsync_ns) { *at = e->vsync_ns; return 1; }
    if (!e->grid || !e->tick) return -1;
    uint64_t ready = e->at, end = 0; int g = mtl_gpu_done(e->frame - 1, &end);
    if (!g) return 0;
    if (g > 0 && end > ready) ready = end;
    uint64_t t = ready <= e->grid ? e->grid : e->grid + (ready - e->grid + e->tick - 1) / e->tick * e->tick;
    if (t > hnow) return 0;
    e->vsync_ns = t; *at = t; return 1;
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
        vfh_ent *e = &g_vfh[f % VFH]; if (e->frame != f || !e->queued) continue;
        uint64_t *o = (uint64_t *)out + 5 * k++, shown = 0;
        int handed = e->enqueued && e->ticket <= d;
        double pt = handed && e->ticket ? mtl_presented_time(e->ticket) : 0;
        if (pt > 0) { uint64_t ago = hnow > (uint64_t)(pt * 1e9) ? hnow - (uint64_t)(pt * 1e9) : 0; shown = gnow > ago ? gnow - ago : 1; }
        else if (handed) {
            uint64_t vs; int v = vfh_vsync(e, hnow, &vs);
            if (v > 0) { uint64_t ago = hnow > vs ? hnow - vs : 0; shown = gnow > ago ? gnow - ago : 1; }
            else if (v < 0) { if (!e->seen_ns) e->seen_ns = gnow; shown = e->seen_ns; }
        }
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
// through null from SetLayerNextHdrContentGamutMetadata, main+0x2036f4). No VRR on the host, and SDR unless DM_HDR=1. Paper white 100 nits, the screen's own white
// DM_HDR=1 is the HDR mode (Switch 2 games; the HDR setting of the Mac and iOS launchers sets it): while the window's screen can show EDR and the window has the HDR layer
// (dm_hdr_layer_tick below), the display reports HDR on, so the game takes its HDR path (its window becomes HDR10: RGB10A2 with PQ values in BT.2020). Elsewhere it reports SDR,
// and the game keeps its own SDR picture on the SDR layer, as without DM_HDR.
// The HDR layer (RGB10A2 in BT.2100 PQ, front_mac.m and front_ios.m) takes the PQ window as it is, without HDR10 metadata (docs/reference/rendering.md)
// The format is 1 with HDR on and 0 with it off: the SDK's own answer is on = (format == 1), and a format above 1 is an error, 0x272 (Donkey Kong Bananza v458752,
// nn::vi::detail::DefaultDisplay::QueryHdrStatus, sdk+0x55bd60; found in pull request #1249). The peak (DM_HDR_PEAK, automatic by default; vi_hdr_peak below) bounds the extended luminance range the game declares (its metadata call, below)
// Donkey Kong Bananza boots its SDR window (0x25), queries the status, then makes an RGB10A2 window (0x3d) of PQ code values with SDR white at the paper white (202.9 nits told 203).
// Neither it nor Mario Kart World imports the SDK's HDR status event (CreateHdrStatusEvent and the rest), so a game learns of another screen only by asking again: the queries are counted in the log
// A decimal number, clamped to 16 bits (the answers are 16-bit)
static uint32_t vi_env_u32(const char *name, uint32_t def) { const char *e = getenv(name); if (!e || !*e) return def; unsigned long v = strtoul(e, NULL, 10); return v > 65535 ? 65535 : (uint32_t)v; }
// The HDR mode: 0 off, 1 HDR while the window's screen can show EDR. DM_HDR is read the way the front ends read it (atoi: any non-zero number is 1) when first asked; the front end changes it while the game runs (dm_hdr_set_mode: the iOS in-game menu), and the game, which asks for its status every frame,
// switches its window on its next query, as when the window moves to another screen. For Switch 2 games only (dm_sdk_ounce): a Switch 1 game answers SDR
static _Atomic int g_vi_hdr_mode = -1;   // -1 until DM_HDR is read
static _Atomic int g_vi_hdr_live;        // the mode was changed while the game runs: its SDR answers are logged too
static _Atomic int g_vi_can_fresh;       // the mode was turned on: the layer tick takes its next reading of the screen as a first one (dm_hdr_layer_tick)
int dm_hdr_mode(void) {
    int m = atomic_load(&g_vi_hdr_mode);
    if (m >= 0) return m;
    const char *e = getenv("DM_HDR"); int v = e && *e ? atoi(e) : 0, unread = -1;
    atomic_compare_exchange_strong(&g_vi_hdr_mode, &unread, v != 0);   // a mode set meanwhile by the front end wins
    return atomic_load(&g_vi_hdr_mode);
}
void dm_hdr_set_mode(int m) {
    m = m != 0;
    if (atomic_exchange(&g_vi_hdr_mode, m) != m) { atomic_store(&g_vi_hdr_live, 1); if (m) atomic_store(&g_vi_can_fresh, 1); LOG("[nvn2-hdr] HDR output %s while the game runs", m ? "on" : "off"); }
}
static int vi_hdr_on(void) { return dm_hdr_mode() && dm_sdk_ounce(); }
// The window's screen's current EDR headroom (front_mac.m: NSScreen.maximumExtendedDynamicRangeColorComponentValue; front_ios.m: UIScreen.currentEDRHeadroom; read with each
// layer tick): how many times the SDR white the screen can show right now, which falls as the screen gets brighter and depends on its mode. It is 1.0 unless a layer on the screen
// asks for EDR (NSScreen.h), so it means something only while the window has the HDR layer. Unknown (0) without a front end (a windowless run): the automatic peak is then 1000,
// and nothing is rolled off
__attribute__((weak)) float front_hdr_headroom(void) { return 0.0f; }   // 0: unknown (no front end that reads it)
// The screen's potential EDR headroom (front_mac.m: maximumPotentialExtendedDynamicRangeColorComponentValue, capped by the reference headroom in a reference mode; front_ios.m:
// potentialEDRHeadroom), what the current headroom rises to once the HDR layer is shown: the automatic peak's first answer (vi_screen_peak). 0 without a front end
__attribute__((weak)) float front_hdr_potential(void) { return 0.0f; }
// The paper white told to the game: 100 nits. The game draws its normal white at it, and Core Animation shows a PQ layer relative to the screen's SDR white, 100 nits at EDR 1.0, so
// the game's white is the screen's SDR white: as bright as with HDR off, following the brightness slider, the headroom left to its highlights. That mapping is documented for a
// layer with HDR10 metadata (CAEDRMetadata.h: "1.0 mapping to diffuse white of 100 nits") and for an app that converts PQ to EDR itself (WWDC21 10161); for the PQ layer without
// metadata, as the host makes it, it was measured (an extended-range screen capture). BT.2408's reference white, 203, is what a TV's standard would have the game told: in a prototype that
// told Donkey Kong Bananza 203 and scaled its window so its white still landed at the screen's SDR white, its mining tunnel came out the same (median and mean light equal,
// highlights to 1.61 and 1.67 times its white, below its 1400-nit cap either way), so 100 stays, with the window going to the layer as it is. Told 203 without the scaling, the
// whole picture was twice as bright as SDR (Mario Kart World and Ocarina of Time, by eye). Not a setting: nothing a player would set changes the picture for the better
float dm_hdr_paper_white(void) { return 100.0f; }

// The window's layer and when HDR is answered. The front end owns the layer and calls dm_hdr_layer_tick on its main thread with each reading of the screen (four times a second,
// and on screen changes), with whether the screen can show EDR at all (its potential headroom is above 1), its current headroom and whether the layer is the HDR one; the tick
// returns the layer to have:
// - The HDR layer once the mode is on, a Switch 2 game that can ask for HDR is running and the screen can show EDR. The game is known from the loader (dm_hdr_boot: a Switch 2
//   sdk, and one of the game's own modules importing QueryDisplayHdrStatus), before its first status query, so the layer is there for that query, or from its own query (only Switch 2 games
//   ask; the iOS in-game menu turning HDR on). On iOS the layer is made before the loader knows the game, so the front end does not decide it. Until then the layer is the SDR
//   one the host always had, so an SDR screen, a Switch 1 game, a Switch 2 game that never asks, or the mode off show exactly what they showed before HDR output existed, and pay no
//   conversion pass and no EDR power (CALayer.h: wantsExtendedDynamicRangeContent "should only be set when displaying EDR contents").
// - HDR is answered from the tick that makes the HDR layer, not before: a game makes its HDR window only on a layer that shows it. It is not held back for the headroom, which ramps from
//   about 1.2 to its value within 2 s after an EDR layer appears: Donkey Kong Bananza decides its window at its first query while it boots and, with a cold pipeline archive, did
//   not ask again for over two minutes, so an answer held for the ramp kept it SDR. The automatic peak follows the ramp through its hold (vi_screen_peak), and the present rolls
//   off what the peak told runs ahead of the screen meanwhile (dm_hdr_rolloff). Until HDR is answered the game's SDR window is drawn into the HDR layer at 100 nits with the
//   SDR layer's curve, sRGB (present_mode 2), as it looks on the SDR layer.
// - Back to the SDR layer once HDR is no longer wanted (the mode turned off, the window moved to a screen without EDR) and the present has drawn two windows in a row that are not
//   PQ (dm_present_sdr_run): the game has seen the SDR answer and made its SDR window, so none of its HDR frames reaches the SDR layer.
// - A change of the screen's capability counts after it has held for half a second, so a window dragged along the edge between two screens does not switch the layer back and forth.
// Without a front end (windowless runs) HDR is answered by the mode alone; with one, only once its first tick has read the screen (dm_hdr_front)
static _Atomic int g_vi_asked;      // a Switch 2 game asked for its status with the mode on
static _Atomic int g_vi_switch2;    // the loaded game is a Switch 2 game that can ask for its HDR status (dm_hdr_boot)
static _Atomic int g_vi_front;      // a front end ticks, or has said it will (dm_hdr_front)
static _Atomic int g_vi_ready;      // HDR may be answered: the layer is the HDR one from the last tick on, and the screen can show EDR
__attribute__((weak)) int dm_present_sdr_run(void) { return 2; }   // nvn_mtl.m: how many presents in a row drew a window that is not PQ
static int vi_hdr_ready(void) { return !atomic_load(&g_vi_front) || atomic_load(&g_vi_ready); }
// A front end that will tick says so when it makes the game's layer, before the guest starts: until its first tick HDR is not answered (vi_hdr_ready), so a status query
// that comes before the front end has read the screen (iOS ticks first after 0.25 s, and not while it has no screen) does not tell the game HDR on for an SDR screen
void dm_hdr_front(void) { atomic_store(&g_vi_front, 1); }
// main.c, once the modules are loaded: whether a Switch 2 game that can ask for HDR runs. A Switch 2 game that does not import the status query never draws an HDR window, so it
// keeps the SDR layer, its MetalFX and its power, as before HDR output existed. It does not read DM_HDR: on iOS the modules are loaded in the background from the app's main() (dm_host_preload_start),
// before the launcher puts its HDR setting into the environment, and dm_hdr_mode keeps its first reading, so reading it here would keep HDR off for the run
void dm_hdr_boot(void) { atomic_store(&g_vi_switch2, dm_sdk_ounce() && dm_game_any_imports("_ZN2nn2vi21QueryDisplayHdrStatus")); }
int dm_hdr_layer_tick(int can, float headroom, int layer_pq, int64_t now_ms) {
    static int can_held = -1, can_pending = -1, was_want = -1; static int64_t can_since;
    (void)headroom;   // the automatic peak and the roll-off read it (front_hdr_headroom); the layer does not wait for it
    atomic_store(&g_vi_front, 1);
    can = can != 0;
    // HDR output turned on again: the front end (iOS) does not tick while the mode is off and the layer the SDR one, so the screen held may be another one's (a TV attached
    // meanwhile): the reading now is taken as a first one, not held half a second against the stale one while the layer is made on the wrong answer
    if (atomic_exchange(&g_vi_can_fresh, 0)) { can_held = -1; can_pending = -1; }
    if (can_held < 0) can_held = can;   // the first reading is taken as it is
    else if (can == can_held) can_pending = -1;
    else if (can != can_pending) { can_pending = can; can_since = now_ms; }
    else if (now_ms - can_since >= 500) { can_held = can; can_pending = -1; LOG("[nvn2-hdr] the window's screen %s EDR", can ? "can show" : "cannot show"); }
    int want = (atomic_load(&g_vi_switch2) || atomic_load(&g_vi_asked)) && can_held && dm_hdr_mode();   // the mode read last: the first reading is kept (dm_hdr_mode)
    int r = want ? 1 : layer_pq && dm_present_sdr_run() < 2;   // not wanted: the HDR layer stays until the game's SDR window is presented
    // Ready from the tick that makes the HDR layer: the front end sets the layer this tick returns on the same thread right after it (front_hdr_layer_set), and the game starts right
    // after the Mac's first tick, so waiting for the next tick told a query in that quarter second SDR. A query that lands between this store and the layer's change gets HDR for a
    // frame whose drawable is still the SDR one: the present converts the HDR10 window for it (present_mode 1), so it shows correctly
    atomic_store(&g_vi_ready, want);   // wanted: this tick returns the HDR layer
    if (was_want != want) { was_want = want; if (want || layer_pq) LOG("[nvn2-hdr] HDR layer %s", want ? "wanted (a Switch 2 game that asks for HDR runs with HDR output on, the screen can show EDR)" : "no longer wanted: back to the SDR layer once the game presents its SDR window"); }
    return r;
}

// The peak the status query answers, in nits. DM_HDR_PEAK=<n> is that number (16-bit), read at every query: a diagnostic switch, for a screen that reports its headroom wrongly.
// DM_HDR_PEAK=auto, the default, is what the window's screen can show right now in the game's own nits: the paper white times its current EDR headroom (front_hdr_headroom). The
// present shows the paper white at the screen's SDR white (EDR 1.0, at every brightness), and the headroom is how far above it the screen goes, so that is the highest level the
// screen shows unchanged. A game told that tone-maps into it: Donkey Kong Bananza declares the peak (400, 1000, up to its own 1400) and re-declares at its next query when it changes;
// Ocarina of Time declares min(peak, 3.5 x paper white). The answer moves in steps of about 6 % (40 to a decade), at least 1.25 times the paper white (the game's tone curve keeps
// some room above its white; the present rolls off what the screen cannot show, dm_hdr_rolloff) and at most 10,000 (the top of PQ). With no headroom known (no front end): 1000.
// A new step is taken once readings have stayed for a second (DM_HDR_PEAK_HOLD, ms) within a band of two neighboring steps on one side of the answer, and then the band's lower
// one, so auto-brightness drift and a drag of the slider do not re-tone-map the game at every frame, and a headroom flickering between two steps cannot hold the answer back.
// Unset: automatic
static pthread_mutex_t vi_peak_mu = PTHREAD_MUTEX_INITIALIZER;
// The step for a headroom: the game's white times it, in steps of about 6 %, within the floor and 10,000
static uint32_t vi_peak_step(float h) {
    double pw = dm_hdr_paper_white(), v = pow(10.0, round(log10(fmax(pw * h, 1.0)) * 40.0) / 40.0);
    uint32_t cand = (uint32_t)(round(fmin(v, 10000.0) / 10.0) * 10.0), floor_ = (uint32_t)(ceil(1.25 * pw / 10.0) * 10.0);
    if (cand < floor_) cand = floor_;
    if (cand > 10000) cand = 10000;
    if (cand < 10) cand = 10;   // a paper white under 8 nits: never a peak of 0
    return cand;
}
// The answer: the first one is the screen's potential headroom, not the current one, which is 1.0 until the HDR layer is shown and then ramps up over about two seconds (front_mac.m
// front_hdr_headroom), so a game that reads its peak once is not held at the floor for the run; it comes down to the current headroom through the hold below, and the present rolls
// off what the screen cannot show meanwhile (dm_hdr_rolloff)
static uint32_t vi_peak_held(uint32_t cand) {
    static uint32_t answered, band_lo, band_hi; static int64_t band_since;
    pthread_mutex_lock(&vi_peak_mu);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;   // under the lock: the times stay in order
    if (!answered) { float pot = front_hdr_potential(); uint32_t seed = isfinite(pot) && pot > 0.0f ? vi_peak_step(pot) : 0; answered = seed > cand ? seed : cand; }
    else if (cand == answered) band_lo = band_hi = 0;
    else {
        int same_side = band_lo && (band_lo > answered) == (cand > answered);
        uint32_t lo = same_side && band_lo < cand ? band_lo : cand, hi = same_side && band_hi > cand ? band_hi : cand;
        if (!same_side || (double)hi / lo > 1.08) { band_lo = band_hi = cand; band_since = now; }   // two neighboring steps differ by 6 % (7 % after rounding to 10 nits)
        else { band_lo = lo; band_hi = hi; }
        if (now - band_since >= vi_env_u32("DM_HDR_PEAK_HOLD", 1000)) { answered = band_lo; band_lo = band_hi = 0; }   // the hold in ms (DM_HDR_PEAK_HOLD; 0 in t_present_hdr)
    }
    uint32_t r = answered; pthread_mutex_unlock(&vi_peak_mu);
    return r;
}
static uint32_t vi_screen_peak(void) {
    float h = front_hdr_headroom();
    if (!(isfinite(h) && h > 0.0f)) return 1000;
    return vi_peak_held(vi_peak_step(h));
}
// DM_HDR_PEAK that is neither a number of nits (1 to 65535; larger is 65535) nor auto counts as unset, with one red line: "Auto" or "abc" told the game a peak of 0
static uint32_t vi_hdr_peak(int *automatic) {
    const char *e = getenv("DM_HDR_PEAK"); char *end = NULL; unsigned long v = 0;
    int set = e && *e, aut = set && !strcmp(e, "auto"), num = set && !aut && e[0] >= '0' && e[0] <= '9' && (v = strtoul(e, &end, 10)) > 0 && !*end;
    if (set && !aut && !num) { static atomic_int said; if (!atomic_exchange(&said, 1)) LOG("[RED] DM_HDR_PEAK=%s is neither a peak in nits (1 to 65535) nor auto: taken as unset", e); }
    *automatic = aut || !num;
    return *automatic ? vi_screen_peak() : v > 65535 ? 65535 : (uint32_t)v;
}
static _Atomic uint32_t g_vi_declared_hi;   // the extended range's top the game last declared, in nits (0 before it says)
static _Atomic uint32_t g_vi_told_peak;     // the peak the status query last answered with HDR on, in nits
// What the game may draw, in nits: the smaller of the range it declares and the peak it was told; 0 before either. Mario Kart World keeps its pixels within the peak (its brightest
// about 280 nits told 400, 357 told 1000 or 4000, at its title) but declares about 1.6 to 1.8 times it (628, 1801, 7206), so the declared range alone would compress pixels it never
// draws. A game that has declared nothing yet may draw up to the peak it was told. The present's roll-off and its SDR screenshots map this onto their target (present.metal)
float dm_hdr_source_peak(void) {
    uint32_t d = atomic_load(&g_vi_declared_hi), told = atomic_load(&g_vi_told_peak);
    if (told && (!d || told < d)) d = told;
    return (float)d;
}
// The present's roll-off (present.metal mode 4): when the game may draw more than the screen shows right now (100 x the current headroom, *target), its
// pixels above *knee are compressed smoothly into the target, the brightest channel setting the scale so the hue is kept; *source (dm_hdr_source_peak) lands on the target. The knee
// is three quarters of the target and never below the paper white, so the game's white is never touched; with less room than that (the knee above 90 % of the target) values above
// the target clip instead. A range within 5 % of the target is clipped too (the automatic peak is rounded to 10 nits and stepped). With the automatic peak the game is told about what
// the screen shows, so this is off while the screen holds still: it rolls off the second a new step waits for its hold after the screen dimmed, a peak floor above a dim screen,
// and a fixed peak (DM_HDR_PEAK=<n>) above the screen. The target follows the current headroom, not the held answer: the game's white is protected by the knee, and the screen
// clips at its current headroom whatever was answered. Returns 0 when nothing is to be rolled off
int dm_hdr_rolloff(float *knee, float *target, float *source) {
    float h = front_hdr_headroom(), d = dm_hdr_source_peak();
    if (!(isfinite(h) && h > 0.0f) || !(d > 0.0f)) return 0;
    float t = 100.0f * h, k = fmaxf(100.0f, 0.75f * t);
    if (d <= 1.05f * t || k > 0.9f * t) return 0;
    *knee = k; *target = t; if (source) *source = d; return 1;
}
// Set once the game's status query has answered HDR on: its RGB10A2 window then holds PQ code values (present_mode, nvn_mtl.m); an RGB10A2 window of a game that never asked for HDR is not decoded.
// It stays set when the answer later turns SDR (another screen, HDR output turned off): the game keeps its HDR window until it asks again, and the present still decodes it; a game
// whose SDR window were RGB10A2 would then be decoded as PQ. None of the games here is: their SDR windows are RGBA8 (Donkey Kong Bananza, Mario Kart World, Ocarina of Time)
static _Atomic int g_vi_hdr_answered;
int vi_hdr_answered(void) { return atomic_load(&g_vi_hdr_answered); }
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR status: SDR output, or HDR on with DM_HDR=1 once the window has the HDR layer on a screen that can show EDR", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp;
    int mode = dm_sdk_ounce() ? dm_hdr_mode() : 0;
    if (mode) atomic_store(&g_vi_asked, 1);   // the front end makes the HDR layer at its next tick (dm_hdr_layer_tick)
    if (!mode && !atomic_load(&g_vi_hdr_live)) { if (on) *on = 0; if (fmt) *fmt = 0; if (peak) *peak = 0; return RES_OK; }   // off from the start: SDR, not logged
    // On a screen without EDR the game draws its own SDR picture, as a Switch 2 does on a TV without HDR10, rather than Core Animation mapping an HDR picture down
    int hdr = mode && vi_hdr_ready();
    int automatic = 0; uint32_t p = hdr ? vi_hdr_peak(&automatic) : 0;   // only with HDR on: an SDR layer's headroom (1) must not move the held automatic peak
    // Logged when the answer changes, and at the 1st, 2nd, 10th, 100th and 1000th query: whether the game asks again (and so can follow the window to another screen)
    static atomic_int queries, last = -1;
    int n = atomic_fetch_add(&queries, 1) + 1, was = atomic_exchange(&last, hdr);
    if (was != hdr || n == 1 || n == 2 || n == 10 || n == 100 || n == 1000)
        LOG("[nvn2-hdr] vi::QueryDisplayHdrStatus (query %d): %s", n, hdr ? "HDR on, format 1"
            : mode ? "SDR (DM_HDR is on: the window's screen cannot show EDR, or its HDR layer is not ready yet)" : !dm_sdk_ounce() ? "SDR (a Switch 1 game)" : "SDR (HDR output turned off while the game runs)");
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
                if (!was_p) LOG("[nvn2-hdr] vi::QueryDisplayHdrStatus: peak %u nits (automatic: the paper white × the screen's headroom, %.2f now, %.2f potential: the first answer takes the larger)", p, front_hdr_headroom(), front_hdr_potential());
                else LOG("[nvn2-hdr] vi::QueryDisplayHdrStatus: peak now %u nits (automatic: the screen's headroom %.2f; was %u, %d answer%s since that line)",
                         p, front_hdr_headroom(), was_p, c, c == 1 ? "" : "s");
            }
        }
    }
    if (hdr) atomic_store(&g_vi_told_peak, p);
    if (on) *on = (uint8_t)hdr; if (fmt) *fmt = (uint32_t)hdr; if (peak) *peak = hdr ? (uint16_t)p : 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display HDR paper white: 100 nits (the screen's white)", Result, h_vi_PaperWhite, uint16_t *nits, const void *disp) {
    (void)disp; uint16_t v = (uint16_t)lroundf(dm_hdr_paper_white());   // the same with HDR off: a game that reads it once keeps it when HDR is turned on while it runs (the iOS menu)
    static atomic_int n; if (vi_hdr_on() && atomic_fetch_add(&n, 1) < 3) LOG("[nvn2-hdr] vi::QueryDisplayHdrPaperWhite: %u nits", v);
    if (nits) *nits = v; return RES_OK; }
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
// The SDK's HDR metadata calls for the layer (the gamut the game declares, its tone mapping to SDR, its luminance ranges): the host sends none of them to the screen; the extended range bounds the roll-off.
// The first three of each are logged: the game's own statement of the gamut and luminance it targets, as it passed them (the luminance ranges are in nits: 0..203 and 0..1000 on Donkey Kong Bananza and Mario Kart World)
#define VI_META_LOG(...) do { static atomic_int n_; if (vi_hdr_on() && atomic_fetch_add(&n_, 1) < 3) LOG(__VA_ARGS__); } while (0)   // logged only while HDR is on
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "layer HDR metadata: dropped (the host sends none); the gamut is logged", Result, h_vi_HdrGamut, void *layer, int g) {
    (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrContentGamutMetadata: gamut %d (dropped)", g); return RES_OK; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "layer HDR metadata: dropped (the host sends none); the tone mapping is logged", Result, h_vi_HdrToneMap, void *layer, int m) {
    (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrToSdrToneMappingMetadata: tone mapping %d (dropped)", m); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: the extended luminance range bounds the roll-off (dm_hdr_source_peak); logged", Result, h_vi_HdrExtLum,
          void *layer, uint32_t lo, uint32_t hi) {
    (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata: %u..%u", lo & 0xffff, hi & 0xffff);
    // Each change after that is logged too, the first 20 (Mario Kart World declares 0..1000 nits while it makes its HDR window, then 0..1801 at its title)
    static _Atomic uint32_t last; static atomic_int changes; uint32_t r = (lo & 0xffff) | (hi & 0xffff) << 16, was = atomic_exchange(&last, r);
    if (vi_hdr_on() && was && was != r && atomic_fetch_add(&changes, 1) < 20) LOG("[nvn2-hdr] vi::SetLayerNextHdrExtendedContentLuminanceRangeMetadata: now %u..%u (was %u..%u)", r & 0xffff, r >> 16, was & 0xffff, was >> 16);
    if (vi_hdr_on()) atomic_store(&g_vi_declared_hi, hi & 0xffff); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: dropped (the host sends none); the luminance range is logged", Result, h_vi_HdrStdLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; VI_META_LOG("[nvn2-hdr] vi::SetLayerNextHdrStandardContentLuminanceRangeMetadata: %u..%u (dropped)", lo & 0xffff, hi & 0xffff); return RES_OK; }
// The rest of SDK 22's layer HDR metadata calls (the Bezier tone curve and the clears): the SDK forwards them through a table its graphics system fills, which is null on the host, so
// unanswered they call through null (Mario Kart World 1.8.0 sets the curve every frame, by pull request #1116)
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrBezier,
          void *layer, float a, float b, float c) { (void)layer; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrBezierClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrGamutClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrToneMapClear, void *layer) { (void)layer; return RES_OK; }
