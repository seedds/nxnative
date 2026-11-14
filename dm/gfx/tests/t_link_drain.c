// Unit test of the present queue's drain decision (../nvn/nvn_link_drain.h): which sequences of frames taken by the display-link callback ask the next acquire to wait
// more ticks, and which leave the queue alone. Pure: no Metal, no clock; the times are the ones measured on Ocarina of Time (the game presents 1.15 ms after a tick).
#include "../nvn/nvn_link_drain.h"
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)

// Feed n frames taken one frame period apart, all with the same queue time and acquire wait; returns the first drain asked for (0 = none) and its frame in *at
static int feed(nvn_link_drain *d, double *now, int n, double period, double age, double tick, double blocked, int ok, int *at) {
    for (int i = 0; i < n; i++) {
        *now += period;
        int k = nvn_link_drain_next(d, *now, age, tick, blocked, ok);
        if (k) { if (at) *at = i; return k; }
    }
    return 0;
}

int main(void) {
    const double T30 = 1.0 / 30, T60 = 1.0 / 60, W = 0.00115;
    nvn_link_drain d = {0}; double now = 100; int at = -1;

    // The spare ticks of a queue time
    CHECK(nvn_link_drain_spare(T30 - W, T30) == 0, "one 30 Hz tick less the present's delay: nothing to spare");
    CHECK(nvn_link_drain_spare(2 * T30 - W, T30) == 1, "a standing frame on the 30 Hz link: one tick");
    CHECK(nvn_link_drain_spare(2 * T60 - W, T60) == 1, "a standing frame on the 60 Hz link: one tick");
    CHECK(nvn_link_drain_spare(3 * T60 - W, T60) == 2, "a 33 ms hold on the 60 Hz link taken off the acquire's ticks, with a standing frame: two ticks");
    CHECK(nvn_link_drain_spare(T60 + 0.001, T60) == 0, "a frame that would land within the margin of its tick is not moved");
    CHECK(nvn_link_drain_spare(0.01, 0) == 0, "no tick period: nothing");

    // A game on time with no standing frame is never drained
    CHECK(feed(&d, &now, 600, T30, T30 - W, T30, T30 - 0.005, 1, NULL) == 0 && d.drains == 0, "30 fps on time, one tick in the queue: no drain");

    // The standing frame at 30 fps: one tick, after 2 s
    int k = feed(&d, &now, 600, T30, 2 * T30 - W, T30, T30 - 0.005, 1, &at);
    CHECK(k == 1, "a frame standing on the 30 Hz link: the acquire waits one tick more");
    CHECK(at >= 59 && at <= 61, "the drain comes after 2 s of standing frames, not before");
    CHECK(d.drains == 1 && d.run == 0, "the drain is counted and the run starts again");
    CHECK(feed(&d, &now, 600, T30, T30 - W, T30, T30 - 0.005, 1, NULL) == 0, "after the drain the frames wait one tick: no further drain");

    // 60 fps: the frame count is reached before the 2 s
    d = (nvn_link_drain){0};
    k = feed(&d, &now, 600, T60, 2 * T60 - W, T60, T60 - 0.004, 1, &at);
    CHECK(k == 1 && at >= 119 && at <= 121, "a frame standing on the 60 Hz link: one tick after 2 s");

    // A slow frame rate reaches 2 s before 30 frames: the frame count holds it back
    d = (nvn_link_drain){0};
    k = feed(&d, &now, 100, 0.1, 2 * T30 - W, T30, 0.02, 1, &at);
    CHECK(k == 1 && at == LINK_DRAIN_TAKES - 1, "at least 30 frames in a row whatever the time");

    // The 60 Hz link holding frames 33 ms, taken off the acquire's ticks with a standing frame (fixed-rate fullscreen at 30 fps)
    d = (nvn_link_drain){0};
    CHECK(feed(&d, &now, 600, T30, 3 * T60 - W, T60, 2 * T60 - 0.005, 1, NULL) == 2, "two ticks of the 60 Hz link");

    // The shortest queue time of the run decides
    d = (nvn_link_drain){0};
    feed(&d, &now, 40, T30, 3 * T30 - W, T30, 0.02, 1, NULL);
    feed(&d, &now, 1, T30, 2 * T30 - W, T30, 0.02, 1, NULL);
    CHECK(feed(&d, &now, 600, T30, 3 * T30 - W, T30, 0.02, 1, NULL) == 1, "one frame of the run waited only one tick too long: one tick, not two");

    // A game that does not wait in its acquire keeps its standing frame (frames of 16-17.5 ms at 60 fps: the queue is its slack)
    d = (nvn_link_drain){0};
    CHECK(feed(&d, &now, 600, T60, 2 * T60 - W, T60, 0, 1, NULL) == 0, "the game never waited for the link: no drain");
    CHECK(feed(&d, &now, 600, T60, 2 * T60 - W, T60, 0.001, 1, NULL) == 0, "a 1 ms wait is not room to spare");
    CHECK(feed(&d, &now, 600, T30, 2 * T30 - W, T30, 0.004, 1, NULL) == 0, "4 ms of a 33 ms tick is not room to spare");

    // One frame without room, an emptied queue or a take that does not count starts the run again
    d = (nvn_link_drain){0};
    for (int i = 0; i < 20; i++) {
        CHECK(feed(&d, &now, 50, T30, 2 * T30 - W, T30, 0.02, 1, NULL) == 0, "50 standing frames are under 2 s");
        int why = i % 3;
        nvn_link_drain_next(&d, now += T30, why == 0 ? T30 - W : 2 * T30 - W, T30, why == 1 ? 0 : 0.02, why != 2);
        CHECK(d.run == 0, why == 0 ? "a frame taken on its first tick ends the run" : why == 1 ? "a frame the game did not wait before ends the run" : "a take that does not count ends the run");
    }
    CHECK(d.drains == 0, "interrupted runs never drain");
    CHECK(feed(&d, &now, 600, T30, 2 * T30 - W, T30, 0.02, 0, NULL) == 0, "takes that do not count (present snapshots, rescue) never drain");

    printf("t_link_drain: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
