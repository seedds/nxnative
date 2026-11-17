// The port's H.264 stopgap (hle_movie_avc.c without VideoToolbox; docs/reference/movies.md, "H.264 movies without a decoder"): every access unit gives a black picture at its
// presentation time, in presentation order (a queue as deep as the reordering B-frames need, none for Baseline), and the end of the stream flushes the rest. No movie file:
// the access units' bytes do not matter to the stopgap, only their times and the SPS's profile byte. Before, the stopgap handed out no picture, and a player that waits for
// its pictures (Mario vs. Donkey Kong's) held the game's main thread
#if defined(_WIN32)
#include "../hle_movie_win32.c"   // Windows: these black pictures are Media Foundation's fallback where its H.264 transform is missing, taken as absent in main
#else
#include "../hle_movie_avc.c"
#endif
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

#if !defined(__APPLE__)
enum { W = 8, H = 4 };
static int black_nv12(const uint8_t *p) {
    for (int i = 0; i < W * H; i++) if (p[i] != 16) return 0;
    for (int i = W * H; i < W * H * 3 / 2; i++) if (p[i] != 128) return 0;
    return 1;
}
#endif

int main(void) {
#if defined(__APPLE__)
    puts("t_movie_avc: the port's stopgap only (Apple decodes with VideoToolbox): nothing to check");
    return 0;
#else
#if defined(_WIN32)
    g_mf_test_absent = 1;
#endif
    uint8_t nv12[W * H * 3 / 2], rgba[W * H * 4];
    const uint8_t au[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};   // an IDR slice's first bytes: the stopgap does not read them
    // 1. Main profile (B-frames): times in decode order, pictures in presentation order, one per access unit, each black
    {
        mavc *a = mavc_new();
        const int64_t dec[] = {0, 100000, 33333, 66666, 200000, 133333, 166666, 300000, 233333, 266666};
        int64_t got[16]; int n = 0, black = 1;
        for (unsigned i = 0; i < sizeof dec / sizeof *dec; i++) {
            CHECK(mavc_decode(a, au, sizeof au, dec[i]) == 0, "an access unit is taken");
            memset(nv12, 0xEE, sizeof nv12);
            int64_t t = -1;
            if (mavc_take(a, 0, nv12, W, H, &t)) { got[n++] = t; black &= black_nv12(nv12); }
        }
        CHECK(n == (int)(sizeof dec / sizeof *dec) - 4, "pictures leave once more than 4 times are queued (B-frames)");
        CHECK(mavc_queued(a) == 4, "4 pictures wait for the end of the stream");
        int64_t t = -1;
        while (mavc_queued(a) && n < 16) { memset(nv12, 0xEE, sizeof nv12); if (mavc_take(a, 1, nv12, W, H, &t)) { got[n++] = t; black &= black_nv12(nv12); } else break; }
        CHECK(n == (int)(sizeof dec / sizeof *dec), "the end of the stream flushes every queued picture: one picture per access unit");
        int sorted = 1;
        for (int i = 1; i < n; i++) sorted &= got[i - 1] < got[i];
        CHECK(sorted && got[0] == 0 && got[n - 1] == 300000, "pictures in presentation order");
        CHECK(black, "every picture is NV12 black (Y 16, UV 128)");
        CHECK(!mavc_take(a, 1, nv12, W, H, &t), "nothing after the last picture");
        mavc_free(a);
    }
    // 2. Baseline (its SPS in Configure's parameter sets: no B-frames): a picture for each access unit at once
    {
        mavc *a = mavc_new();
        const uint8_t sps[] = {0, 0, 0, 1, 0x67, 66, 0xC0, 0x1E};
        mavc_param_sets(a, sps, sizeof sps);
        int now = 1;
        for (int i = 0; i < 3; i++) { int64_t t = -1; mavc_decode(a, au, sizeof au, i * 33333); now &= mavc_take(a, 0, nv12, W, H, &t) == 1 && t == i * 33333; }
        CHECK(now && mavc_queued(a) == 0, "Baseline: each picture leaves with its own access unit");
        mavc_free(a);
    }
    // 3. The avcC record's profile (Main, 77) keeps the reordering queue; RGBA (native texture mode) pictures are opaque black; a flush or seek drops what is queued
    {
        mavc *a = mavc_new();
        const uint8_t avcc[] = {1, 77, 0, 30, 0xFF, 0xE1};
        mavc_avcc(a, avcc, sizeof avcc);
        mavc_set_rgba(a);
        int64_t t = -1;
        mavc_decode(a, au, sizeof au, 0);
        CHECK(!mavc_take(a, 0, rgba, W, H, &t), "Main: the first picture waits for the reordering queue");
        memset(rgba, 0x11, sizeof rgba);
        CHECK(mavc_take(a, 1, rgba, W, H, &t) == 1 && t == 0, "the end of the stream hands it out");
        int opaque = 1;
        for (int i = 0; i < W * H; i++) { uint32_t v; memcpy(&v, rgba + 4 * i, 4); opaque &= v == 0xff000000u; }
        CHECK(opaque, "RGBA pictures are opaque black");
        for (int i = 0; i < 3; i++) mavc_decode(a, au, sizeof au, i * 33333);
        mavc_drop(a);
        CHECK(mavc_queued(a) == 0 && !mavc_take(a, 1, rgba, W, H, &t), "a flush drops the queued pictures");
        CHECK(mavc_constant(a) == 1, "its pictures are all the same (mavc_constant): the native-texture output writes each texture once (hle_movie.c md_out_tex)");
        mavc_free(a);
    }
    printf("t_movie_avc: %d checks, %d failed\n", checks, fails);
    return fails != 0;
#endif
}
