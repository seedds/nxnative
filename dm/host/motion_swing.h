// A scripted swing of a controller (DM_KEYS / DM_CTL "Swing", hle_hid.c): the six-axis state of a controller that starts lying flat (gravity (0, 0, -1), the identity direction),
// turns about one of its own axes and comes back to where it began, for motion games that read a swing from the sensor (Nintendo Switch Sports Resort's racket, bat and sword).
// Pure C, no host state, so tests/t_motion.c checks it on any machine. Frame and units as motion_fusion.h: x right, y forward (away from the player), z up out of the face;
// acceleration in G in the six-axis state's convention (the gravity direction less the controller's own acceleration: (0, 0, -1) lying still face up), angular velocity in
// revolutions / s, angle in revolutions, direction rows = the controller's axes in the world.
// The motion: a backswing (half the swing's angle back, SWING_BACK_S), the swing (the whole angle forward at the given peak speed), a follow-back (half the angle back again,
// SWING_BACK_S), each a sin^2 speed profile, so the speed and the acceleration are continuous and the controller ends still where it started. The swing's fastest point, the impact,
// is SWING_BACK_S + half the swing after the start. The controller sits SWING_ARM_M ahead (+y) of the pivot (wrist / elbow), so a turn about x or z also gives it a centripetal and a
// tangential acceleration; each axis saturates at the sensor's range (SWING_ACC_MAX_G, the IC's 8 G), as a real swing does. A turn about y (the long axis) twists in place.
// The cap means the acceleration agrees with the gyro only below it: at 1500 deg/s the centripetal term (21 G at the impact) is clipped to 8 G, so a reader that compared |acc|
// with the angular velocity would see less pull than the turn implies (a real IC clips the same way; reconsider if a game is found to check it)
#pragma once
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SWING_DEG 180.0           // the swing's angle (a racket's forehand, from 90 degrees back to 90 degrees through)
#define SWING_BACK_S 0.6          // the backswing and the follow-back: 90 degrees each at a peak of 300 deg/s, slow enough not to read as a swing
#define SWING_DPS_DEFAULT 1500.0  // the swing's peak angular velocity (deg/s): a brisk swing, under the IC's 2000 deg/s range
#define SWING_DPS_MIN 200.0
#define SWING_DPS_MAX 2000.0
#define SWING_ARM_M 0.3           // pivot to controller (m)
#define SWING_ACC_MAX_G 8.0
#define SWING_GRAVITY 9.80665

typedef struct { int axis; double sign, peak_dps; int lift; } swing_spec;   // axis 0 x, 1 y, 2 z; sign +1 / -1 (right-handed about the axis); peak in deg/s (a lift: in G)

// "Swing", "Swing-z", "Swingx@1800": the axis with its sign (a "+" is read too, though DM_KEYS cuts its keys at "+") (default +z: lying flat, a turn counterclockwise seen from above) and the peak speed. Returns 0 on anything else
// A lift ("Lift", "Lift-z", "Liftx@4"): the controller moved along one of its own axes without turning, toward + (or - with the sign) of that axis: +z is out of its face, up
// when it lies flat. It is accelerated then braked over LIFT_S (a sine of acceleration A sin(2 pi t / T), so it ends still, moved by A T^2 / (2 pi): about 42 cm at the default
// 3 G), as a quick upward flick that tosses a ball. The reading on that axis is the gravity direction less the acceleration (lifting up flat: -1 - A while it speeds up, -1 + A
// while it brakes), capped at the IC's 8 G; the peak after "@" in G (LIFT_G_MIN..SWING_ACC_MAX_G)
#define LIFT_S 0.3
#define LIFT_G_DEFAULT 3.0
#define LIFT_G_MIN 0.2
static inline int swing_parse(const char *k, swing_spec *out) {
    static const char word[] = "swing", lword[] = "lift";
    int lift = 1;
    for (int i = 0; lword[i]; i++) if (!k[i] || (k[i] | 0x20) != lword[i]) { lift = 0; break; }
    if (!lift) for (int i = 0; word[i]; i++) if (!k[i] || (k[i] | 0x20) != word[i]) return 0;
    const char *s = k + (lift ? 4 : 5);
    swing_spec sp = {2, 1, lift ? LIFT_G_DEFAULT : SWING_DPS_DEFAULT, lift};
    if (*s == '-' || *s == '+') { sp.sign = *s == '-' ? -1 : 1; s++; if (!*s || *s == '@') return 0; }
    if (*s && *s != '@') { int a = (*s | 0x20) - 'x'; if (a < 0 || a > 2) return 0; sp.axis = a; s++; }
    if (*s == '@') {
        char *end; double v = strtod(s + 1, &end);
        if (end == s + 1 || *end || !(lift ? v >= LIFT_G_MIN && v <= SWING_ACC_MAX_G : v >= SWING_DPS_MIN && v <= SWING_DPS_MAX)) return 0;
        sp.peak_dps = v; s = end;
    }
    if (*s) return 0;
    *out = sp; return 1;
}
static inline double swing_seconds(double peak_dps) { return 2 * SWING_DEG / peak_dps; }   // the swing alone: a sin^2 profile covers its angle at half its peak on average
static inline double swing_duration(double peak_dps) { return 2 * SWING_BACK_S + swing_seconds(peak_dps); }
static inline double swing_impact(double peak_dps) { return SWING_BACK_S + swing_seconds(peak_dps) / 2; }
static inline double swing_spec_duration(const swing_spec *sp) { return sp->lift ? LIFT_S : swing_duration(sp->peak_dps); }
static inline double swing_spec_impact(const swing_spec *sp) { return sp->lift ? LIFT_S / 4 : swing_impact(sp->peak_dps); }

