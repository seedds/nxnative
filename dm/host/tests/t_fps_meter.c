// The frame-rate counter (fps_meter.h): presented callbacks counted only for frames that reached the screen, the per-second rate the Mac front end reads twice a second,
// frame generation's frames apart from the game's own, and no reading across a stall
#include "../fps_meter.h"
#include <math.h>
#include <string.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

// One second of presented callbacks at `hz` from time t: every `drop`th one discarded by the system (presented time 0) when drop > 0, every `gen`th one a generated frame
static double run(fps_tally *t, double t0, int hz, int drop, int gen) {
    for (int i = 1; i <= hz; i++) fps_tally_presented(t, drop && i % drop == 0 ? 0.0 : t0 + (double)i / hz, gen && i % gen == 0);
    return t0 + 1;
}
static int read_at(fps_meter *m, fps_tally *t, double now, fps_reading *r) {
    return fps_meter_read(m, atomic_load(&t->shown), atomic_load(&t->generated), now, 1.25, r);
}

int main(void) {
    fps_tally t = {0}; fps_meter m = {0}; fps_reading r; char b[64];

    // Presented callbacks: on screen counts, discarded (0, negative, NaN) does not
    fps_tally_presented(&t, 12.5, 0); fps_tally_presented(&t, 0.0, 0); fps_tally_presented(&t, -1.0, 1); fps_tally_presented(&t, NAN, 1); fps_tally_presented(&t, 12.6, 1);
    CHECK(atomic_load(&t.shown) == 2 && atomic_load(&t.generated) == 1, "two frames on screen (one generated); three discarded callbacks not counted");

    // 60 presented callbacks a second, read every half second: the first read has nothing to compare, then 60 fps
    memset(&t, 0, sizeof t); fps_meter_reset(&m);
    CHECK(!read_at(&m, &t, 100.0, &r), "the first sample gives no reading");
    for (int i = 1; i <= 30; i++) fps_tally_presented(&t, 100.0 + i / 60.0, 0);
    CHECK(read_at(&m, &t, 100.5, &r) && NEAR(r.shown, 60) && NEAR(r.real, 60) && !r.generated, "half a second of 60 Hz callbacks reads 60 fps");
    for (int i = 31; i <= 60; i++) fps_tally_presented(&t, 100.0 + i / 60.0, 0);
    CHECK(read_at(&m, &t, 101.0, &r) && NEAR(r.shown, 60), "a full second (two half-second samples) reads 60 fps");
    fps_label(&r, b, sizeof b); CHECK(!strcmp(b, "60 fps"), "no generated frames: the label is the shown frames alone");

    // Discarded frames lower the count: 120 callbacks, every 4th discarded → 90 on screen
    memset(&t, 0, sizeof t); fps_meter_reset(&m); read_at(&m, &t, 200.0, &r);
    double now = run(&t, 200.0, 120, 4, 0);
    CHECK(read_at(&m, &t, now, &r) && NEAR(r.shown, 90), "discarded drawables are not counted: 90 of 120 callbacks");

    // Frame generation at 1.5x: 60 frames on screen a second, every third generated → 40 of the game's own
    memset(&t, 0, sizeof t); fps_meter_reset(&m); read_at(&m, &t, 300.0, &r);
    now = run(&t, 300.0, 60, 0, 3);
    CHECK(read_at(&m, &t, now, &r) && NEAR(r.shown, 60) && NEAR(r.real, 40) && r.generated, "frame generation: 60 on screen, 40 real");
    fps_label(&r, b, sizeof b); CHECK(!strcmp(b, "60 fps (40 real)"), "the label names the real frames too");
    // A generated frame discarded counts neither as shown nor as generated
    memset(&t, 0, sizeof t); fps_tally_presented(&t, 0.0, 1); fps_tally_presented(&t, 1.0, 0);
    CHECK(atomic_load(&t.shown) == 1 && atomic_load(&t.generated) == 0, "a discarded generated frame is not counted");

    // A stall (the app in the background, the main thread busy): no reading over a span longer than the gap, then readings resume
    memset(&t, 0, sizeof t); fps_meter_reset(&m); read_at(&m, &t, 400.0, &r);
    run(&t, 400.0, 60, 0, 0);
    CHECK(!read_at(&m, &t, 405.0, &r), "a sample 5 s after the last one gives no reading");
    now = run(&t, 405.0, 60, 0, 0);
    CHECK(read_at(&m, &t, now, &r) && NEAR(r.shown, 60), "the next half-second sample after a stall reads again, over the new span only");
    // A count that went backwards (a new tally) starts over instead of reading a huge or negative rate
    fps_tally z = {0};
    CHECK(!read_at(&m, &z, now + 0.5, &r), "a count lower than the last sample gives no reading");

    printf("fps meter: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
