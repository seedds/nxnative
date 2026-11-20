// HDR output's shared state (hdr.h): the setting (read once, never written), the window's screen's capability and headroom (the front end writes them on screen events and a 2 Hz
// poll), what the game was told (hle_vi.c) and the layer HDR metadata it sends. Atomics: the front end writes on the main thread, vi reads on the game's threads every frame, the
// present reads on the present thread
#include "../../host/dm.h"
#include "hdr.h"
#include <stdatomic.h>

static _Atomic int g_pref = -1, g_pw = -1;
int dm_hdr_pref(void) {
    int p = atomic_load(&g_pref);
    if (p >= 0) return p;
    const char *e = getenv("DM_HDR"); int bad, dflt = DM_GAME_HDR ? HDR_PREF_AUTO : HDR_PREF_OFF;
    p = hdr_pref_parse(e, dflt, &bad);
    int was = -1;
    if (atomic_compare_exchange_strong(&g_pref, &was, p)) {
        if (bad) LOG("[RED] [hdr] DM_HDR=%s: auto or 0; keeping %s", e, p ? "auto" : "0");
        else LOG("[hdr] DM_HDR=%s%s: %s", e && *e ? e : (p ? "auto" : "0"), e && *e ? "" : " (the game profile's default)",
                 p ? "HDR on a screen that can show it, switched live as the window moves between screens" : "never HDR");
    }
    return atomic_load(&g_pref);
}
int dm_hdr_paper_white(void) {
    int v = atomic_load(&g_pw);
    if (v > 0) return v;
    const char *e = getenv("DM_HDR_PAPER_WHITE"); v = hdr_paper_white_parse(e);
    if (v < 0) { LOG("[RED] [hdr] DM_HDR_PAPER_WHITE=%s: nits from %d to %d; keeping %d", e, HDR_PAPER_WHITE_MIN, HDR_PAPER_WHITE_MAX, HDR_PAPER_WHITE_DEFAULT); v = HDR_PAPER_WHITE_DEFAULT; }
    else if (e && *e) LOG("[hdr] paper white %d nits (DM_HDR_PAPER_WHITE)", v);
    atomic_store(&g_pw, v);
    return v;
}

static _Atomic int g_on, g_told;
static _Atomic uint32_t g_headroom, g_potential;   // f32 bits
static void f32_store(_Atomic uint32_t *a, float f) { uint32_t b; memcpy(&b, &f, 4); atomic_store(a, b); }
static float f32_load(_Atomic uint32_t *a) { uint32_t b = atomic_load(a); float f; memcpy(&f, &b, 4); return f; }
int dm_hdr_screen(double potential, double current) {
    hdr_decision d = {.pref = dm_hdr_pref()};
    int on = hdr_decision_screen(&d, potential);
    f32_store(&g_potential, (float)potential); dm_hdr_headroom_set(current); atomic_store(&g_on, on);
    return on;
}
void dm_hdr_headroom_set(double current) { f32_store(&g_headroom, current > 1 && isfinite(current) ? (float)current : 1.0f); }
int dm_hdr_on(void) { return atomic_load(&g_on); }
float dm_hdr_headroom(void) { float h = f32_load(&g_headroom); return h > 1 ? h : 1; }
int dm_hdr_answer(uint16_t *peak) {
    int on = atomic_load(&g_on);
    if (peak) *peak = on ? hdr_peak_nits(f32_load(&g_potential), dm_hdr_paper_white()) : 0;
    if (on && !atomic_load_explicit(&g_told, memory_order_relaxed)) atomic_store(&g_told, 1);
    return on;
}
int dm_hdr_told(void) { return atomic_load_explicit(&g_told, memory_order_relaxed); }

static _Atomic int g_meta[7];   // dm_hdr_meta's fields in order; set: any call seen
void dm_hdr_meta_set(int which, int a, int b) {
    switch (which) {
    case HDR_META_STD: atomic_store(&g_meta[0], a); atomic_store(&g_meta[1], b); break;
    case HDR_META_EXT: atomic_store(&g_meta[2], a); atomic_store(&g_meta[3], b); break;
    case HDR_META_GAMUT: atomic_store(&g_meta[4], a); break;
    case HDR_META_TONE: atomic_store(&g_meta[5], a); break;
    default: return;
    }
    atomic_store(&g_meta[6], 1);
}
void dm_hdr_meta_get(dm_hdr_meta *m) {
    int v[7]; for (int i = 0; i < 7; i++) v[i] = atomic_load(&g_meta[i]);
    *m = (dm_hdr_meta){v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
}
