// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md)
#include "hle.h"
#include <mach/mach.h>
#include <TargetConditionals.h>

// What a field holds: the frame rate as a float or int32, vsyncs per frame (60 / frame rate) as an int32, or the frame time in milliseconds as a float. Copies: consecutive copies of the value
// (Unreal keeps one for the game thread, one for the render thread). A row with no pointer (ptr_off 0) has its field at main + field: a constant in main's read-only data, which the loader
// leaves writable (the image's data block on iOS too, loader.c map_image).
// At 60 a frame time gets MS_MARGIN less than the vsync: such a limiter counts from the frame's start, and the work after it (the GPU fence wait, the next frame's start) adds to the
// period, so frames at exactly 16.667 ms came 0.11 ms late each on the Mac (59.6 fps, every 150th frame shown two vsyncs). Slightly shorter, the host's present pacing sets the pace
// (the acquire waits): Luigi's Mansion 3's display interval p99 18.69 → 16.67 ms, 60.0 fps. Not at 30: with present_interval display the presents follow every vsync and hold no
// frame for two, so a shorter time showed some frames for one (30.4 fps); 30 writes the game's own 33.333 ms
enum { FPS_F32, VSYNCS_I32, MS_F32, FPS_I32 };
#define MS_MARGIN 0.5f
static float ms_want(int fps) { return 1000.0f / (float)fps - (fps == 60 ? MS_MARGIN : 0); }
static const struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies; } k_locks[] = {
    // Pokemon Brilliant Diamond 1.3.0: Unity's Application.targetFrameRate. Its icall registration (main+0xe34e60) resolves to main+0xdc8e70 -> +0x5fa640,
    // which stores w0 at main+0x4afc868; the getter at +0x5fa630 reads the same int32. The game sets 30 when entering the field; display pacing alone does not remove that cap.
    { "94ceae325c205c4b9d6f7235552f28fd", "Unity targetFrameRate", 0, 0x4afc868, FPS_I32, 1 },
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Octopath Traveler 0 1.0.5: Unreal Engine 5's console variables t.MaxFPS (the game thread's frame limit: it sleeps until 1 / t.MaxFPS has passed and steps the game by the time that
    // really passed) and rhi.SyncInterval (vsyncs per present: the RHI's present interval). Each is a TAutoConsoleVariable in main's bss (vtable, the console object, then at +0x10 the pointer to
    // its value: the game thread's copy, then the render thread's). The game's config sets t.MaxFPS 30 ([ConsoleVariables] of DefaultEngine.ini; FrameRateLimit=30 of SwitchGameUserSettings.ini
    // sets it again) and rhi.SyncInterval 2 (SwitchDeviceProfiles.ini), so the game asks for present interval 2. Found from the names' references in the static constructors that register them
    // (main+0x2ca6524, main+0x1026e54; RegisterConsoleVariable, then AsVariableFloat / AsVariableInt stored at +0x10)
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal t.MaxFPS", 0x7c738d0 + 0x10, 0, FPS_F32, 2 },
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal rhi.SyncInterval", 0x7b32690 + 0x10, 0, VSYNCS_I32, 2 },
    // DRAGON QUEST XI S 1.0.4: Unreal Engine 4.18's t.MaxFPS and nvn.SyncInterval (the Switch RHI's present interval), the same TAutoConsoleVariable layout. The game's config sets
    // FrameRateLimit=30 (DefaultGameUserSettings.ini) and nvn.SyncInterval=2 (JackGame/Config/Switch/SwitchEngine.ini). Found from the names' references (UTF-32, as the engine's TCHAR on
    // the Switch) in the static constructors: main+0x1e7f98 (TAutoConsoleVariable<float> constructor main+0x6ee0e0, AsVariableFloat stored at +0x10) and main+0x21c00c
    { "1719aabfa5eae42b132b65770b871288", "Unreal t.MaxFPS", 0x8501c38 + 0x10, 0, FPS_F32, 2 },
    { "1719aabfa5eae42b132b65770b871288", "Unreal nvn.SyncInterval", 0x85270b8 + 0x10, 0, VSYNCS_I32, 2 },
    // DRAGON QUEST VII Reimagined 1.2.0: Unreal Engine 4.27's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16, the engine's TCHAR in this build). The game's config sets
    // FrameRateLimit=30 (DefaultGameUserSettings.ini, SwitchGameUserSettings.ini) and rhi.syncinterval=2 (SwitchDeviceProfiles.ini). Registered at main+0x4a61b40 (float, AsVariableFloat
    // stored at +0x10) and main+0x36164f4 (int, AsVariableInt at +0x10)
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "Unreal t.MaxFPS", 0x7f41908 + 0x10, 0, FPS_F32, 2 },
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "Unreal rhi.SyncInterval", 0x7dedf38 + 0x10, 0, VSYNCS_I32, 2 },
    // FINAL FANTASY RESONANCE 1.0.1: Unreal Engine 5.6.1's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16). The engine's Switch device profile sets rhi.syncinterval=2
    // (BaseSwitchDeviceProfiles.ini). Registered at main+0x33a4dec (float, IConsoleManager slot 2; AsVariableFloat stored at +0x10) and main+0x149e0bc (int, slot 1; AsVariableInt at +0x10)
    { "84e182f72ee1da4f477804bd05b54c15", "Unreal t.MaxFPS", 0x8c29af0 + 0x10, 0, FPS_F32, 2 },
    { "84e182f72ee1da4f477804bd05b54c15", "Unreal rhi.SyncInterval", 0x8adca10 + 0x10, 0, VSYNCS_I32, 2 },
    // OCTOPATH TRAVELER II 1.1.1: Unreal Engine 4.27's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16). The game's Switch device profile sets t.MaxFPS=30 and
    // rhi.syncinterval=2 (Majesty/Config/Switch/SwitchDeviceProfiles.ini; its Windows, PS4 and PS5 profiles set t.MaxFPS=60). Registered at main+0x2e50444 (float, AsVariableFloat stored at
    // +0x10) and main+0x134d380 (int, AsVariableInt at +0x10)
    { "0d9649011312f62e93f6a5673a09d7e9", "Unreal t.MaxFPS", 0x76fef58 + 0x10, 0, FPS_F32, 2 },
    { "0d9649011312f62e93f6a5673a09d7e9", "Unreal rhi.SyncInterval", 0x75ad0d0 + 0x10, 0, VSYNCS_I32, 2 },
    // SHIN MEGAMI TENSEI V: Vengeance 1.0.3: its Switch profile sets Project.FrameRateLimit=30 and rhi.syncinterval=2. The project variable (registered at +0xdd8624)
    // feeds GameUserSettings.FrameRateLimit at +0xb4 (+0xdd7d24 / +0xdd7f94), which applies it to t.MaxFPS. Write this source value so settings changes keep the target.
    // rhi.SyncInterval is registered at +0x2aa0754. Both use the same TAutoConsoleVariable value pointer at +0x10 and consecutive game/render-thread copies.
    { "0a4f14a5c12f470a4d3168a8f2ae6005", "Unreal Project.FrameRateLimit", 0x6d6d5d0 + 0x10, 0, FPS_F32, 2 },
    { "0a4f14a5c12f470a4d3168a8f2ae6005", "Unreal rhi.SyncInterval", 0x796de10 + 0x10, 0, VSYNCS_I32, 2 },
    // Luigi's Mansion 3 1.4.0: Next Level Games' frame limiter. At the end of each frame the main thread calls the frame callbacks (main+0xb7c308) with the milliseconds since the frame
    // started; the limiter (main+0x7a3e70) runs jobs or sleeps until 33.333 ms have passed, the float at main+0x13c94dc (read only there, at main+0x7a3ea8 and main+0x7a3f18), and the game
    // steps by the time that really passed. The game asks for present interval 2 too, which the profile's present_interval display ignores; both are needed
    { "79e5950ffa85acf63e28c9aec051eac2", "NLG frame limiter target", 0, 0x13c94dc, MS_F32, 1 },
};
#define NLOCKS ((int)(sizeof k_locks / sizeof k_locks[0]))

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }

