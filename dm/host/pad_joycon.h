#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dm_input.h"
#include "motion_pro.h"

// Nintendo Switch Joy-Con sideways decoder (DM_JOYCON_SPLIT=1, LNPANDY/NX2Apple#740):
// macOS switches Bluetooth Joy-Con (L) (0x057e:0x2006) and Joy-Con (R) (0x057e:0x2007) to standard
// 0x30 49-byte input reports at 60 Hz, while GCController merges the pair into one controller and
// drops the rail SL and SR buttons. Each half here becomes one horizontal sideways controller with
// its stick rotated 90 degrees and its rail SL and SR mapped to L and R.

#define JOYCON_VID             0x057e
#define JOYCON_PID_L           0x2006
#define JOYCON_PID_R           0x2007
#define JOYCON_REPORT_0x30     0x30
#define JOYCON_REPORT_MIN_LEN  12
#define JOYCON_STICK_CENTER    2048
#define JOYCON_STICK_RADIUS    1400.0f
#define JOYCON_STICK_DZ        0.12f

typedef struct {
    uint16_t pid;
    int calibrated;
    int cx, cy;
    uint64_t buttons;
    float sx, sy;
    uint64_t joy_buttons;   // upright Joy-Con (L) / (R) npad buttons (dm_joy_set, DM_BTN_LSL..RSR)
    float joy_sx, joy_sy;   // unrotated stick axes in [-1, 1] for dm_joy_set
    int home;
    int battery;   // 0..4 from the upper nibble of byte 2 (4=full, 3=75%, 2=50%, 1=25%, 0=empty)
    int charging;  // 1 when bit 4 of byte 2 is set
} joycon_state;

static inline int joycon_is_pid(long vid, long pid) {
    return vid == JOYCON_VID && (pid == JOYCON_PID_L || pid == JOYCON_PID_R);
}

static inline int joycon_side(uint16_t pid) {
    return pid == JOYCON_PID_R ? 1 : 0;
}

static inline void joycon_init(joycon_state *s, uint16_t pid) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->pid = pid;
    s->cx = JOYCON_STICK_CENTER;
    s->cy = JOYCON_STICK_CENTER;
}

static inline void joycon_stick_scale(float x, float y, float *ox, float *oy) {
    float m = sqrtf(x * x + y * y);
    if (m <= JOYCON_STICK_DZ) { *ox = 0.0f; *oy = 0.0f; return; }
    float k = (m - JOYCON_STICK_DZ) / ((1.0f - JOYCON_STICK_DZ) * m);
    float sx = x * k, sy = y * k;
    *ox = sx < -1.0f ? -1.0f : (sx > 1.0f ? 1.0f : sx);
    *oy = sy < -1.0f ? -1.0f : (sy > 1.0f ? 1.0f : sy);
}

