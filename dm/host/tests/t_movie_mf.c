// Media Foundation's H.264 and AAC transforms behind the movie decoder's interfaces (hle_movie_win32.c: mavc_* and maac_*), on streams written by tests/movie_streams.py
// without an encoder (its docstring): I_PCM pictures, a P picture of skipped macroblocks (a copy of its reference) and B pictures of skipped macroblocks (the average of their
// two references), so every decoded picture is known exactly; silent AAC frames. Checked: the pictures byte for byte in NV12 and within the conversion's rounding in RGBA, in
// presentation order with their times (B-frames reordered by the transform), the avcC record with length-prefixed units and in-band Annex B parameter sets, a size change
// (MF_E_TRANSFORM_STREAM_CHANGE), a flush in the middle of a stream, the drain at the end, and Media Foundation taken as absent (black pictures, no AAC decoder).
// Where a transform cannot be made or refuses the stream (an N edition without the Media Feature Pack; Wine without GStreamer's decoders) the program says why and skips.
#include "../hle_movie_win32.c"
#include "movie_streams.h"
#include <math.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

typedef struct { int n; int64_t pts[16]; uint8_t *pic[16]; } got_t;
static void got_free(got_t *g) { for (int i = 0; i < g->n; i++) free(g->pic[i]); g->n = 0; }
static void take_all(mavc *a, int flush, int w, int h, int bpp, got_t *g) {
    for (;;) {
        uint8_t *p = malloc((size_t)w * h * bpp / (bpp == 4 ? 1 : 2) + 16); int64_t t = -1;
        if (!mavc_take(a, flush, p, w, h, &t)) { free(p); return; }
        if (g->n < 16) { g->pts[g->n] = t; g->pic[g->n++] = p; } else free(p);
        if (!flush) return;   // one picture per access unit, as hle_movie.c takes them
    }
}
// Every access unit of s, a picture taken after each, then the end of the stream; bpp 3 (NV12, counted in halves) or 4 (RGBA)
static void decode(mavc *a, const mst_h264 *s, int bpp, got_t *g) {
    for (int i = 0; i < s->nau; i++) { CHECK(mavc_decode(a, s->au[i].p, s->au[i].n, s->au[i].pts_us) == 0, "access unit %d is taken", i); take_all(a, 0, s->w, s->h, bpp, g); }
    take_all(a, 1, s->w, s->h, bpp, g);
}
static int diff_nv12(const uint8_t *a, const uint8_t *b, int w, int h) { int n = 0; for (int i = 0; i < w * h * 3 / 2; i++) n += a[i] != b[i]; return n; }
static void check_nv12(const char *what, const mst_h264 *s, got_t *g) {
    CHECK(g->n == s->npic, "%s: %d pictures for %d access units (%d expected)", what, g->n, s->nau, s->npic);
    for (int i = 0; i < g->n && i < s->npic; i++) {
        CHECK(g->pts[i] == s->pic[i].pts_us, "%s: picture %d at %lld us, %lld expected (presentation order)", what, i, (long long)g->pts[i], (long long)s->pic[i].pts_us);
        int d = diff_nv12(g->pic[i], s->pic[i].nv12, s->w, s->h);
        CHECK(d == 0, "%s: picture %d differs in %d of %d bytes", what, i, d, s->w * s->h * 3 / 2);
    }
}
// RGBA of the expected NV12 in floating point, BT.709 or BT.601, video range
static void check_rgba(const char *what, const mst_h264 *s, got_t *g) {
    CHECK(g->n == s->npic, "%s: %d pictures (%d expected)", what, g->n, s->npic);
    double rv = s->matrix == 709 ? 1.5748 : 1.402, gu = s->matrix == 709 ? 0.1873 : 0.3441, gv = s->matrix == 709 ? 0.4681 : 0.7141, bu = s->matrix == 709 ? 1.8556 : 1.772;
    for (int i = 0; i < g->n && i < s->npic; i++) {
        const uint8_t *e = s->pic[i].nv12; int worst = 0;
        for (int y = 0; y < s->h; y++) for (int x = 0; x < s->w; x++) {
            double l = (e[y * s->w + x] - 16) * 255.0 / 219.0, u = (e[s->w * s->h + (y / 2) * s->w + (x & ~1)] - 128) * 255.0 / 224.0, v = (e[s->w * s->h + (y / 2) * s->w + (x & ~1) + 1] - 128) * 255.0 / 224.0;
            double c[3] = {l + rv * v, l - gu * u - gv * v, l + bu * u};
            for (int k = 0; k < 3; k++) { int want = (int)lround(c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k]), d = abs(g->pic[i][4 * (y * s->w + x) + k] - want); if (d > worst) worst = d; }
            if (g->pic[i][4 * (y * s->w + x) + 3] != 255) worst = 255;
        }
        CHECK(worst <= 1, "%s: picture %d is %d away from the BT.%d conversion (1 allowed: the 16-bit fixed point)", what, i, worst, s->matrix);
    }
}
static int is_black_nv12(const uint8_t *p, int w, int h) { for (int i = 0; i < w * h; i++) if (p[i] != 16) return 0; for (int i = w * h; i < w * h * 3 / 2; i++) if (p[i] != 128) return 0; return 1; }

