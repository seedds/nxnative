// Controller motion (gyro + accelerometer) for the six-axis sensor (hle_hid.c). Pure C, no host state, so tests/t_motion.c checks it on any machine.
// Input: what the front end reads from GameController (GCMotion), in the controller's own frame as Apple reports it: acceleration in G (gravity direction, as CoreMotion:
// (0, 0, -1) lying face up), rotation rate in rad/s. Output: the Switch's SixAxisSensorState fields: acceleration in G, angular velocity in revolutions/s, angle in revolutions,
// direction = the controller's three axes in the world (rows). Switch frame: x right, y forward (away from the player), z up out of the face; flat at rest acc = (0, 0, -1).
// The frame map (DM_MOTION_AXES, default "x,y,z") is a signed permutation from Apple's axes to the Switch's, applied to acceleration and rotation alike (rotation times its
// determinant, so a mirror stays a proper rotation); DM_MOTION_ACC_SIGN=-1 flips only the acceleration, for a source that reports the specific force (+1 G up at rest).
// Orientation: the gyro is integrated (R = R exp(w dt)), and tilt drift is pulled back toward the measured gravity (Mahony, gain k) while |acc| is near 1 G; yaw is free to drift,
// as on the console without a recenter.
#pragma once
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct { int src[3]; float sign[3]; float acc_sign; float det; } motion_map;
typedef struct {
    float R[3][3];        // controller -> world (columns = the controller's axes in the world)
    float angle[3];       // integrated revolutions per axis
    float acc[3], gyro[3];// latest mapped sample: G, revolutions / s
    int primed;
} motion_state;

