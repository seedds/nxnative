// Controller motion fusion (motion_fusion.h): axis maps, units, orientation from the gyro, tilt correction toward gravity, and the SixAxisSensorState fields. No host, no controller
#include "../motion_fusion.h"
#include <stdio.h>
#include <float.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void run(motion_state *s, const motion_map *m, float ax, float ay, float az, float wx, float wy, float wz, float secs, float k) {
    float a[3] = {ax, ay, az}, w[3] = {wx, wy, wz};
    int n = (int)(secs * 250 + 0.5f);
    for (int i = 0; i < n; i++) motion_update(s, m, a, w, 1 / 250.f, k);
}


static int finite_state(const motion_state *s) {
    for (int i = 0; i < 3; i++) {
        if (!isfinite(s->acc[i]) || !isfinite(s->gyro[i]) || !isfinite(s->angle[i])) return 0;
        for (int j = 0; j < 3; j++) if (!isfinite(s->R[i][j])) return 0;
    }
    return 1;
}

static int same_rotation(const motion_state *a, const motion_state *b) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (a->R[i][j] != b->R[i][j]) return 0;
    return 1;
}
static int same_state(const motion_state *a, const motion_state *b) {
    if (a->primed != b->primed || !same_rotation(a, b)) return 0;
    for (int i = 0; i < 3; i++) if (a->acc[i] != b->acc[i] || a->gyro[i] != b->gyro[i] || a->angle[i] != b->angle[i]) return 0;
    return 1;
}