int main(void) {
    const char *(*wine)(void) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    // Whether the transforms are there and take these streams: the first access unit of the Main stream, and an AAC decoder
    {
        mavc *a = mavc_new();
        mavc_decode(a, mst_main.au[0].p, mst_main.au[0].n, 0);
        int fail = a->failed; char why[200]; snprintf(why, sizeof why, "%s", a->why); mavc_free(a);
        int ch = 2, rate = 48000; maac *m = maac_new(mst_aac2.asc, mst_aac2.asc_n, &ch, &rate); int afail = !m; maac_free(m);
        if (fail || afail) {
            if (wine) printf("t_movie_mf: under Wine %s: %s%s%s; Wine makes these transforms from GStreamer's elements (winegstreamer: avdec_h264 and avdec_aac from gstreamer1.0-libav, "
                             "videoconvert and audioconvert from gstreamer1.0-plugins-base, h264parse from gstreamer1.0-plugins-bad), which this Wine does not have: skipped\n", wine(), fail ? why : "", fail && afail ? "; " : "",
                             afail ? "no AAC decoder (the [RED] line above)" : "");
            else printf("t_movie_mf: %s%s%s (a Windows N edition without the Media Feature Pack?): skipped\n", fail ? why : "", fail && afail ? "; " : "", afail ? "no AAC decoder (the [RED] line above)" : "");
            return 0;
        }
    }
    got_t g = {0};
    // 1. Baseline: the avcC record, length-prefixed units, a picture cropped from 64 to 60 lines, P pictures that copy their reference
    { mavc *a = mavc_new(); mavc_avcc(a, mst_base.avcc, (size_t)mst_base.avcc_n); decode(a, &mst_base, 3, &g); check_nv12("Baseline", &mst_base, &g); got_free(&g); mavc_free(a); }
    // 2. Main: parameter sets in the stream, B pictures reordered by the transform into presentation order, each the average of its references
    { mavc *a = mavc_new(); decode(a, &mst_main, 3, &g); check_nv12("Main", &mst_main, &g); got_free(&g); mavc_free(a); }
    // 3. RGBA (the native texture mode) by the SPS's matrix: BT.709 (Main), BT.601 (Baseline)
    { mavc *a = mavc_new(); mavc_set_rgba(a); decode(a, &mst_main, 4, &g); check_rgba("Main RGBA", &mst_main, &g); got_free(&g); mavc_free(a); }
    { mavc *a = mavc_new(); mavc_set_rgba(a); mavc_avcc(a, mst_base.avcc, (size_t)mst_base.avcc_n); decode(a, &mst_base, 4, &g); check_rgba("Baseline RGBA", &mst_base, &g); got_free(&g); mavc_free(a); }
    // 4. A flush in the middle (a seek: hle_movie.c md_reset), then the stream from its start: its pictures, none from before the flush
    {
        mavc *a = mavc_new();
        for (int i = 0; i < 3; i++) { mavc_decode(a, mst_main.au[i].p, mst_main.au[i].n, mst_main.au[i].pts_us); take_all(a, 0, mst_main.w, mst_main.h, 3, &g); }
        got_free(&g); mavc_drop(a);
        CHECK(mavc_queued(a) == 0, "a flush drops the queued pictures");
        decode(a, &mst_main, 3, &g); check_nv12("Main after a flush", &mst_main, &g); got_free(&g);
        // 5. then another stream of another size on the same decoder: the transform changes its output format (MF_E_TRANSFORM_STREAM_CHANGE)
        mavc_avcc(a, mst_base.avcc, (size_t)mst_base.avcc_n); decode(a, &mst_base, 3, &g); check_nv12("Baseline after Main (a size change)", &mst_base, &g); got_free(&g);
        CHECK(mavc_constant(a) == 0, "a decoder's pictures are the movie's (mavc_constant 0)");
        mavc_free(a);
    }
    // 6. Media Foundation taken as absent: one black picture per access unit at its time, in presentation order (the Main stream's 4 held until its end), constant
    {
        g_mf_test_absent = 1;
        mavc *a = mavc_new(); decode(a, &mst_main, 3, &g);
        int black = 1, sorted = 1; for (int i = 0; i < g.n; i++) { black &= is_black_nv12(g.pic[i], mst_main.w, mst_main.h); if (i) sorted &= g.pts[i - 1] < g.pts[i]; }
        CHECK(g.n == mst_main.nau && black && sorted, "no decoder: %d black pictures in order for %d access units", g.n, mst_main.nau);
        CHECK(mavc_constant(a) == 1, "no decoder: its pictures are all the same (mavc_constant 1)");
        got_free(&g); mavc_free(a);
        int ch = 2, rate = 48000; maac *m = maac_new(mst_aac2.asc, mst_aac2.asc_n, &ch, &rate);
        CHECK(!m, "no decoder: no AAC decoder either (the caller outputs silence)"); maac_free(m);
        g_mf_test_absent = 0;
    }
    // 7. AAC: the configuration's rate and channels when the MediaData has none, 1024 samples a frame, all zero (silent frames), PCM16 interleaved
    for (int k = 0; k < 2; k++) {
        const mst_aac *s = k ? &mst_aac1 : &mst_aac2; int ch = 0, rate = 0;
        maac *m = maac_new(s->asc, s->asc_n, &ch, &rate);
        CHECK(m && ch == s->channels && rate == s->rate, "AAC: %d channels %d Hz from the AudioSpecificConfig (%d, %d expected)", ch, rate, s->channels, s->rate);
        if (!m) continue;
        static int16_t pcm[8192 * 8]; long total = 0, nonzero = 0; int frames = 8, bad = 0, peak = 0;
        for (int i = 0; i < frames; i++) {
            int fr = maac_decode(m, s->frame, s->frame_n, pcm, 8192);
            if (fr < 0) bad++; else { total += fr; for (int j = 0; j < fr * ch; j++) { nonzero += pcm[j] != 0; if (abs(pcm[j]) > peak) peak = abs(pcm[j]); } }
        }
        CHECK(!bad && total >= (long)(frames - 2) * 1024 && total <= (long)frames * 1024 && total % 1024 == 0, "AAC %d ch: %ld samples per channel for %d frames of 1024", s->channels, total, frames);
        // Silence. Windows' decoder gives 16-bit PCM itself; Wine's decodes to floats (FFmpeg's avdec_aac) and GStreamer's audioconvert dithers them to 16 bits, which leaves
        // samples of -1 to 1 (about one in eight under Wine 11): within that rounding
        CHECK(wine ? peak <= 1 : nonzero == 0, "AAC %d ch: silent frames give silence (%ld samples not zero, the largest %d)", s->channels, nonzero, peak);
        maac_free(m);
    }
    printf("t_movie_mf: %d checks, %d failed%s\n", checks, fails, wine ? " (under Wine)" : "");
    return fails != 0;
}
