// Unit test of the present queue's drain decision (../nvn/nvn_link_drain.h): which sequences of frames taken by the display-link callback ask the next acquire to wait
// more ticks, which drains end in the late latch, the backoff, and which leave the queue alone. Pure: no Metal, no clock; the times are the ones measured on Ocarina of Time
// (the game presents 1.15 ms after a tick).
#include "../nvn/nvn_link_drain.h"
#include <stdio.h>

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)

// Feed n frames taken one frame period apart, all with the same queue time, acquire wait and present after their wake (room 0: no latch; gpu_lead -1: unknown GPU timing);
// returns the first drain asked for (0 = none) and its frame in *at
static int feedlg(nvn_link_drain *d, double *now, int n, double period, double age, double tick, double blocked, int ok, double lead, double gpu_lead, double room, int *at) {
    for (int i = 0; i < n; i++) {
        *now += period;
        int k = nvn_link_drain_next(d, *now, age, tick, blocked, ok, lead, gpu_lead, room);
        if (k) { if (at) *at = i; return k; }
    }
    return 0;
}
static int feedl(nvn_link_drain *d, double *now, int n, double period, double age, double tick, double blocked, int ok, double lead, double room, int *at) { return feedlg(d, now, n, period, age, tick, blocked, ok, lead, -1, room, at); }
static int feed(nvn_link_drain *d, double *now, int n, double period, double age, double tick, double blocked, int ok, int *at) { return feedlg(d, now, n, period, age, tick, blocked, ok, -1, -1, 0, at); }