static void invalid_samples(void) {
    motion_map m; motion_map_parse(&m, NULL);
    float bad[] = {NAN, INFINITY, -INFINITY};
    // Reject the whole sample, even before priming; the next valid sample must
    // give exactly the result it would have given without the rejected one.
    for (int primed = 0; primed < 2; primed++) for (int sensor = 0; sensor < 2; sensor++)
    for (int axis = 0; axis < 3; axis++) for (int b = 0; b < 3; b++) {
        float a[3] = {0, 0, -1}, w[3] = {0.1f, -0.2f, 0.3f};
        motion_state s; motion_reset(&s);
        if (primed) motion_update(&s, &m, a, w, 0, 0.5f);
        motion_state before = s, expected = s;
        (sensor ? w : a)[axis] = bad[b];
        motion_update(&s, &m, a, w, 0.004f, 0.5f);
        CHECK(same_state(&s, &before), "non-finite sample leaves every field unchanged");
        a[0] = a[1] = 0; a[2] = -1; w[0] = 0.1f; w[1] = -0.2f; w[2] = 0.3f;
        motion_update(&s, &m, a, w, 0.004f, 0.5f);
        motion_update(&expected, &m, a, w, 0.004f, 0.5f);
        CHECK(finite_state(&s) && same_state(&s, &expected), "valid sample recovers without resetting fusion");
    }
    float a[3] = {0.2f, 0, -0.98f}, w[3] = {0.1f, 0.2f, 0.3f};
    float times[] = {NAN, INFINITY, -INFINITY, -0.004f, 0, 0.250001f};
    for (int i = 0; i < 6; i++) {
        motion_state s; motion_reset(&s); motion_update(&s, &m, a, w, 0, 0.5f);
        motion_state expected = s;
        motion_update(&s, &m, a, w, times[i], 0.5f);
        motion_update(&expected, &m, a, w, 0, 0.5f);
        CHECK(finite_state(&s) && same_state(&s, &expected), "invalid time or gap reports a sample without integration");
    }
    float gains[] = {NAN, INFINITY, -INFINITY, -1};
    for (int i = 0; i < 4; i++) {
        a[0] = 0.2f;
        motion_state s; motion_reset(&s); motion_update(&s, &m, a, w, 0, 0);
        motion_state expected = s;
        a[0] = -0.2f; // Non-zero tilt error makes an infinite gain observable.
        motion_update(&s, &m, a, w, 0.004f, gains[i]);
        motion_update(&expected, &m, a, w, 0.004f, 0);
        CHECK(finite_state(&s) && same_state(&s, &expected), "invalid gain disables tilt correction");
    }
    // Finite inputs can overflow the float norm or accumulated angle too.
    motion_state s; motion_reset(&s); motion_update(&s, &m, a, w, 0, 0);
    motion_state before = s;
    float large[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    motion_update(&s, &m, a, large, 0.25f, 0);
    CHECK(finite_state(&s) && same_rotation(&s, &before), "overflowing rotation norm does not corrupt orientation");
    s.angle[0] = FLT_MAX;
    motion_update(&s, &m, a, large, 0.25f, 0);
    CHECK(finite_state(&s) && s.angle[0] == FLT_MAX, "overflowing angle does not poison accumulated revolutions");
}

static void mapping_and_precision(void) {
    int perms[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    for (int p = 0; p < 6; p++) for (int bits = 0; bits < 8; bits++) {
        char spec[] = "+x,+y,+z";
        for (int i = 0; i < 3; i++) { spec[3*i] = bits >> i & 1 ? '-' : '+'; spec[3*i+1] = "xyz"[perms[p][i]]; }
        motion_map m; CHECK(motion_map_parse(&m, spec), "every signed permutation parses");
        float a[3] = {0.2f, -0.3f, -0.9f}, w[3] = {0.1f, 0.2f, -0.3f};
        motion_state s; motion_reset(&s); motion_update(&s, &m, a, w, 0, 0);
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            ok &= near(s.acc[i], m.sign[i] * a[m.src[i]], 1e-6f);
            ok &= near(s.gyro[i], m.sign[i] * w[m.src[i]] * m.det / (2 * (float)M_PI), 1e-6f);
        }
        CHECK(ok && finite_state(&s), "all 48 frame maps retain acceleration and axial gyro conventions");
    }
    motion_map m; motion_map_parse(&m, NULL);
    motion_state s; motion_reset(&s);
    run(&s, &m, 0, 0, -1, 0, 0, 0.0001f, 300, 0);
    CHECK(finite_state(&s) && near(s.angle[2], 0.03f / (2 * (float)M_PI), 5e-6f), "small rotations retain precision over five minutes");
    CHECK(near(s.R[0][0], cosf(0.03f), 1e-4f) && near(s.R[1][0], sinf(0.03f), 1e-4f), "small rotations accumulate in orientation");
}

// The rest offset learner (motion_rest): a controller held still with a gyro offset learns it from two agreeing one-second windows and takes it off, ramped in; moving, a slow
// steady turn past 2 deg/s, or a later window far from the offset in use learns nothing; a break drops a pending window
static int feed(motion_rest *r, const float a[3], const float w[3], double t0, double secs, double step) {   // returns the samples fed
    int n = 0;   // an integer sample index, the time from it: a double stepped by `step` drifts from the sample count
    for (; n * step < secs; n++) motion_rest_update(r, a, w, t0 + n * step);
    return n;
}
static void rest_checks(void) {
    motion_rest r; motion_rest_reset(&r);
    float a[3] = {0, 0, -1}, w[3] = {0.006f, 0.0035f, -0.0115f}, o[3];   // the DualSense's 0.34, 0.20, -0.66 deg/s
    feed(&r, a, w, 0, 1.05, 0.015);
    CHECK(!r.have && r.pending, "rest: one still second is only a candidate");
    feed(&r, a, w, 1.05, 1.05, 0.015);
    CHECK(r.have && near(r.bias[0], w[0], 1e-5f) && near(r.bias[2], w[2], 1e-5f), "rest: two agreeing seconds: the offset");
    motion_rest_apply(&r, w, r.t_set, o); CHECK(near(o[0], w[0], 1e-6f), "rest: ramped in: nothing taken off at once");
    motion_rest_apply(&r, w, r.t_set + MOTION_REST_RAMP_S / 2, o); CHECK(near(o[0], w[0] / 2, 1e-5f), "rest: half after half the ramp");
    motion_rest_apply(&r, w, r.t_set + MOTION_REST_RAMP_S, o); CHECK(near(o[0], 0, 1e-6f) && near(o[2], 0, 1e-6f), "rest: all of it after the ramp");
    // a later window far from it (2 deg/s more) is no new offset; a near one is averaged in
    float far[3] = {w[0] + 0.035f, w[1], w[2]}, nearw[3] = {w[0] + 0.01f, w[1], w[2]};
    float b0 = r.bias[0]; motion_rest_break(&r); feed(&r, a, far, 10, 1.05, 0.015); motion_rest_break(&r); CHECK(r.bias[0] == b0, "rest: a later window 2 deg/s away is ignored");
    feed(&r, a, nearw, 20, 1.05, 0.015); CHECK(near(r.bias[0], w[0] + 0.005f, 1e-5f), "rest: a near one is averaged in");
    // a slow steady turn about gravity at 3 deg/s: no first offset
    motion_rest_reset(&r); float slow[3] = {0, 0, 0.0524f}; feed(&r, a, slow, 0, 5, 0.015);
    CHECK(!r.have, "rest: a slow steady turn (3 deg/s) is no offset");
    // turning or moving: nothing
    motion_rest_reset(&r);
    for (int i = 0; i < 300; i++) { float wm[3] = {0.5f * sinf(i * 0.2f), 0, 0}; motion_rest_update(&r, a, wm, i * 0.015); }
    CHECK(!r.have, "rest: a turning controller learns nothing");
    motion_rest_reset(&r);
    for (int i = 0; i < 300; i++) { float am[3] = {0.05f * sinf(i * 0.3f), 0, -1}; motion_rest_update(&r, am, w, i * 0.015); }
    CHECK(!r.have, "rest: a moving controller (acceleration) learns nothing");
    // a break between the two windows drops the candidate
    motion_rest_reset(&r); feed(&r, a, w, 0, 1.05, 0.015); motion_rest_break(&r); feed(&r, a, w, 1.05, 1.05, 0.015);
    CHECK(!r.have && r.pending, "rest: a break drops the pending window: the next is a new first");
    motion_rest_apply(&r, w, 5, o); CHECK(o[0] == w[0], "rest: nothing learned, nothing taken off");
    motion_rest_reset(&r); CHECK(!r.have && !r.pending, "rest: reset forgets");
}

// The playout delay: the 90th percentile of how late the next sample arrives, capped at 40 ms
static void lag_checks(void) {
    motion_lag l; motion_lag_reset(&l);
    CHECK(l.delay_ns == 0, "lag: none before a second sample");
    motion_lag_add(&l, 30000000); CHECK(l.delay_ns == 30000000, "lag: one spacing is the delay");
    for (int i = 0; i < 40; i++) motion_lag_add(&l, i % 10 == 9 ? 34000000 : 30000000);   // a report in ten 4 ms late
    CHECK(l.n == MOTION_LAG_N && l.delay_ns == 34000000, "lag: the 90th percentile covers the late ones that come one in ten");
    for (int i = 0; i < 40; i++) motion_lag_add(&l, i % 20 == 19 ? 60000000 : 30000000);   // a lost report in twenty
    CHECK(l.delay_ns == 30000000, "lag: a rare stall does not set the delay");
    for (int i = 0; i < 40; i++) motion_lag_add(&l, 90000000);
    CHECK(l.delay_ns == MOTION_LAG_MAX_NS, "lag: capped at 40 ms");
    motion_lag_add(&l, 5000000000ull); CHECK(l.need_us[(l.next + MOTION_LAG_N - 1) % MOTION_LAG_N] == 1000000, "lag: a silence counts as a second");
    motion_lag_reset(&l); for (int i = 0; i < 40; i++) motion_lag_add(&l, 15000000 + (uint64_t)(i % 4) * 1000000);
    CHECK(l.delay_ns == 18000000, "lag: 67 Hz with 3 ms of jitter: 18 ms");
}

int main(void) {
    motion_map m;
    CHECK(motion_map_parse(&m, NULL) && m.src[0] == 0 && m.det == 1, "default map is the identity");
    CHECK(motion_map_parse(&m, "x,-z,y") && m.src[1] == 2 && m.sign[1] == -1 && m.det == 1, "x,-z,y is a proper rotation");
    CHECK(motion_map_parse(&m, "y,x,z") && m.det == -1, "a swap is a mirror");
    CHECK(motion_map_parse(&m, "-x, y, z") && m.det == -1, "one sign flip is a mirror");
    CHECK(!motion_map_parse(&m, "x,x,z") && m.det == 1, "a repeated axis is refused (identity)");
    CHECK(!motion_map_parse(&m, "x,y") , "two entries are refused");

    // flat and still: identity orientation, acc (0,0,-1), no rotation
    motion_map id; motion_map_parse(&id, NULL);
    motion_state s; motion_reset(&s);
    run(&s, &id, 0, 0, -1, 0, 0, 0, 1, 0.5f);
    float acc[3], gyro[3], ang[3], dir[9];
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(acc[2], -1, 1e-5f) && near(dir[0], 1, 1e-4f) && near(dir[4], 1, 1e-4f) && near(dir[8], 1, 1e-4f), "flat: identity direction");

    // a quarter turn of yaw over one second (pi/2 rad/s): gyro 0.25 rev/s, angle 0.25 rev, the controller's x axis now points along world y (as the old shake's convention)
    motion_reset(&s);
    run(&s, &id, 0, 0, -1, 0, 0, (float)M_PI / 2, 1, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(gyro[2], 0.25f, 1e-4f), "gyro in revolutions / s");
    CHECK(near(ang[2], 0.25f, 0.01f), "angle integrates to a quarter revolution");
    CHECK(near(dir[0], 0, 0.02f) && near(dir[1], 1, 0.02f), "x axis turned to world y");

    // pitch: the top raised 30 degrees about x, with gravity measured to match: stays there (correction agrees with the gyro)
    motion_reset(&s);
    float th = 30 * (float)M_PI / 180;
    run(&s, &id, 0, 0, -1, 0, 0, 0, 0.1f, 0.5f);
    run(&s, &id, 0, 0, -1, th, 0, 0, 1, 0);          // turn by the gyro alone (no correction while the accelerometer lags)
    run(&s, &id, 0, -sinf(th), -cosf(th), 0, 0, 0, 2, 0.5f);   // then still, gravity seen tilted in the body
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(dir[5], sinf(th), 0.02f) && near(dir[4], cosf(th), 0.02f), "y axis raised 30 degrees");

    // tilt drift: the gyro says nothing while gravity shows a 20-degree roll: the correction pulls the orientation there
    motion_reset(&s);
    run(&s, &id, 0, 0, -1, 0, 0, 0, 0.1f, 0.5f);
    float r = 20 * (float)M_PI / 180;
    run(&s, &id, sinf(r), 0, -cosf(r), 0, 0, 0, 20, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(-dir[2], sinf(r), 0.03f), "roll pulled toward gravity");

    // a source that starts tilted is primed level with its gravity
    motion_reset(&s);
    run(&s, &id, sinf(r), 0, -cosf(r), 0, 0, 0, 0.01f, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(-dir[2], sinf(r), 0.02f), "primed from the first sample's gravity");

    // A phone held upright starts with gravity on -y, parallel to the old heading reference.
    // All six axis-aligned poses must map their measured gravity to world down on the first sample.
    for (int axis = 0; axis < 3; axis++) for (int sign = -1; sign <= 1; sign += 2) {
        float a[3] = {0}, w[3] = {0}; a[axis] = (float)sign;
        motion_reset(&s); motion_update(&s, &id, a, w, 0, 0.5f);
        CHECK(near(s.R[2][axis] * sign, -1, 1e-5f), "first sample: gravity points down in every starting pose");
    }

    // a frame map: Apple reports rotation about its y where the Switch has z (map "x,-z,y": switch y = -apple z, switch z = apple y)
    motion_map fm; motion_map_parse(&fm, "x,-z,y");
    motion_reset(&s);
    run(&s, &fm, 0, -1, 0, 0, (float)M_PI / 2, 0, 1, 0.5f);   // Apple: gravity along -y, rotating about +y
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(acc[2], -1, 1e-4f) && near(gyro[2], 0.25f, 1e-4f), "mapped: gravity on -z, yaw on z");
    CHECK(near(dir[1], 1, 0.02f), "mapped yaw turns x to world y");

    // the specific-force convention: +1 G up at rest becomes gravity down with DM_MOTION_ACC_SIGN=-1
    motion_map fs; motion_map_parse(&fs, NULL); fs.acc_sign = -1;
    motion_reset(&s);
    run(&s, &fs, 0, 0, 1, 0, 0, 0, 0.2f, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(acc[2], -1, 1e-5f) && near(dir[8], 1, 1e-4f), "acc sign flip");

    // orthonormal after a long noisy spin
    motion_reset(&s);
    for (int i = 0; i < 25000; i++) { float a[3] = {0, 0, -1}, w[3] = {0.7f * sinf(i * 0.01f), 1.3f, -0.4f}; motion_update(&s, &id, a, w, 1 / 250.f, 0.5f); }
    float d01 = s.R[0][0] * s.R[0][1] + s.R[1][0] * s.R[1][1] + s.R[2][0] * s.R[2][1], n0 = s.R[0][0] * s.R[0][0] + s.R[1][0] * s.R[1][0] + s.R[2][0] * s.R[2][0];
    CHECK(near(d01, 0, 1e-4f) && near(n0, 1, 1e-4f), "orientation stays orthonormal");

    // the phone's own sensors: held upright in landscape, screen toward the player, gravity is toward the bottom of the picture (Switch -y) whichever way it is turned
    float up3[3] = {-1, 0, 0}, up4[3] = {1, 0, 0}, flat[3] = {0, 0, -1}, o[3];
    motion_phone_to_switch(3, up3, o); CHECK(near(o[0], 0, 1e-6f) && near(o[1], -1, 1e-6f) && near(o[2], 0, 1e-6f), "landscape right upright: gravity to the picture's bottom");
    motion_phone_to_switch(4, up4, o); CHECK(near(o[0], 0, 1e-6f) && near(o[1], -1, 1e-6f), "landscape left upright: gravity to the picture's bottom");
    motion_phone_to_switch(3, flat, o); CHECK(near(o[2], -1, 1e-6f), "flat face up stays face up");
    // turning the phone (landscape right) about its own long side, which is the picture's x axis: rotation about phone -y becomes rotation about Switch +x
    float rl[3] = {0, -1, 0}; motion_phone_to_switch(3, rl, o); CHECK(near(o[0], 1, 1e-6f) && near(o[1], 0, 1e-6f), "rotation axes follow the picture");

    invalid_samples();
    rest_checks();
    lag_checks();
    mapping_and_precision();

    printf("t_motion: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
