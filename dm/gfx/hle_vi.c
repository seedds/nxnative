// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
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
// through null from SetLayerNextHdrContentGamutMetadata, main+0x2036f4). The host answers them itself.
// Native HDR: a Switch 2 game with HDR of its own asks whether the display is in HDR (QueryDisplayHdrStatus: on, the output format and the display's peak in nits) and for the
// paper white the player set in the console's HDR settings (QueryDisplayHdrPaperWhite, nits), and while the display is in HDR draws its swap chain as HDR10: RGB10A2 holding
// PQ-encoded BT.2020 (format 1 answered; NVN format 0x3d where SDR is 0x25), with SDR content at paper white. It then sets the layer's metadata every frame: standard content
// up to its paper white times its own factor, extended content up to the peak, the gamut (The Legend of Zelda: Ocarina of Time: sead's display setup at main+0x346c6a0 and
// main+0x5703f0; 0..203, 0..600, gamut 0 when told 600 and 203). The display is in HDR here while the game's screen shows HDR (front_hdr_potential: macOS's potential EDR
// headroom above 1.05) and the player has neither turned HDR off (DM_HDR=0) nor native HDR (DM_HDR_NATIVE=0); the present then decodes the picture (present.metal, src_pq) with
// the paper white as SDR white. The player's calibration (Cartridge, per screen) gives the peak and the paper white: DM_HDR_PEAK_NITS (default the screen's potential headroom
// times the paper white) and DM_HDR_PAPER_WHITE (default 203 nits, BT.2408). A game reads them once at start: a calibration change takes effect at its next start
__attribute__((weak)) double front_hdr_potential(void) { return 0; }   // front_mac.m, front_ios.m; graphics unit tests have none
static int vi_env_int(const char *k, int def, int lo, int hi) { const char *e = getenv(k); int v = e && *e ? atoi(e) : def; return v < lo ? lo : v > hi ? hi : v; }
static int vi_hdr_wanted(void) { const char *a = getenv("DM_HDR"), *b = getenv("DM_HDR_NATIVE"); return !(a && !strcmp(a, "0")) && !(b && !strcmp(b, "0")); }
static int vi_hdr_display(void) { return vi_hdr_wanted() && front_hdr_potential() > 1.05; }
// The player's calibration of the game's screen: DM_HDR_CALIBRATION="<screen name>=<peak>/<paper white>;…" (Cartridge, per screen; the name as macOS gives it, matched
// without case), 0 for a value the screen has none of
__attribute__((weak)) void front_screen_name(char *out, size_t n) { if (n) out[0] = 0; }   // front_mac.m; iOS and the tests have one screen and no calibration
static void vi_hdr_calibration(int *peak, int *paper) {
    *peak = *paper = 0; const char *e = getenv("DM_HDR_CALIBRATION"); char name[128]; if (!e || !*e) return;
    front_screen_name(name, sizeof name); if (!name[0]) return;
    for (const char *p = e; *p; ) {
        const char *end = strchr(p, ';'), *eq = memchr(p, '=', end ? (size_t)(end - p) : strlen(p)); size_t nl = eq ? (size_t)(eq - p) : 0;
        if (eq && nl == strlen(name) && !strncasecmp(p, name, nl)) { int a = 0, b = 0; if (sscanf(eq + 1, "%d/%d", &a, &b) >= 1) { *peak = a; *paper = b; } return; }
        if (!end) break; p = end + 1;
    }
}
static int vi_hdr_paper(void) { int pk, pw; vi_hdr_calibration(&pk, &pw); return vi_env_int("DM_HDR_PAPER_WHITE", pw > 0 ? pw : 203, 80, 500); }
static int vi_hdr_peak(void) {
    int pk, pw0; vi_hdr_calibration(&pk, &pw0); int pw = vi_hdr_paper(), def = pk > 0 ? pk : (int)(front_hdr_potential() * pw + 0.5);
    return vi_env_int("DM_HDR_PEAK_NITS", def > pw ? def : pw, pw, 10000);
}
static atomic_int g_hdr_native;   // the game asked and was told the display is in HDR: its swap chain is HDR10
int vi_hdr_native_white(void) { return atomic_load(&g_hdr_native) ? vi_hdr_paper() : 0; }   // nvn_mtl.m present_pipeline: the HDR10 picture's paper white, 0 for none
HLE_FORCE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", "display HDR status: HDR10 while the game's screen shows HDR, else SDR", Result, h_vi_HdrStatus,
          uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp; int hdr = vi_hdr_display(), pk = hdr ? vi_hdr_peak() : 0;
    if (on) *on = (uint8_t)hdr; if (fmt) *fmt = hdr ? 1 : 0; if (peak) *peak = (uint16_t)(pk > 65535 ? 65535 : pk);
    int was = atomic_exchange(&g_hdr_native, hdr);
    static atomic_int once; if (!atomic_exchange(&once, 1) || was != hdr)
        LOG("vi::QueryDisplayHdrStatus: %s (paper white %d nits, peak %d nits; screen HDR range %.2f times SDR white%s)", hdr ? "HDR, the game draws HDR10" : "SDR",
            vi_hdr_paper(), pk, front_hdr_potential(), vi_hdr_wanted() ? "" : "; DM_HDR=0 or DM_HDR_NATIVE=0");
    return RES_OK; }
HLE_FORCE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", "display HDR paper white: the player's (DM_HDR_PAPER_WHITE), 203 nits by default (BT.2408)", Result, h_vi_PaperWhite,
          uint16_t *nits, const void *disp) {
    (void)disp; if (nits) *nits = (uint16_t)vi_hdr_paper(); return RES_OK; }
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
HLE_FORCE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrGamut, void *layer, int g) {
    (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerNextHdrContentGamutMetadata %d (the present takes the picture as BT.2020)", g); return RES_OK; }
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrToneMap, void *layer, int m) {
    (void)layer; (void)m; return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrExtLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerNextHdrExtendedContentLuminanceRange %u..%u nits", lo, hi); return RES_OK; }
HLE_FORCE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrStdLum,
          void *layer, uint32_t lo, uint32_t hi) { (void)layer; static atomic_int once; if (!atomic_exchange(&once, 1)) LOG("vi::SetLayerNextHdrStandardContentLuminanceRange %u..%u nits", lo, hi); return RES_OK; }
// The rest of SDK 22's layer HDR metadata calls (the Bezier tone curve and the clears): the SDK forwards them through a table its graphics system fills, which is null on the host, so
// unanswered they call through null (Mario Kart World 1.8.0 sets the curve every frame, by pull request #1116)
HLE_FORCE("_ZN2nn2vi39SetLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerEfff", "layer HDR metadata: SDR output, nothing to apply", Result, h_vi_HdrBezier,
          void *layer, float a, float b, float c) { (void)layer; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrAdjustableBezierMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrBezierClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi37ClearLayerNextHdrContentGamutMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrGamutClear, void *layer) { (void)layer; return RES_OK; }
HLE_FORCE("_ZN2nn2vi41ClearLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerE", "layer HDR metadata cleared: SDR output", Result, h_vi_HdrToneMapClear, void *layer) { (void)layer; return RES_OK; }
