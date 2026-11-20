// HDR output's pure parts (nvn/hdr.h): DM_HDR's values, the per-screen decision through screen changes (the setting is never changed by them), the peak the game is told,
// paper white, the PQ (ST 2084) decode at known code values, the RGB10A2 unpack, the PQ → extended linear sRGB mapping fs_edr does and the screenshots' SDR curve.
// CPU only; the GPU side of the same mapping is in t_present_crop
#include "../nvn/hdr.h"
#include <stdio.h>
static int failed, cases;
#define EXPECT(n, w, v) do { long a = (w), b = (v); cases++; printf("%s expected %ld → got %ld %s\n", n, a, b, a == b ? "✓" : "✗"); failed += a != b; } while (0)
#define NEAR(n, w, v, tol) do { double a = (w), b = (v); cases++; int ok = fabs(a - b) <= (tol); printf("%s expected %.5g → got %.5g %s\n", n, a, b, ok ? "✓" : "✗"); failed += !ok; } while (0)

int main(void) {
    int bad;
    // DM_HDR: auto | 0, unset = the game profile's default, anything else reported and the default kept
    EXPECT("unset: the profile's default (auto)", HDR_PREF_AUTO, hdr_pref_parse(NULL, HDR_PREF_AUTO, &bad)); EXPECT("unset: not bad", 0, bad);
    EXPECT("unset: the profile's default (off)", HDR_PREF_OFF, hdr_pref_parse(NULL, HDR_PREF_OFF, &bad));
    EXPECT("empty: the default", HDR_PREF_AUTO, hdr_pref_parse("", HDR_PREF_AUTO, &bad));
    EXPECT("auto", HDR_PREF_AUTO, hdr_pref_parse("auto", HDR_PREF_OFF, &bad)); EXPECT("auto: not bad", 0, bad);
    EXPECT("0", HDR_PREF_OFF, hdr_pref_parse("0", HDR_PREF_AUTO, &bad)); EXPECT("0: not bad", 0, bad);
    const char *wrong[] = {"1", "on", "Auto", "yes", "2", " auto"};
    for (unsigned i = 0; i < sizeof wrong / sizeof *wrong; i++) { EXPECT(wrong[i], HDR_PREF_AUTO, hdr_pref_parse(wrong[i], HDR_PREF_AUTO, &bad)); EXPECT("reported", 1, bad); }

    // Capability: potential EDR headroom at least HDR_CAPABLE_POTENTIAL (4.0; the play test measured the XDR panel at 16, the Studio Display at 2 then 1)
    EXPECT("MacBook Pro XDR (16): capable", 1, hdr_screen_capable(16.0));
    EXPECT("SDR screen (1.0): not capable", 0, hdr_screen_capable(1.0));
    EXPECT("just above 1: not capable", 0, hdr_screen_capable(1.01));
    EXPECT("Studio Display at start (2.0): not capable", 0, hdr_screen_capable(2.0));
    EXPECT("3.99: not capable", 0, hdr_screen_capable(3.99));
    EXPECT("4.0: capable (the threshold is inclusive)", 1, hdr_screen_capable(4.0));
    EXPECT("0 (no screen value): not capable", 0, hdr_screen_capable(0));
    EXPECT("NaN: not capable", 0, hdr_screen_capable(NAN));
    // setting × capability
    EXPECT("auto × capable: on", 1, hdr_decide(HDR_PREF_AUTO, 1)); EXPECT("auto × not capable: off", 0, hdr_decide(HDR_PREF_AUTO, 0));
    EXPECT("0 × capable: off", 0, hdr_decide(HDR_PREF_OFF, 1)); EXPECT("0 × not capable: off", 0, hdr_decide(HDR_PREF_OFF, 0));

    // Screen changes: MacBook → Studio Display → MacBook, then clamshell on the Studio Display and the lid opened again; the setting is the same after every step
    const double screens[] = {16.0, 2.0, 16.0, 16.0, 1.0, 2.0, 16.0};   // the Studio Display as measured: 2 at start, then 1
    const int want_auto[] = {1, 0, 1, 1, 0, 0, 1};
    for (int pref = HDR_PREF_OFF; pref <= HDR_PREF_AUTO; pref++) {
        hdr_decision d = {.pref = pref};
        for (unsigned i = 0; i < sizeof screens / sizeof *screens; i++) {
            char name[96];
            snprintf(name, sizeof name, "DM_HDR=%s, screen %u (potential %.0f): %s", pref ? "auto" : "0", i, screens[i], pref && want_auto[i] ? "HDR" : "SDR");
            EXPECT(name, pref ? want_auto[i] : 0, hdr_decision_screen(&d, screens[i]));
            EXPECT("the setting is unchanged", pref, d.pref);
        }
    }

    // The peak the game is told: potential × paper white within [paper white, 1600]
    EXPECT("XDR, 203: 1600 (the cap)", 1600, hdr_peak_nits(16.0, 203));
    EXPECT("potential 4, 203: 812", 812, hdr_peak_nits(4.0, 203));
    EXPECT("potential 1.01, 203: at least paper white", 205, hdr_peak_nits(1.01, 203));
    EXPECT("potential 0.5: paper white", 203, hdr_peak_nits(0.5, 203));
    EXPECT("NaN: paper white", 203, hdr_peak_nits(NAN, 203));
    // DM_HDR_PAPER_WHITE
    EXPECT("paper white unset: 203", 203, hdr_paper_white_parse(NULL));
    EXPECT("paper white 250", 250, hdr_paper_white_parse("250"));
    EXPECT("paper white 79: refused", -1, hdr_paper_white_parse("79")); EXPECT("paper white 1001: refused", -1, hdr_paper_white_parse("1001"));
    EXPECT("paper white 2x: refused", -1, hdr_paper_white_parse("2x"));

    // PQ (SMPTE ST 2084) at known code values: (code, nits) from the inverse EOTF
    const double pq[][2] = {{0, 0}, {0.062337, 0.1}, {0.508078, 100}, {0.580689, 203}, {0.751827, 1000}, {0.902572, 4000}, {1.0, 10000}};
    for (unsigned i = 0; i < sizeof pq / sizeof *pq; i++) {
        char name[64]; snprintf(name, sizeof name, "PQ %.6f → %.1f nits", pq[i][0], pq[i][1]);
        NEAR(name, pq[i][1], hdr_pq_nits(pq[i][0]), fmax(pq[i][1] * 1e-3, 1e-4));
    }
    NEAR("PQ below 0 → 0", 0, hdr_pq_nits(-0.1), 0); NEAR("PQ above 1 → 10000", 10000, hdr_pq_nits(1.5), 1e-6);
    // 10-bit: code 594 (of 1023) is paper white to within a 10-bit step
    NEAR("10-bit code 594 → about 203 nits", 203, hdr_pq_nits(594 / 1023.0), 2.0);
    // RGB10A2 unpack: R bits 0-9, G 10-19, B 20-29
    double c[3]; hdr_rgb10a2_codes(1023u | 594u << 10 | 0u << 20 | 3u << 30, c);
    NEAR("RGB10A2 R", 1.0, c[0], 1e-9); NEAR("RGB10A2 G", 594 / 1023.0, c[1], 1e-9); NEAR("RGB10A2 B", 0, c[2], 1e-9);
    // PQ → extended linear sRGB: paper white grey is 1.0 in every channel (the matrix's rows sum to 1), 1000 nits grey 4.93, clamped at the headroom
    double g[3] = {0.580689, 0.580689, 0.580689}, e[3];
    hdr_pq_to_edr(g, 203, 8, e); for (int i = 0; i < 3; i++) NEAR("paper white grey → 1.0", 1.0, e[i], 2e-3);
    double k[3] = {0.751827, 0.751827, 0.751827}; hdr_pq_to_edr(k, 203, 8, e); NEAR("1000 nits grey → 4.93", 1000 / 203.0, e[1], 5e-3);
    hdr_pq_to_edr(k, 203, 3, e); NEAR("1000 nits grey at headroom 3 → 3", 3, e[0], 0);
    // a BT.2020 primary red at paper white: outside BT.709, so negative green and blue in extended sRGB (kept), red above 1
    double r[3] = {0.580689, 0, 0}; hdr_pq_to_edr(r, 203, 8, e);
    NEAR("BT.2020 red: R 1.66", 1.660491, e[0], 4e-3); EXPECT("BT.2020 red: G below 0", 1, e[1] < 0); EXPECT("BT.2020 red: B below 0", 1, e[2] < 0);
    // The screenshots' SDR curve: unchanged to 0.8, paper white 0.926 (sRGB 247), continuous at the knee, never above 1
    NEAR("SDR shoulder: 0.5 unchanged", 0.5, hdr_sdr_shoulder(0.5), 0);
    NEAR("SDR shoulder: paper white", 1 - 0.2 * exp(-1), hdr_sdr_shoulder(1.0), 1e-12);
    EXPECT("SDR shoulder: paper white in sRGB bytes", 247, lround(hdr_srgb_encode(hdr_sdr_shoulder(1.0)) * 255));
    NEAR("SDR shoulder: continuous at 0.8", 0.8, hdr_sdr_shoulder(0.8 + 1e-9), 1e-8);
    EXPECT("SDR shoulder: 50 × paper white stays below 1", 1, hdr_sdr_shoulder(50) <= 1.0);
    printf("== %d checks, %d failed ==\n", cases, failed);
    return failed != 0;
}
