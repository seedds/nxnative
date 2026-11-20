// Movie picture under DM_NDVE_PICTURE=auto (nvn_ndve_picture.h): frames are given as their GPU span, the network's GPU time and the movie's picture period, in ms.
// At 60 Hz (16.67 ms): OVER is a span past 18.3 ms whose span without the network is at most 15 ms; UNDER a span of at most 15 ms.
#include "../nvn/nvn_ndve_picture.h"
#include <stdio.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

static const double P60 = 1.0 / 60;
// n frames of span and network time (ms) at period p (s); the picture after the last one
static int frames(ndve_movie *m, int n, double span, double net, double p) {
    int v = NA_MEASURING;
    for (int i = 0; i < n; i++) v = ndve_movie_picture(m, span * 1e-3, net * 1e-3, p);
    return v;
}
// The M1 Air's movie frames (the log of the first run): span 32.86 ms, network 24.35 ms
#define OVER 32.86, 24.35
#define UNDER 12.0, 6.0
#define LATE_ANYWAY 40.0, 10.0   // late, and late without the network too: 30 ms

int main(void) {
    {   ndve_movie m = {0};
        check(frames(&m, 2, OVER, P60) == NA_MEASURING, "the first two frames are warm-up: still measuring after two OVER");
        check(frames(&m, 2, OVER, P60) == NA_MEASURING, "two OVER after warm-up: still measuring");
        check(frames(&m, 1, OVER, P60) == NA_DECODED, "the third OVER after warm-up: decoded");
        check(frames(&m, 5, UNDER, P60) == NA_DECODED, "frames that fit afterwards: decoded stays for the rest of the movie");
    }
    {   ndve_movie m = {0};
        check(frames(&m, 2, UNDER, P60) == NA_MEASURING && frames(&m, 2, UNDER, P60) == NA_MEASURING, "warm-up and two UNDER: still measuring");
        check(frames(&m, 1, UNDER, P60) == NA_NETWORK, "the third UNDER after warm-up: the network");
        check(frames(&m, 5, OVER, P60) == NA_NETWORK, "OVER frames afterwards: the network stays for the rest of the movie");
    }
    {   ndve_movie m = {0}; frames(&m, 2, UNDER, P60);
        frames(&m, 2, OVER, P60); frames(&m, 1, UNDER, P60);
        check(frames(&m, 2, OVER, P60) == NA_MEASURING, "OVER, OVER, UNDER, OVER, OVER: the UNDER resets the OVER run");
        check(frames(&m, 1, OVER, P60) == NA_DECODED, "  a third OVER in a row: decoded");
        ndve_movie n = {0}; frames(&n, 2, UNDER, P60);
        frames(&n, 2, UNDER, P60); frames(&n, 1, 17.0, 6.0, P60);   // between: neither late nor under 0.9 of the period
        check(frames(&n, 2, UNDER, P60) == NA_MEASURING, "UNDER, UNDER, a frame neither OVER nor UNDER, UNDER, UNDER: still measuring");
        check(frames(&n, 1, UNDER, P60) == NA_NETWORK, "  a third UNDER in a row: the network");
    }
    {   ndve_movie m = {0};
        check(frames(&m, 20, LATE_ANYWAY, P60) == NA_MEASURING, "late frames that dropping the network would not make fit are not OVER");
        frames(&m, 2, OVER, P60); frames(&m, 1, LATE_ANYWAY, P60);
        check(frames(&m, 2, OVER, P60) == NA_MEASURING, "  and they reset the OVER run");
    }
    {   ndve_movie m = {0};
        check(frames(&m, 10, OVER, 0) == NA_MEASURING, "period 0 (no movie rate known): frames not counted");
        check(frames(&m, 2, OVER, P60) == NA_MEASURING, "  nor as warm-up: the next two frames are");
        check(frames(&m, 3, OVER, P60) == NA_DECODED, "  then three OVER: decoded");
        ndve_movie n = {0}; frames(&n, 2, OVER, P60); frames(&n, 2, OVER, P60);
        check(frames(&n, 3, OVER, 0) == NA_MEASURING && frames(&n, 1, OVER, P60) == NA_DECODED, "period 0 between OVER frames neither counts nor resets the run");
    }
    {   ndve_movie m = {0};
        check(ndve_movie_start(&m, 100), "the first network frame starts a movie");
        int starts = 0; for (uint64_t f = 101; f <= 160; f++) starts += ndve_movie_start(&m, f);
        check(!starts, "network frames one after another: the same movie");
        check(!ndve_movie_start(&m, 160 + NA_GAP), "NA_GAP - 1 frames without one: the same movie");
        frames(&m, 5, OVER, P60);
        check(ndve_movie_start(&m, 161 + 2 * NA_GAP), "NA_GAP frames without one: a movie starts");
        check(frames(&m, 2, UNDER, P60) == NA_MEASURING, "  decided before; measuring again, its first two frames warm-up");
        check(frames(&m, 3, UNDER, P60) == NA_NETWORK, "  and decided afresh: the network");
        ndve_movie n = {0}; ndve_movie_start(&n, 0); frames(&n, 4, OVER, P60);
        check(!ndve_movie_start(&n, 1) && frames(&n, 1, OVER, P60) == NA_DECODED, "frame 0 is a frame like any other: its run carries on at frame 1");
    }
    printf("%d checks, %d failed\n", checks, fail);
    return fail != 0;
}