static inline int joycon_decode_0x30(joycon_state *s, const uint8_t *r, size_t n) {
    if (!s || !r || n < JOYCON_REPORT_MIN_LEN || r[0] != JOYCON_REPORT_0x30) return 0;
    if (s->pid != JOYCON_PID_L && s->pid != JOYCON_PID_R) return 0;

    s->charging = (r[2] >> 4) & 1;
    s->battery = (r[2] >> 5) & 7;

    uint64_t b = 0, jb = 0;
    int rx = 0, ry = 0;
    if (s->pid == JOYCON_PID_L) {
        // Left Joy-Con held sideways (rotated 90 degrees counter-clockwise, rail on top):
        // Down->A (right), Left->B (bottom), Right->X (top), Up->Y (left), SL->L, SR->R
        uint8_t lb = r[5], sh = r[4];
        if (lb & 0x01) { b |= DM_BTN_A; jb |= DM_BTN_DOWN; }
        if (lb & 0x08) { b |= DM_BTN_B; jb |= DM_BTN_LEFT; }
        if (lb & 0x04) { b |= DM_BTN_X; jb |= DM_BTN_RIGHT; }
        if (lb & 0x02) { b |= DM_BTN_Y; jb |= DM_BTN_UP; }
        if (lb & 0x20) { b |= DM_BTN_L; jb |= DM_BTN_LSL; }
        if (lb & 0x10) { b |= DM_BTN_R; jb |= DM_BTN_LSR; }
        if (lb & 0x40) { b |= DM_BTN_ZL; jb |= DM_BTN_L; }
        if (lb & 0x80) { b |= DM_BTN_ZR; jb |= DM_BTN_ZL; }
        if (sh & 0x01) { b |= DM_BTN_PLUS; jb |= DM_BTN_MINUS; }
        if (sh & 0x20) b |= DM_BTN_MINUS;
        if (sh & 0x08) { b |= DM_BTN_LS; jb |= DM_BTN_LS; }
        s->home = 0;
        rx = (int)r[6] | (((int)(r[7] & 0x0f)) << 8);
        ry = ((int)(r[7] >> 4)) | (((int)r[8]) << 4);
    } else {
        // Right Joy-Con held sideways (rotated 90 degrees clockwise, rail on top):
        // X->A (right), A->B (bottom), Y->X (top), B->Y (left), SL->L, SR->R
        uint8_t rb = r[3], sh = r[4];
        if (rb & 0x02) { b |= DM_BTN_A; jb |= DM_BTN_X; }
        if (rb & 0x08) { b |= DM_BTN_B; jb |= DM_BTN_A; }
        if (rb & 0x01) { b |= DM_BTN_X; jb |= DM_BTN_Y; }
        if (rb & 0x04) { b |= DM_BTN_Y; jb |= DM_BTN_B; }
        if (rb & 0x20) { b |= DM_BTN_L; jb |= DM_BTN_RSL; }
        if (rb & 0x10) { b |= DM_BTN_R; jb |= DM_BTN_RSR; }
        if (rb & 0x40) { b |= DM_BTN_ZL; jb |= DM_BTN_R; }
        if (rb & 0x80) { b |= DM_BTN_ZR; jb |= DM_BTN_ZR; }
        if (sh & 0x02) { b |= DM_BTN_PLUS; jb |= DM_BTN_PLUS; }
        if (sh & 0x04) { b |= DM_BTN_LS; jb |= DM_BTN_RS; }
        s->home = (sh & 0x10) != 0;
        rx = (int)r[9] | (((int)(r[10] & 0x0f)) << 8);
        ry = ((int)(r[10] >> 4)) | (((int)r[11]) << 4);
    }
    s->buttons = b;
    s->joy_buttons = jb;

    if (rx == 0 && ry == 0) {
        s->sx = 0.0f;
        s->sy = 0.0f;
        s->joy_sx = 0.0f;
        s->joy_sy = 0.0f;
        return 1;
    }
    if (!s->calibrated && rx >= 1650 && rx <= 2450 && ry >= 1650 && ry <= 2450) {
        s->cx = rx;
        s->cy = ry;
        s->calibrated = 1;
    }
    float dx = (float)(rx - s->cx) / JOYCON_STICK_RADIUS;
    float dy = (float)(ry - s->cy) / JOYCON_STICK_RADIUS;
    joycon_stick_scale(dx, dy, &s->joy_sx, &s->joy_sy);
    float rot_x = (s->pid == JOYCON_PID_L) ? -dy : dy;
    float rot_y = (s->pid == JOYCON_PID_L) ? dx : -dx;
    joycon_stick_scale(rot_x, rot_y, &s->sx, &s->sy);
    return 1;
}

static inline int joycon_motion_decode(pro_motion *s, uint16_t pid, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (pid != JOYCON_PID_L && pid != JOYCON_PID_R) return 0;
    int n = pro_motion_decode(s, r, len, now, out);
    for (int i = 0; i < n; i++) {
        // pro_motion_decode outputs upright coordinates for Left/Pro (x right, y forward, z up, flat acc = 0,0,-1).
        // Right Joy-Con's IMU sensor is mounted flipped 180 degrees around raw X (raw Y and Z inverted -> upright -ax, ay, -az).
        float uax = (pid == JOYCON_PID_L) ? out[i].acc[0] : -out[i].acc[0];
        float uay = out[i].acc[1];
        float uaz = (pid == JOYCON_PID_L) ? out[i].acc[2] : -out[i].acc[2];
        float ugx = (pid == JOYCON_PID_L) ? out[i].rot[0] : -out[i].rot[0];
        float ugy = out[i].rot[1];
        float ugz = (pid == JOYCON_PID_L) ? out[i].rot[2] : -out[i].rot[2];
        if (pid == JOYCON_PID_L) {
            // Left Joy-Con held sideways (top to the player's right, matching s2_map_left_alone)
            out[i].acc[0] = uay; out[i].acc[1] = -uax; out[i].acc[2] = uaz;
            out[i].rot[0] = ugy; out[i].rot[1] = -ugx; out[i].rot[2] = ugz;
        } else {
            // Right Joy-Con held sideways (top to the player's left, matching s2_map_right_alone)
            out[i].acc[0] = -uay; out[i].acc[1] = uax; out[i].acc[2] = uaz;
            out[i].rot[0] = -ugy; out[i].rot[1] = ugx; out[i].rot[2] = ugz;
        }
    }
    return n;
}