// "x,y,z" with optional '-' per entry, e.g. "x,-z,y". Returns 0 and the identity on a malformed string
static inline int motion_map_parse(motion_map *m, const char *s) {
    motion_map id = {{0, 1, 2}, {1, 1, 1}, 1, 1};
    *m = id;
    if (!s || !*s) return 1;
    motion_map r = id; int used = 0;
    for (int i = 0; i < 3; i++) {
        while (*s == ' ') s++;
        float sg = 1; if (*s == '-') { sg = -1; s++; } else if (*s == '+') s++;
        int a = *s == 'x' || *s == 'X' ? 0 : *s == 'y' || *s == 'Y' ? 1 : *s == 'z' || *s == 'Z' ? 2 : -1;
        if (a < 0 || used >> a & 1) return 0;
        used |= 1 << a; r.src[i] = a; r.sign[i] = sg; s++;
        while (*s == ' ') s++;
        if (i < 2) { if (*s != ',') return 0; s++; }
    }
    if (*s) return 0;
    // determinant of the signed permutation: sign product times the permutation's parity
    int inv = (r.src[0] > r.src[1]) + (r.src[0] > r.src[2]) + (r.src[1] > r.src[2]);
    r.det = r.sign[0] * r.sign[1] * r.sign[2] * (inv & 1 ? -1.f : 1.f);
    *m = r;
    return 1;
}
static inline void motion_map_apply(const motion_map *m, const float in[3], float out[3], float scale) {
    for (int i = 0; i < 3; i++) out[i] = m->sign[i] * in[m->src[i]] * scale;
}
static inline void motion_reset(motion_state *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 3; i++) s->R[i][i] = 1;
    s->acc[2] = -1;
}
static inline void m3_mul(float o[3][3], const float a[3][3], const float b[3][3]) {
    float t[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(o, t, sizeof t);
}
static inline void m3_orthonormalize(float R[3][3]) {   // Gram-Schmidt on the columns
    float c[3][3]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) c[j][i] = R[i][j];
    for (int k = 0; k < 3; k++) {
        for (int p = 0; p < k; p++) { float d = c[k][0] * c[p][0] + c[k][1] * c[p][1] + c[k][2] * c[p][2]; for (int i = 0; i < 3; i++) c[k][i] -= d * c[p][i]; }
        float n = sqrtf(c[k][0] * c[k][0] + c[k][1] * c[k][1] + c[k][2] * c[k][2]); if (n < 1e-6f) n = 1;
        for (int i = 0; i < 3; i++) c[k][i] /= n;
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = c[j][i];
}
// One sample: raw Apple acceleration (G) and rotation rate (rad/s), dt in seconds since the previous one (0 for the first). k = tilt correction gain (rad/s per unit error)
static inline void motion_update(motion_state *s, const motion_map *m, const float acc_raw[3], const float rot_raw[3], float dt, float k) {
    // A bad sensor sample must not prime or poison the persistent fusion state.
    for (int i = 0; i < 3; i++) if (!isfinite(acc_raw[i]) || !isfinite(rot_raw[i])) return;
    if (!isfinite(dt) || dt < 0) dt = 0;
    if (!isfinite(k) || k < 0) k = 0;
    float a[3], w[3];
    motion_map_apply(m, acc_raw, a, m->acc_sign);
    motion_map_apply(m, rot_raw, w, m->det);           // rad/s in the Switch frame
    if (!s->primed || dt <= 0 || dt > 0.25f) {         // first sample or a gap: no integration step
        if (!s->primed) {                              // start level with the measured gravity, facing forward
            float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (n > 0.5f && n < 1.5f) {
                // world down (0,0,-1) = R g_body  ->  R's third row = -g_body / |g|
                float z[3] = {-a[0] / n, -a[1] / n, -a[2] / n}, y0[3] = {0, 1, 0};
                // Upright phones have gravity along y: choose another heading reference before projecting it onto the level plane.
                if (fabsf(z[1]) > 0.99f) { y0[0] = 1; y0[1] = 0; }
                float d = y0[0] * z[0] + y0[1] * z[1] + y0[2] * z[2];
                float y[3] = {y0[0] - d * z[0], y0[1] - d * z[1], y0[2] - d * z[2]};
                float yn = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                if (yn > 1e-3f) {
                    for (int i = 0; i < 3; i++) y[i] /= yn;
                    float x[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
                    for (int j = 0; j < 3; j++) { s->R[0][j] = x[j]; s->R[1][j] = y[j]; s->R[2][j] = z[j]; }
                }
            }
            s->primed = 1;
        }
    } else {
        // tilt correction: predicted gravity in the body = R^T (0,0,-1) = -(third row of R)
        float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (k > 0 && n > 0.8f && n < 1.2f) {
            float gm[3] = {a[0] / n, a[1] / n, a[2] / n}, gp[3] = {-s->R[2][0], -s->R[2][1], -s->R[2][2]};
            float e[3] = {gm[1] * gp[2] - gm[2] * gp[1], gm[2] * gp[0] - gm[0] * gp[2], gm[0] * gp[1] - gm[1] * gp[0]};
            for (int i = 0; i < 3; i++) w[i] += k * e[i];
        }
        float th = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt;
        if (isfinite(th) && th > 1e-9f) {
            float u[3] = {w[0] * dt / th, w[1] * dt / th, w[2] * dt / th}, c = cosf(th), sn = sinf(th), v = 1 - c;
            float E[3][3] = {
                {c + u[0] * u[0] * v,        u[0] * u[1] * v - u[2] * sn, u[0] * u[2] * v + u[1] * sn},
                {u[1] * u[0] * v + u[2] * sn, c + u[1] * u[1] * v,        u[1] * u[2] * v - u[0] * sn},
                {u[2] * u[0] * v - u[1] * sn, u[2] * u[1] * v + u[0] * sn, c + u[2] * u[2] * v}};
            m3_mul(s->R, s->R, E);
            m3_orthonormalize(s->R);
        }
    }
    // reported angular velocity: the gyro itself (revolutions / s), without the tilt correction term
    float wraw[3]; motion_map_apply(m, rot_raw, wraw, m->det / (2 * (float)M_PI));
    for (int i = 0; i < 3; i++) {
        s->gyro[i] = wraw[i]; s->acc[i] = a[i];
        if (dt > 0 && dt <= 0.25f) { float angle = s->angle[i] + wraw[i] * dt; if (isfinite(angle)) s->angle[i] = angle; }
    }
}
// The SixAxisSensorState fields from the state: acc[3], gyro[3], angle[3], dir[9] (rows = the controller's x, y, z axes in the world)
static inline void motion_fill(const motion_state *s, float acc[3], float gyro[3], float angle[3], float dir[9]) {
    for (int i = 0; i < 3; i++) { acc[i] = s->acc[i]; gyro[i] = s->gyro[i]; angle[i] = s->angle[i]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) dir[3 * i + j] = s->R[j][i];
}
// The phone's own sensors (CoreMotion: x to the right of the screen in portrait, y to its top, z out of the screen) in the Switch frame for how the screen is turned, so a phone
// held like a Switch in handheld (or mounted in a controller) is the console: x to the right of the picture, y to its top, z toward the player. orient = UIInterfaceOrientation:
// 1 portrait, 2 upside down, 3 landscape right (home indicator right: the phone's top to the left), 4 landscape left (the phone's top to the right). Proper rotations only,
// so the same map serves acceleration and rotation rate
static inline void motion_phone_to_switch(int orient, const float in[3], float out[3]) {
    switch (orient) {
    case 3: out[0] = -in[1]; out[1] = in[0]; out[2] = in[2]; break;
    case 4: out[0] = in[1]; out[1] = -in[0]; out[2] = in[2]; break;
    case 2: out[0] = -in[0]; out[1] = -in[1]; out[2] = in[2]; break;
    default: out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; break;
    }
}
// Gyro rest offset of a controller the host receives raw rotation from (GameController: a DualSense at rest read 0.34, 0.20, -0.66 deg/s in Nintendo Switch Sports Resort's
// session): a window of about MOTION_REST_S of samples in which the gyro varies by under MOTION_REST_GYRO_SPAN rad/s and the acceleration by under MOTION_REST_ACC_SPAN G per
// axis is a controller lying or held still, and its mean rotation an offset candidate. A slow steady turn about gravity passes those spans too, so: a first offset needs two
// windows in a row that agree within MOTION_REST_AGREE and is at most MOTION_REST_FIRST_MAX (real offsets are under 1 deg/s); later ones must lie within MOTION_REST_AGREE of the
// offset in use and are averaged into it. motion_rest_apply takes the offset off, ramped in over MOTION_REST_RAMP_S from the one in use before, so a game's own bias estimate
// sees no step. The caller starts the window again on a gap in the samples (motion_rest_break) and forgets the offset when the stream goes stale or another controller takes
// the player (motion_rest_reset). As the Joy-Con 2 front end's s2_rest (switch2_proto.h), in the units dm_motion_set takes (G, rad/s), timed by the samples' own clock
#define MOTION_REST_S 1.0
#define MOTION_REST_GYRO_SPAN 0.03f       // rad/s (1.7 deg/s)
#define MOTION_REST_ACC_SPAN 0.02f        // G
#define MOTION_REST_FIRST_MAX 0.0349f     // rad/s (2 deg/s)
#define MOTION_REST_AGREE 0.0218f         // rad/s (1.25 deg/s)
#define MOTION_REST_RAMP_S 0.75
typedef struct { int have, pending; double t0, sum[3], t_set; unsigned n; float bias[3], cand[3], from[3], gmin[3], gmax[3], amin[3], amax[3]; } motion_rest;
static inline void motion_rest_break(motion_rest *r) { r->n = 0; r->pending = 0; }
static inline void motion_rest_reset(motion_rest *r) { memset(r, 0, sizeof *r); }
static inline int motion_rest_near(const float a[3], const float b[3]) { for (int i = 0; i < 3; i++) if (fabsf(a[i] - b[i]) > MOTION_REST_AGREE) return 0; return 1; }
// The offset being taken off at time t: ramped from the last one to the newest over MOTION_REST_RAMP_S
static inline void motion_rest_now(const motion_rest *r, double t, float out[3]) {
    double f = r->have ? (t - r->t_set) / MOTION_REST_RAMP_S : 0; if (f < 0) f = 0; if (f > 1) f = 1;
    for (int i = 0; i < 3; i++) out[i] = r->have ? (float)(r->from[i] + (r->bias[i] - r->from[i]) * f) : 0;
}
static inline void motion_rest_set(motion_rest *r, const float m[3], double t) {
    float cur[3]; motion_rest_now(r, t, cur);
    for (int i = 0; i < 3; i++) { r->from[i] = cur[i]; r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i]; }
    r->have = 1; r->t_set = t;
}
static inline void motion_rest_update(motion_rest *r, const float acc[3], const float rot[3], double t) {
    for (int i = 0; i < 3; i++) if (!isfinite(acc[i]) || !isfinite(rot[i])) { motion_rest_break(r); return; }
    if (!r->n) { r->t0 = t; for (int i = 0; i < 3; i++) { r->sum[i] = 0; r->gmin[i] = r->gmax[i] = rot[i]; r->amin[i] = r->amax[i] = acc[i]; } }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += rot[i];
        if (rot[i] < r->gmin[i]) r->gmin[i] = rot[i]; if (rot[i] > r->gmax[i]) r->gmax[i] = rot[i];
        if (acc[i] < r->amin[i]) r->amin[i] = acc[i]; if (acc[i] > r->amax[i]) r->amax[i] = acc[i];
        if (r->gmax[i] - r->gmin[i] > MOTION_REST_GYRO_SPAN || r->amax[i] - r->amin[i] > MOTION_REST_ACC_SPAN) { motion_rest_break(r); return; }   // moving: start again
    }
    r->n++;
    if (t - r->t0 < MOTION_REST_S || r->n < 10) return;
    float m[3]; for (int i = 0; i < 3; i++) m[i] = (float)(r->sum[i] / r->n);
    r->n = 0;
    if (r->have) { if (motion_rest_near(m, r->bias)) motion_rest_set(r, m, t); return; }   // a later window: only one that agrees with the offset in use
    for (int i = 0; i < 3; i++) if (fabsf(m[i]) > MOTION_REST_FIRST_MAX) { r->pending = 0; return; }
    if (r->pending && motion_rest_near(m, r->cand)) { motion_rest_set(r, m, t); r->pending = 0; return; }   // the second window agreeing: the first offset
    memcpy(r->cand, m, sizeof r->cand); r->pending = 1;   // the window right after this one must agree (a pending one is dropped by any break)
}
static inline void motion_rest_apply(const motion_rest *r, const float rot[3], double t, float out[3]) { float b[3]; motion_rest_now(r, t, b); for (int i = 0; i < 3; i++) out[i] = rot[i] - b[i]; }