// Unreal's render resolution held in game memory: DM_UE_SCREEN_PERCENT=<25..400> turns the game's dynamic resolution off (r.DynamicRes.OperationMode 0) and sets r.ScreenPercentage, the 3D
// views' size as a percentage of the back buffer; above 100 the engine renders larger and scales down (supersampling). Unset or 0 leaves the game's own. Written whenever they differ, as the frame
// limits above (the device profile sets them again on a mode change). Same TAutoConsoleVariable layout: the value pointer at +0x10, the game thread's copy then the render thread's
static const struct { const char *build; uint32_t opmode, sp; } k_uesp[] = {
    // Pikmin 4 1.1.1: docked the device profile (SwitchConsole) keeps the dynamic resolution between 66.666 and 83.333 % of 1920x1080. r.DynamicRes.OperationMode registered at
    // main+0x5b27e1c (TAutoConsoleVariable main+0x9fbfa60), r.ScreenPercentage at main+0x359fd38 (main+0x97f3ab0). On an M4 Pro, 200 % (3840x2160 scaled to 1080p) took the GPU from about 8 to 12 ms a frame
    { "060f827bd2b38703e9ea11e3210d89bc", 0x9fbfa60, 0x97f3ab0 },
};
static int g_uesp_row = -2; static uint8_t *g_uesp_base; static _Atomic float g_uesp_want;   // the build's row (-1 none), main's base, the percentage held (0 = the game's own)
static int uesp_row(void) {
    if (g_uesp_row != -2) return g_uesp_row;
    g_uesp_row = -1;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (int k = 0; k < (int)(sizeof k_uesp / sizeof k_uesp[0]); k++) if (!strcmp(g_mod[i].build_id, k_uesp[k].build)) { g_uesp_base = g_mod[i].base; g_uesp_row = k; }
    // the profile's ue_screen_percent (the Mac's default; an iPhone keeps the game's own unless chosen) unless the environment says
#if TARGET_OS_IPHONE
    const float def = 0;
#else
    const float def = DM_GAME_UE_SCREEN_PERCENT;
#endif
    const char *e = getenv("DM_UE_SCREEN_PERCENT"); float v = e ? strtof(e, NULL) : def;
    if (v != 0 && (v < 25 || v > 400)) { LOG("[RED] DM_UE_SCREEN_PERCENT=%g: must be 25 to 400; leaving the game's resolution", v); v = 0; }
    if (v != 0 && g_uesp_row < 0) LOG("DM_UE_SCREEN_PERCENT=%g: no address for this main build; ignored", v);
    g_uesp_want = g_uesp_row < 0 ? 0 : v;
    return g_uesp_row;
}
// The Mac window's key 2 (front_mac.m): the percentage held now (0 = the game's own, -1 = no address for this build), and the next of 100 → 150 → 200 (returns it; 0 when this build has no address)
int dm_ue_sp(void) { return g_uesp_row < 0 ? -1 : (int)g_uesp_want; }   // -1: no address (or not looked up yet)
int dm_ue_sp_cycle(void) {
    if (uesp_row() < 0) return 0;
    int w = (int)g_uesp_want, n = w < 100 ? 100 : w < 150 ? 150 : w < 200 ? 200 : 100;
    g_uesp_want = n; LOG("Unreal r.ScreenPercentage → %d (key 2)", n); return n;
}
static void ue_screen_percent_tick(void) {
    static int nlog; int state = uesp_row(); float want = g_uesp_want; uint8_t *base = g_uesp_base;
    if (state < 0 || want == 0) return;
    uint64_t om, sp;
    if (!rd((uint64_t)(uintptr_t)(base + k_uesp[state].opmode + 0x10), &om, sizeof om) || !om || !rd((uint64_t)(uintptr_t)(base + k_uesp[state].sp + 0x10), &sp, sizeof sp) || !sp) return;
    for (int c = 0; c < 2; c++) {
        int32_t *o = (int32_t *)(uintptr_t)(om + 4 * (uint64_t)c); float *s = (float *)(uintptr_t)(sp + 4 * (uint64_t)c); int32_t ov; float sv;
        if (!rd((uint64_t)(uintptr_t)o, &ov, sizeof ov) || !rd((uint64_t)(uintptr_t)s, &sv, sizeof sv)) return;
        if (ov != 0) { dm_guest_write_prepare(o, sizeof *o); __atomic_store_n(o, 0, __ATOMIC_RELAXED); }
        if (sv != want) { dm_guest_write_prepare(s, sizeof *s); *s = want; if (nlog < 6) { nlog++; LOG("main: Unreal r.ScreenPercentage%s %g → %g, dynamic resolution off (DM_UE_SCREEN_PERCENT)", c ? " (render thread)" : "", sv, want); } }
    }
}