// Convert a decoded sideways Joy-Con IMU sample back to upright Joy-Con coordinates (for dm_motion_set_joy),
// preserving the Right Joy-Con sensor's 180-degree X-axis mounting flip correction.
static inline void joycon_motion_upright(uint16_t pid, const pro_motion_sample *sideways, float acc[3], float rot[3]) {
    if (!sideways) return;
    if (pid == JOYCON_PID_L) {
        acc[0] = -sideways->acc[1]; acc[1] = sideways->acc[0]; acc[2] = sideways->acc[2];
        rot[0] = -sideways->rot[1]; rot[1] = sideways->rot[0]; rot[2] = sideways->rot[2];
    } else {
        acc[0] = sideways->acc[1]; acc[1] = -sideways->acc[0]; acc[2] = sideways->acc[2];
        rot[0] = sideways->rot[1]; rot[1] = -sideways->rot[0]; rot[2] = sideways->rot[2];
    }
}

// Gyro rest offset (~1 s of still 200 Hz samples, matching s2_rest_update in switch2_proto.h):
// compensates uncalibrated zero-rate drift on Bluetooth Joy-Con 0x30 reports without SPI commands.
#define JOYCON_REST_N 180
typedef struct {
    int n, have;
    float sum[3], gmin[3], gmax[3], amin[3], amax[3], bias[3];
} joycon_rest;

static inline void joycon_rest_apply(joycon_rest *r, pro_motion_sample *s) {
    if (!r || !s) return;
    if (!r->n) {
        for (int i = 0; i < 3; i++) {
            r->sum[i] = 0.0f;
            r->gmin[i] = r->gmax[i] = s->rot[i];
            r->amin[i] = r->amax[i] = s->acc[i];
        }
    }
    int moving = 0;
    for (int i = 0; i < 3; i++) {
        r->sum[i] += s->rot[i];
        if (s->rot[i] < r->gmin[i]) r->gmin[i] = s->rot[i];
        if (s->rot[i] > r->gmax[i]) r->gmax[i] = s->rot[i];
        if (s->acc[i] < r->amin[i]) r->amin[i] = s->acc[i];
        if (s->acc[i] > r->amax[i]) r->amax[i] = s->acc[i];
        if (r->gmax[i] - r->gmin[i] > 0.12f || r->amax[i] - r->amin[i] > 0.08f) moving = 1;
    }
    if (moving) {
        r->n = 0;
    } else if (++r->n >= JOYCON_REST_N) {
        r->n = 0;
        float m[3];
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            m[i] = r->sum[i] / JOYCON_REST_N;
            if (fabsf(m[i]) > 0.08f) ok = 0;
        }
        if (ok) {
            for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
            r->have = 1;
        }
    }
    if (r->have) {
        for (int i = 0; i < 3; i++) s->rot[i] -= r->bias[i];
    }
}

// Choosing on the controllers, as on a Switch's Change Grip/Order screen (held for 3 s so play cannot trigger it by accident):
// SL + SR held on a Joy-Con in a pair splits it into its own sideways player; L/ZL on a left Joy-Con alone and R/ZR on a right one alone, held together, pair them.
#define JOYCON_GESTURE_NS 3000000000ull
enum { JOYCON_GESTURE_NONE, JOYCON_GESTURE_SPLIT, JOYCON_GESTURE_JOIN };
typedef struct { int kind, done; uint64_t since; } joycon_hold;

static inline int joycon_gesture(const joycon_state *s, int paired) {
    if (!s) return JOYCON_GESTURE_NONE;
    uint64_t b = s->joy_buttons;
    uint64_t rails = s->pid == JOYCON_PID_R ? (DM_BTN_RSL | DM_BTN_RSR) : (DM_BTN_LSL | DM_BTN_LSR);
    if (paired) return (b & rails) == rails ? JOYCON_GESTURE_SPLIT : JOYCON_GESTURE_NONE;
    uint64_t sh = s->pid == JOYCON_PID_R ? (DM_BTN_R | DM_BTN_ZR) : (DM_BTN_L | DM_BTN_ZL);
    return (b & sh) && !(b & rails) ? JOYCON_GESTURE_JOIN : JOYCON_GESTURE_NONE;
}

static inline int joycon_hold_update(joycon_hold *h, int kind, uint64_t now) {
    if (!h) return 0;
    if (kind != h->kind) { h->kind = kind; h->since = now; h->done = 0; }
    return kind != JOYCON_GESTURE_NONE && now >= h->since && now - h->since >= JOYCON_GESTURE_NS;
}