int main(void) {
    char label[160];
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
        nvn_link_drain_next(&d, now += T30, why == 0 ? T30 - W : 2 * T30 - W, T30, why == 1 ? 0 : 0.02, why != 2, -1, -1, 0);
        CHECK(d.run == 0, why == 0 ? "a frame taken on its first tick ends the run" : why == 1 ? "a frame the game did not wait before ends the run" : "a take that does not count ends the run");
    }
    CHECK(d.drains == 0, "interrupted runs never drain");
    CHECK(feed(&d, &now, 600, T30, 2 * T30 - W, T30, 0.02, 0, NULL) == 0, "takes that do not count (present snapshots, rescue) never drain");


    // With the late latch: the frame presented just after each tick (1.3 ms after its wake, room 8.3 ms on a fixed 120 Hz screen) stands too
    const double L = 0.0013, R = 0.0083, B = T60 - 0.004, A = T60 - L;   // present after the wake, room, the acquire's wait, the queue time of a frame taken one tick later
    CHECK(nvn_link_latch_spare(A, L, -1, R, T60) == 1, "a frame presented 1.3 ms after its wake and taken on the next tick: one tick");
    CHECK(nvn_link_latch_spare(2 * T60 - L, L, -1, R, T60) == 2, "and one that stood a whole tick more: two");
    CHECK(nvn_link_latch_spare(-0.0002, 0.0011, -1, R, T60) == 0, "a frame the latch took on the tick that woke it: none");
    CHECK(nvn_link_latch_spare(T60 - 0.006, 0.006, -1, R, T60) == -1, "a present past half the room: the latch would not have taken it");
    CHECK(nvn_link_latch_spare(A, -1, -1, R, T60) == -1 && nvn_link_latch_spare(A, L, -1, 0, T60) == -1, "no wake before it, or no latch: -1");
    CHECK(nvn_link_latch_spare(T60 - 0.004, 0.004, -1, 0.0165, T60) == 1, "4 ms of 16.5 ms room counts as early");
    CHECK(nvn_link_latch_spare(T60 - 0.0080, 0.0080, -1, 0.0165, T60) == 1, "8 ms of 16.5 ms room (a built-in ProMotion screen in its fixed 60 Hz mode) counts as early: half the room is inside the wait (14.4 ms)");
    CHECK(nvn_link_latch_spare(T60 - 0.0120, 0.0120, -1, 0.033, T60) == 1, "12 ms of 33 ms room (a window on that screen) counts as early: an eighth of a tick inside the wait (14.6 ms)");
    CHECK(nvn_link_latch_spare(T60 - 0.0130, 0.0130, -1, 0.033, T60) == -1, "but 13 ms does not, though inside the wait: a later frame 1.6 ms later would miss it");
    CHECK(nvn_link_latch_spare(2 * T60 - 0.0155, 0.0155, -1, 0.033, T60) == -1, "nor 15.5 ms, though under half the room: the wait ends 14.6 ms after the wake and the latch would not have taken it");
    CHECK(nvn_link_latch_spare(T60 - 0.0041, 0.0041, -1, 0.0083, T60) == 1 && nvn_link_latch_spare(T60 - 0.0082, 0.0082, -1, 0.0165, T60) == 1, "the measured limits stay (4.13 ms at 8.3 ms of room, 8.25 ms at 16.5 ms)");
    // GPU completion gate (issue #3251): CPU lead under limit still requires gpu_lead < room - tick / 8
    CHECK(nvn_link_latch_spare(T60 - 0.005, 0.005, 0.0150, 0.0165, T60) == -1, "early CPU (5 ms) with GPU finishing at 15 ms on 16.5 ms room does not enter the latch");
    CHECK(nvn_link_latch_spare(T60 - 0.005, 0.005, 0.0140, 0.0165, T60) == 1, "early CPU (5 ms) with GPU finishing at 14 ms fits 16.5 ms room with an eighth-tick margin");
    CHECK(nvn_link_latch_spare(T60 - 0.005, 0.005, 0.0170, 0.033, T60) == 1, "17 ms GPU fits a 33 ms ProMotion window room (30.9 ms limit)");
    CHECK(nvn_link_latch_spare(T60 - 0.005, 0.005, 0.0315, 0.033, T60) == -1, "31.5 ms GPU exceeds a 33 ms window room's margin");
    CHECK(nvn_link_latch_spare(A, L, 0.0055, R, T60) == 1 && nvn_link_latch_spare(A, L, 0.0065, R, T60) == -1, "8.3 ms room allows 5.5 ms GPU and rejects 6.5 ms GPU (6.2 ms limit)");
    d = (nvn_link_drain){0};
    CHECK(feedlg(&d, &now, 600, T60, T60 - 0.005, T60, B, 1, 0.005, 0.0155, 0.0165, NULL) == 0, "2 s of early CPU presents whose GPU finishes at 15.5 ms on 16.5 ms room never drain into the latch");
    d = (nvn_link_drain){0};
    CHECK(feedlg(&d, &now, 600, T60, 2 * T60 - 0.005, T60, B, 1, 0.005, 0.0155, 0.0165, NULL) == 1, "a standing frame with late GPU still gets a plain drain of one tick, not into the latch");
    d = (nvn_link_drain){0};
    CHECK(feed(&d, &now, 600, T60, A, T60, B, 1, NULL) == 0, "without the latch that frame waited under a tick: no drain");
    d = (nvn_link_drain){0};
    k = feedl(&d, &now, 600, T60, A, T60, B, 1, L, R, &at);
    CHECK(k == (1 | LINK_DRAIN_LATCH) && at >= 119 && at <= 121, "with it: after 2 s, one tick, into the latch");
    CHECK(d.entries == 1 && d.drains == 1 && d.in && d.stand == 1, "counted as a drain into the latch, and watched");
    d = (nvn_link_drain){0};
    CHECK(feedl(&d, &now, 600, T60, 2 * T60 - L, T60, B, 1, L, R, NULL) == (2 | LINK_DRAIN_LATCH), "a standing frame as well: one drain of two ticks, into the latch");
    d = (nvn_link_drain){0};
    CHECK(feedl(&d, &now, 600, T60, A, T60, 0.001, 1, L, R, NULL) == 0, "a game with no room in its acquire keeps the frame");
    d = (nvn_link_drain){0};
    feedl(&d, &now, 60, T60, A, T60, B, 1, L, R, NULL);
    nvn_link_drain_next(&d, now += T60, T60 - 0.009, T60, B, 1, 0.009, -1, 0.0165);
    CHECK(d.run == 0, "a present past half the room halfway ends the run");
    feedl(&d, &now, 60, T60, A, T60, B, 1, L, R, NULL);
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, -1, -1, R);
    CHECK(d.run == 0, "and so does a frame whose acquire no callback woke");
    d = (nvn_link_drain){0};
    feedl(&d, &now, 60, T60, 2 * T60 - L, T60, B, 1, L, R, NULL);
    feedl(&d, &now, 1, T60, 2 * T60 - 0.009, T60, B, 1, 0.009, 0.0165, NULL);
    CHECK(feedl(&d, &now, 600, T60, 2 * T60 - L, T60, B, 1, L, R, NULL) == 1, "a run of standing frames, one of them late after its wake: the plain drain of one tick, not into the latch");

    // The backoff: only drains into the latch count. A fallback soon after one doubles the time the next one needs; LINK_LATCH_KEEP latched frames in a row reset it
    d = (nvn_link_drain){0};
    for (int i = 0; i < 3; i++) { nvn_link_drain_next(&d, now += T60, -0.0002, T60, B, 1, 0.0011, -1, R); nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R); }
    CHECK(!d.back && !d.in, "presents latched on their own (a hitch emptied the queue), then standing frames: no drain into the latch, no backoff");
    k = feedl(&d, &now, 600, T60, A, T60, B, 1, L, R, NULL);
    CHECK(k == (1 | LINK_DRAIN_LATCH), "a drain into the latch");
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R);
    CHECK(!d.back && d.in, "its tick shows the frame that stood: not a fallback");
    for (int i = 0; i < 300; i++) nvn_link_drain_next(&d, now += T60, -0.0002, T60, B, 1, 0.0011, -1, R);
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R);
    CHECK(d.back == 1 && !d.in, "a fallback 300 latched frames after it (a hitch every 5 s): the next one needs twice the time");
    k = feedl(&d, &now, 1200, T60, A, T60, B, 1, L, R, &at);
    snprintf(label, sizeof label, "so it comes after 4 s (frame %d)", at);
    CHECK(k == (1 | LINK_DRAIN_LATCH) && at >= 238 && at <= 242, label);
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R); nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R);
    CHECK(d.back == 2 && !d.in, "a drain into the latch that latched nothing (two standing frames) backs off too");
    for (int r = 0; r < 8; r++) {
        feedl(&d, &now, 3000, T60, A, T60, B, 1, L, R, NULL);
        nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R); nvn_link_drain_next(&d, now += T60, -0.0002, T60, B, 1, 0.0011, -1, R); nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R);
    }
    CHECK(d.back == LINK_LATCH_BACK_MAX, "repeated fallbacks stop at LINK_LATCH_BACK_MAX doublings");
    feedl(&d, &now, 600, T60, A, T60, B, 1, L, R, NULL);   // early frames that stand under a tick, the drain into the latch still 22 s away
    k = feedl(&d, &now, 3000, T60, 2 * T60 - L, T60, B, 1, L, R, &at);
    CHECK(k == 1 && at >= 119 && at <= 121, "backed off, after early frames that stood under a tick, frames that stand a whole tick are still drained 2 s after they started, short of the latch");
    k = feedl(&d, &now, 3000, T60, A, T60, B, 1, L, R, &at);
    CHECK(k == (1 | LINK_DRAIN_LATCH) && at >= 1915 && at <= 1925, "and the drain into the latch comes after 32 s");
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R);
    for (int i = 0; i < LINK_LATCH_KEEP - 1; i++) nvn_link_drain_next(&d, now += T60, -0.0002, T60, B, 1, 0.0011, -1, R);
    CHECK(d.back == LINK_LATCH_BACK_MAX, "one latched frame short of LINK_LATCH_KEEP: still backed off");
    nvn_link_drain_next(&d, now += T60, -0.0002, T60, B, 1, 0.0011, -1, R);
    CHECK(!d.back, "LINK_LATCH_KEEP latched frames in a row: reset");
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, R);
    CHECK(!d.back && !d.in, "a fallback after holding that long does not back off");
    feedl(&d, &now, 600, T60, A, T60, B, 1, L, R, NULL);
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, 0);
    nvn_link_drain_next(&d, now += T60, A, T60, B, 1, L, -1, 0);
    CHECK(!d.back && !d.in, "the latch stopping for the frames (room 0) ends the watch without a verdict");

    printf("t_link_drain: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