void dm_fps_lock_tick(void) {
    ue_screen_percent_tick();
    static int state = -2, rows[NLOCKS], nrow, nlog[NLOCKS], target; static uint8_t *base; static const char *src;
    if (state == -1) return;
    if (state == -2) {
        state = -1;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
            for (int k = 0; k < NLOCKS; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = k; base = g_mod[i].base; }
        if (!nrow) return;
        const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET; src = e ? "DM_FPS_TARGET" : "game profile fps_target";
        if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; leaving the game's frame limit", e); t = 0; }
        if (!t) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", k_locks[rows[r]].what, src); return; }
        target = t; state = 0;
    }
    for (int r = 0; r < nrow; r++) {
        int k = rows[r]; uint64_t obj = (uint64_t)(uintptr_t)base;
        if (k_locks[k].ptr_off && (!rd((uint64_t)(uintptr_t)(base + k_locks[k].ptr_off), &obj, sizeof obj) || !obj)) continue;   // not created yet
        for (int c = 0; c < k_locks[k].copies; c++) {
            uint64_t a = obj + k_locks[k].field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (k_locks[k].kind == FPS_F32 || k_locks[k].kind == MS_F32) {
                int ms = k_locks[k].kind == MS_F32; float f, w = ms ? ms_want(target) : (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(ms ? f >= 8 && f <= 100 : f >= 10 && f <= 120)) continue;   // already set, or not set yet (RE Engine: 6000 at boot; Unreal: 0 = no limit until the config sets it)
                snprintf(was, sizeof was, ms ? "%.3f ms" : "%g", f);
            } else {
                int fps = k_locks[k].kind == FPS_I32;
                want = fps ? (uint32_t)target : 60 / (uint32_t)target;
                if (cur == want || cur < (fps ? 10u : 1u) || cur > (fps ? 120u : 4u)) continue;
                snprintf(was, sizeof was, "%u", cur);
            }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[k] < 8) { nlog[k]++; char now[24];
                if (k_locks[k].kind == MS_F32) snprintf(now, sizeof now, "%.3f ms (%d fps)", ms_want(target), target);
                else snprintf(now, sizeof now, "%u%s", k_locks[k].kind == FPS_F32 ? (unsigned)target : want, k_locks[k].kind == FPS_F32 || k_locks[k].kind == FPS_I32 ? " fps" : " vsyncs");
                LOG("main: %s%s %s → %s (%s)", k_locks[k].what, k_locks[k].copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, now, src); }
        }
    }
}