// Playout delay of a live stream (motion_lag): a source sends a sample every 15 to 30 ms while the game reads 5 ms ticks up to the present, so read at the present most ticks lie
// past the newest sample and can only hold it. Played back a little late, every tick lies between two samples that have arrived. The delay a stream needs is how long after a
// sample's time the next one arrives (arrival_{k+1} - t_k: the sample spacing plus the next one's transport lateness); the stream's delay is the 90th percentile of the last
// MOTION_LAG_N of them, capped at MOTION_LAG_MAX_NS so a stream that stalls (a lost Joy-Con 2 report: 60 or 90 ms) does not add its stall to every tick
#define MOTION_LAG_N 32
#define MOTION_LAG_PCT 90
#define MOTION_LAG_MAX_NS 40000000ull
#define MOTION_LAG_CLIP_NS 1000000000ull   // a need past a second is a silence, not a spacing (the stream starts over anyway)
typedef struct { uint32_t need_us[MOTION_LAG_N]; unsigned n, next; uint64_t delay_ns; } motion_lag;
static inline void motion_lag_reset(motion_lag *l) { memset(l, 0, sizeof *l); }
static inline void motion_lag_add(motion_lag *l, uint64_t need_ns) {
    if (need_ns > MOTION_LAG_CLIP_NS) need_ns = MOTION_LAG_CLIP_NS;
    l->need_us[l->next] = (uint32_t)(need_ns / 1000); l->next = (l->next + 1) % MOTION_LAG_N; if (l->n < MOTION_LAG_N) l->n++;
    uint32_t s[MOTION_LAG_N]; memcpy(s, l->need_us, sizeof s);   // the window, sorted (insertion: 32 values at most, once a sample)
    for (unsigned i = 1; i < l->n; i++) { uint32_t v = s[i]; unsigned j = i; while (j && s[j - 1] > v) { s[j] = s[j - 1]; j--; } s[j] = v; }
    unsigned k = l->n * MOTION_LAG_PCT / 100; if (k >= l->n) k = l->n - 1;
    uint64_t d = (uint64_t)s[k] * 1000; l->delay_ns = d < MOTION_LAG_MAX_NS ? d : MOTION_LAG_MAX_NS;
}