// One sin^2 segment of length d (s) turning by phi (rad), tau seconds in: the angle so far, the speed and the angular acceleration
static inline void swing_segment(double tau, double d, double phi, double *th, double *w, double *al) {
    double x = tau / d, s = sin(M_PI * x);
    *th = phi * (x - sin(2 * M_PI * x) / (2 * M_PI));
    *w = 2 * phi / d * s * s;
    *al = 2 * phi / d * (M_PI / d) * sin(2 * M_PI * x);
}
// The pose t seconds after the start. Returns 0 (and the pose at rest) outside the swing. acc / gyro / angle / dir as SixAxisSensorState's fields
static inline int swing_pose(const swing_spec *sp, double t, float acc[3], float gyro[3], float angle[3], float dir[9]) {
    memset(acc, 0, 3 * sizeof *acc); memset(gyro, 0, 3 * sizeof *gyro); memset(angle, 0, 3 * sizeof *angle); memset(dir, 0, 9 * sizeof *dir);
    acc[2] = -1; dir[0] = dir[4] = dir[8] = 1;
    if (sp->lift) {   // no turn: the gravity direction less the acceleration along the axis, a sine over LIFT_S
        if (!(t >= 0 && t < LIFT_S)) return 0;
        acc[sp->axis] = (float)fmax(-SWING_ACC_MAX_G, fmin(SWING_ACC_MAX_G, acc[sp->axis] - sp->sign * sp->peak_dps * sin(2 * M_PI * t / LIFT_S)));   // the IC saturates
        return 1;
    }
    double S = swing_seconds(sp->peak_dps), T = swing_duration(sp->peak_dps), big = SWING_DEG * M_PI / 180;
    if (!(t >= 0 && t < T)) return 0;
    double th, w, al;
    if (t < SWING_BACK_S) swing_segment(t, SWING_BACK_S, -big / 2, &th, &w, &al);
    else if (t < SWING_BACK_S + S) { swing_segment(t - SWING_BACK_S, S, big, &th, &w, &al); th -= big / 2; }
    else { swing_segment(t - SWING_BACK_S - S, SWING_BACK_S, -big / 2, &th, &w, &al); th += big / 2; }
    th *= sp->sign; w *= sp->sign; al *= sp->sign;
    // R = rotation by th about the body axis (controller -> world, columns = the controller's axes in the world)
    int a = sp->axis, b = (a + 1) % 3, c = (a + 2) % 3;
    double R[3][3] = {{0}}, cs = cos(th), sn = sin(th);
    R[a][a] = 1; R[b][b] = cs; R[c][c] = cs; R[c][b] = sn; R[b][c] = -sn;
    // body gravity R^T (0, 0, -1), less the controller's own acceleration at r = (0, L, 0) from the pivot: w x (w x r) + alpha x r, about the unit axis u
    double u[3] = {0}, r[3] = {0, SWING_ARM_M, 0}, lin[3];
    u[a] = 1;
    double ur = u[0] * r[0] + u[1] * r[1] + u[2] * r[2];
    double uxr[3] = {u[1] * r[2] - u[2] * r[1], u[2] * r[0] - u[0] * r[2], u[0] * r[1] - u[1] * r[0]};
    for (int i = 0; i < 3; i++) lin[i] = w * w * (ur * u[i] - r[i]) + al * uxr[i];
    for (int i = 0; i < 3; i++) {
        double g = -R[2][i] - lin[i] / SWING_GRAVITY;
        acc[i] = (float)fmax(-SWING_ACC_MAX_G, fmin(SWING_ACC_MAX_G, g));
        for (int j = 0; j < 3; j++) dir[3 * i + j] = (float)R[j][i];
    }
    gyro[a] = (float)(w / (2 * M_PI));
    angle[a] = (float)(th / (2 * M_PI));
    return 1;
}
