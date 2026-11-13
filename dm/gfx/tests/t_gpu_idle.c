// GPU idle accounting (nvn_gpuidle.h): synthetic spans of tagged command buffers, the numbers hand-calculated.
// All times are nanoseconds; MS and US only name the magnitudes the host sees.
#include "../nvn/nvn_gpuidle.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", why); } } while (0)

int main(void) {
    const uint64_t MS = 1000000, US = 1000;
    // Idle between frames: consecutive frames with a known gap (the Animal Crossing drop windows: about 2.4 ms of GPU nothing a frame)
    {
        gpuidle_tracker t = {0};
        CHECK(gpuidle_gap_ns(&t, 10, 1, 5 * MS, 14 * MS) == -1, "the first sampled frame has no previous end: not measured, and the tracker takes its span");
        CHECK(gpuidle_gap_ns(&t, 11, 1, 16400000, 22 * MS) == 2400000, "frame 11 starts 2.4 ms after frame 10's last GPU end");
        CHECK(t.frame == 11 && t.end_ns == 22 * MS && t.has, "the tracker holds the last sampled frame's end");
        CHECK(gpuidle_gap_ns(&t, 12, 1, 22 * MS + 5 * US, 30 * MS) == (int64_t)(5 * US), "a small gap is kept as it is");
        CHECK(gpuidle_gap_ns(&t, 13, 1, 30 * MS - 3 * US, 38 * MS) == 0, "the next frame starting before the previous one ended (overlap) is idle 0, not negative");
    }
    // A missing span is skipped, not counted as 0, and it breaks the chain: the next frame is not consecutive with the last one measured
    {
        gpuidle_tracker t = {0};
        CHECK(gpuidle_gap_ns(&t, 10, 1, 5 * MS, 14 * MS) == -1, "first frame: nothing to compare with");
        CHECK(gpuidle_gap_ns(&t, 11, 0, 0, 0) == -1, "frame 11's span is unknown (no tagged command buffer recorded times): skipped");
        CHECK(!t.has || t.frame == 10, "an unknown span does not advance the tracker");
        CHECK(gpuidle_gap_ns(&t, 12, 1, 20 * MS, 30 * MS) == -1, "frame 12 is not consecutive with frame 10: skipped, not counted as the 6 ms since frame 10's end");
        CHECK(gpuidle_gap_ns(&t, 13, 1, 30 * MS + 2 * MS, 40 * MS) == (int64_t)(2 * MS), "frame 13 pairs with frame 12 again");
    }
    // A non-consecutive frame (a frame number skipped between two known spans) is skipped, but the chain continues from the later frame
    {
        gpuidle_tracker t = {0};
        CHECK(gpuidle_gap_ns(&t, 10, 1, 5 * MS, 14 * MS) == -1, "first frame: not measured");
        CHECK(gpuidle_gap_ns(&t, 14, 1, 20 * MS, 30 * MS) == -1, "frame 14 after frame 10 (9, 8, ... missed their sample): skipped");
        CHECK(gpuidle_gap_ns(&t, 15, 1, 30 * MS + 1 * MS, 38 * MS) == (int64_t)(1 * MS), "frame 15 pairs with frame 14, the last one with a known span");
    }
    // Idle inside a frame: the span minus the sum of the command buffers' GPU times, clamped at 0 when they overlap
    {
        CHECK(gpuidle_inside_ns(16700000, 14200000) == 2500000, "span 16.7 ms, command buffers 14.2 ms: 2.5 ms of the span with nothing running");
        CHECK(gpuidle_inside_ns(10 * MS, 12 * MS) == 0, "overlapping command buffers (sum 12 > span 10 ms): idle 0, not negative");
        CHECK(gpuidle_inside_ns(10 * MS, 10 * MS) == 0, "command buffers exactly filling the span: idle 0");
    }
    // Busy share of a constructed window: the sum of the frames' GPU times over the window from the earliest start to the latest end
    {
        gpuidle_window w = {0};
        CHECK(gpuidle_window_busy(&w) == -1, "an empty window has no share");
        gpuidle_window_add(&w, 9 * MS, 0, 10 * MS);
        gpuidle_window_add(&w, 8 * MS, 12 * MS, 22 * MS);
        CHECK(w.n == 2 && w.t0_ns == 0 && w.t1_ns == 22 * MS && w.sum_ns == 17 * MS, "the window is the earliest start to the latest end of the frames counted");
        CHECK(gpuidle_window_busy(&w) > 77.0 && gpuidle_window_busy(&w) < 78.0, "17 ms of GPU work over a 22 ms window: about 77.3%");
        gpuidle_window_add(&w, 1 * MS, 25 * MS, 26 * MS);   // a frame the sum and the span disagree on would still widen the window only to its own end
        CHECK(gpuidle_window_busy(&w) > 69.0 && gpuidle_window_busy(&w) < 70.0, "adding frame 3: 18 ms over 26 ms, about 69.2%");
        gpuidle_window d = {0};
        gpuidle_window_add(&d, 5 * MS, 10 * MS, 10 * MS);   // a degenerate span (start == end) has no measurable window
        CHECK(gpuidle_window_busy(&d) == -1, "a window with no width: no share");
    }
    printf("gpu idle: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