// The Mac's DM_CTL "cvarf <main offset> <value>" (hle_hid.c), for trying an Unreal console variable while the game runs: the TAutoConsoleVariable at main+offset holds at +0x10
// the pointer to its value, the game thread's copy then the render thread's; both are set to the float
void dm_ctl_cvar_f32(uint64_t off, float v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvarf main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { float *p = (float *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvarf main+%#llx = %g (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
// "peek <guest address> <count>": count floats (and their bits) from guest memory, read safely, to the log
void dm_ctl_peek(uint64_t a, int n) {
    for (int i = 0; i < n && i < 64; i += 4) {
        uint32_t w[4] = {0}; int k = n - i < 4 ? n - i : 4;
        if (!rd(a + 4 * (uint64_t)i, w, 4 * (size_t)k)) { LOG("DM_CTL: peek %#llx: not readable", (unsigned long long)(a + 4 * (uint64_t)i)); return; }
        float f[4]; memcpy(f, w, sizeof f);
        LOG("DM_CTL: peek %#llx: %g %g %g %g (%08x %08x %08x %08x)", (unsigned long long)(a + 4 * (uint64_t)i), f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
    }
}
// "memdump <guest address> <bytes> <path>": guest memory to a file, read safely (stops at the first unreadable page)
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] DM_CTL: memdump: cannot open %s", path); return; }
    static uint8_t buf[1 << 16]; uint64_t done = 0;
    while (done < n) { size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf; if (!rd(a + done, buf, k)) break; fwrite(buf, 1, k, f); done += k; }
    fclose(f); LOG("DM_CTL: memdump %#llx: %llu of %llu bytes → %s", (unsigned long long)a, (unsigned long long)done, (unsigned long long)n, path);
}
// "cvari <main offset> <value>": the same for an int console variable
void dm_ctl_cvar_i32(uint64_t off, int32_t v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvari main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { int32_t *p = (int32_t *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvari main+%#llx = %d (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
