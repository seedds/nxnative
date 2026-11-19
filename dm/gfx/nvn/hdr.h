// HDR output (DM_HDR): a game that renders HDR itself is told so through vi (hle_vi.c QueryDisplayHdrStatus) while the window's screen can show it, and its HDR window texture
// is presented in extended dynamic range (nvn_mtl.m present_now, msl/present.metal fs_edr). Ocarina of Time (Switch 2) asks every frame and switches its window textures between
// RGBA8 (SDR) and RGB10A2 (HDR10: PQ, BT.2020) on the answer, live, both ways. The setting is read once and never written: the screen decides only whether it is in effect, so moving
// the window from a MacBook (HDR) to a Studio Display (SDR) and back turns HDR off and on again. The pure parts are here (t_hdr.c tests them); the shared state is hdr.c
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// A screen shows HDR when its potential EDR headroom (NSScreen.maximumPotentialExtendedDynamicRangeColorComponentValue) is at least this. Measured in the play test (2026-11-18):
// a MacBook Pro's built-in XDR panel 16.00; a Studio Display (no HDR) 2.00 at start, then 1.00 (backlight headroom below maximum brightness, unstable). 4 keeps the Studio Display
// SDR at either value and still takes an HDR panel (an XDR panel 16, the Pro Display XDR about the same; an EDR-capable screen below 4 gives too little room for highlights
// anyway). One constant: the front end's [hdr] line logs the potential and current values on every change
#define HDR_CAPABLE_POTENTIAL 4.0
// The peak the game is told (QueryDisplayHdrStatus): the screen's potential headroom × paper white, within [paper white, this]. 1600 nits is the MacBook Pro XDR panel's peak; the game
// itself caps it again (Ocarina of Time: min(peak, 1400 or its per-mode table))
#define HDR_PEAK_MAX_NITS 1600.0
// Paper white (QueryDisplayHdrPaperWhite, DM_HDR_PAPER_WHITE): the BT.2408 reference, 203 nits. The present maps it to EDR 1.0 (the screen's SDR white). Accepted range
#define HDR_PAPER_WHITE_DEFAULT 203
#define HDR_PAPER_WHITE_MIN 80
#define HDR_PAPER_WHITE_MAX 1000

enum { HDR_PREF_OFF = 0, HDR_PREF_AUTO = 1 };
// DM_HDR: "auto" (HDR where the screen can show it) or "0" (never); unset or empty = dflt (the game profile's: DM_GAME_HDR). Anything else: *bad = 1 and dflt
static inline int hdr_pref_parse(const char *e, int dflt, int *bad) {
    if (bad) *bad = 0;
    if (!e || !*e) return dflt;
    if (!strcmp(e, "auto")) return HDR_PREF_AUTO;
    if (!strcmp(e, "0")) return HDR_PREF_OFF;
    if (bad) *bad = 1;
    return dflt;
}
static inline int hdr_screen_capable(double potential) { return potential >= HDR_CAPABLE_POTENTIAL; }
static inline int hdr_decide(int pref, int capable) { return pref == HDR_PREF_AUTO && capable; }
// The whole decision as the front end drives it: pref is fixed at start, each screen event (moving screens, its parameters, full screen) gives the window's screen's
// potential headroom; returns whether HDR is in effect. Nothing here writes pref
typedef struct { int pref, capable, on; double potential; } hdr_decision;
static inline int hdr_decision_screen(hdr_decision *d, double potential) {
    d->potential = potential; d->capable = hdr_screen_capable(potential); d->on = hdr_decide(d->pref, d->capable);
    return d->on;
}
static inline uint16_t hdr_peak_nits(double potential, double paper_white) {
    double p = potential * paper_white;
    if (!(p >= paper_white)) p = paper_white;   // NaN too
    if (p > HDR_PEAK_MAX_NITS) p = HDR_PEAK_MAX_NITS;
    return (uint16_t)lround(p);
}
static inline int hdr_paper_white_parse(const char *e) {
    if (!e || !*e) return HDR_PAPER_WHITE_DEFAULT;
    char *end; long v = strtol(e, &end, 10);
    if (*end || v < HDR_PAPER_WHITE_MIN || v > HDR_PAPER_WHITE_MAX) return -1;
    return (int)v;
}

// SMPTE ST 2084 (PQ) EOTF: a code value 0..1 → absolute luminance in nits (0..10000). The same constants as msl/present.metal's pq_eotf
#define HDR_PQ_M1 (2610.0 / 16384.0)
#define HDR_PQ_M2 (2523.0 / 4096.0 * 128.0)
#define HDR_PQ_C1 (3424.0 / 4096.0)
#define HDR_PQ_C2 (2413.0 / 4096.0 * 32.0)
#define HDR_PQ_C3 (2392.0 / 4096.0 * 32.0)
static inline double hdr_pq_nits(double e) {
    if (!(e > 0)) return 0;
    if (e > 1) e = 1;
    double p = pow(e, 1.0 / HDR_PQ_M2), n = p - HDR_PQ_C1; if (n < 0) n = 0;
    return 10000.0 * pow(n / (HDR_PQ_C2 - HDR_PQ_C3 * p), 1.0 / HDR_PQ_M1);
}
// Linear BT.2020 → linear BT.709 (sRGB primaries), rows (ITU-R BT.2087). The same matrix as msl/present.metal's
static const double HDR_BT2020_TO_709[3][3] = {
    { 1.660491, -0.587641, -0.072850},
    {-0.124550,  1.132900, -0.008349},
    {-0.018151, -0.100579,  1.118730},
};
// One RGB10A2Unorm texel (R bits 0-9, G 10-19, B 20-29, A 30-31) → code values 0..1
static inline void hdr_rgb10a2_codes(uint32_t t, double c[3]) { for (int i = 0; i < 3; i++) c[i] = ((t >> (10 * i)) & 0x3ff) / 1023.0; }
// A PQ BT.2020 texel → extended linear sRGB with paper white = 1.0, clamped to the headroom above (negative values outside BT.709 are kept, as extended sRGB carries them):
// what fs_edr outputs
static inline void hdr_pq_to_edr(const double code[3], double paper_white, double headroom, double out[3]) {
    double n[3]; for (int i = 0; i < 3; i++) n[i] = hdr_pq_nits(code[i]) / paper_white;
    for (int i = 0; i < 3; i++) { double v = 0; for (int j = 0; j < 3; j++) v += HDR_BT2020_TO_709[i][j] * n[j]; out[i] = v < headroom ? v : headroom; }
}
// The screenshots' HDR → SDR curve (msl/present.metal fs_pq_sdr) on the largest channel m of linear BT.709 with paper white = 1: unchanged up to 0.8, then 1 − 0.2·e^(−(m − 0.8)/0.2)
static inline double hdr_sdr_shoulder(double m) { return m > 0.8 ? 1 - 0.2 * exp(-(m - 0.8) / 0.2) : m; }
static inline double hdr_srgb_encode(double c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055; }

// ---------- The shared state (hdr.c; any thread) ----------
int dm_hdr_pref(void);                                         // DM_HDR, read once (default: the game profile's hdr key, DM_GAME_HDR); never written
int dm_hdr_screen(double potential, double current);           // the front end, on every screen event: the window's screen's EDR headroom (potential, current); returns whether HDR is in effect
void dm_hdr_headroom_set(double current);                      // the front end's 2 Hz poll while HDR is in effect (the brightness slider sends no notification)
int dm_hdr_on(void);                                           // the setting and the screen allow HDR
float dm_hdr_headroom(void);                                   // the current EDR headroom, at least 1
int dm_hdr_paper_white(void);                                  // nits (DM_HDR_PAPER_WHITE, 203)
int dm_hdr_answer(uint16_t *peak);                             // vi::QueryDisplayHdrStatus: on, and the peak in nits; an "on" answer marks the game told (dm_hdr_told)
int dm_hdr_told(void);                                         // the game has been told HDR at least once: its RGB10A2 window textures are PQ from then on
enum { HDR_META_STD, HDR_META_EXT, HDR_META_GAMUT, HDR_META_TONE };
void dm_hdr_meta_set(int which, int a, int b);                 // the game's layer HDR metadata (hle_vi.c): luminance ranges (lo, hi nits), gamut, tone mapping (a)
typedef struct { int std_lo, std_hi, ext_lo, ext_hi, gamut, tone, set; } dm_hdr_meta;
void dm_hdr_meta_get(dm_hdr_meta *m);
