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
    int home;
    int battery;   // 0..4 from the upper nibble of byte 2 (4=full, 3=75%, 2=50%, 1=25%, 0=empty)
    int charging;  // 1 when bit 4 of byte 2 is set
} joycon_state;

static inline int joycon_is_pid(long vid, long pid) {
    return vid == JOYCON_VID && (pid == JOYCON_PID_L || pid == JOYCON_PID_R);
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

    uint64_t b = 0;
    int rx = 0, ry = 0;
    if (s->pid == JOYCON_PID_L) {
        // Left Joy-Con held sideways (rotated 90 degrees counter-clockwise, rail on top):
        // Down->A (right), Left->B (bottom), Right->X (top), Up->Y (left), SL->L, SR->R
        uint8_t lb = r[5], sh = r[4];
        if (lb & 0x01) b |= DM_BTN_A;
        if (lb & 0x08) b |= DM_BTN_B;
        if (lb & 0x04) b |= DM_BTN_X;
        if (lb & 0x02) b |= DM_BTN_Y;
        if (lb & 0x20) b |= DM_BTN_L;
        if (lb & 0x10) b |= DM_BTN_R;
        if (lb & 0x40) b |= DM_BTN_ZL;
        if (lb & 0x80) b |= DM_BTN_ZR;
        if (sh & 0x01) b |= DM_BTN_PLUS;
        if (sh & 0x20) b |= DM_BTN_MINUS;
        if (sh & 0x08) b |= DM_BTN_LS;
        s->home = 0;
        rx = (int)r[6] | (((int)(r[7] & 0x0f)) << 8);
        ry = ((int)(r[7] >> 4)) | (((int)r[8]) << 4);
    } else {
        // Right Joy-Con held sideways (rotated 90 degrees clockwise, rail on top):
        // X->A (right), A->B (bottom), Y->X (top), B->Y (left), SL->L, SR->R
        uint8_t rb = r[3], sh = r[4];
        if (rb & 0x02) b |= DM_BTN_A;
        if (rb & 0x08) b |= DM_BTN_B;
        if (rb & 0x01) b |= DM_BTN_X;
        if (rb & 0x04) b |= DM_BTN_Y;
        if (rb & 0x20) b |= DM_BTN_L;
        if (rb & 0x10) b |= DM_BTN_R;
        if (rb & 0x40) b |= DM_BTN_ZL;
        if (rb & 0x80) b |= DM_BTN_ZR;
        if (sh & 0x02) b |= DM_BTN_PLUS;
        if (sh & 0x04) b |= DM_BTN_LS;
        s->home = (sh & 0x10) != 0;
        rx = (int)r[9] | (((int)(r[10] & 0x0f)) << 8);
        ry = ((int)(r[10] >> 4)) | (((int)r[11]) << 4);
    }
    s->buttons = b;

    if (rx == 0 && ry == 0) {
        s->sx = 0.0f;
        s->sy = 0.0f;
        return 1;
    }
    if (!s->calibrated && rx >= 1650 && rx <= 2450 && ry >= 1650 && ry <= 2450) {
        s->cx = rx;
        s->cy = ry;
        s->calibrated = 1;
    }
    float dx = (float)(rx - s->cx) / JOYCON_STICK_RADIUS;
    float dy = (float)(ry - s->cy) / JOYCON_STICK_RADIUS;
    float rot_x = (s->pid == JOYCON_PID_L) ? -dy : dy;
    float rot_y = (s->pid == JOYCON_PID_L) ? dx : -dx;
    joycon_stick_scale(rot_x, rot_y, &s->sx, &s->sy);
    return 1;
}

static inline int joycon_motion_decode(pro_motion *s, uint16_t pid, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (pid != JOYCON_PID_L && pid != JOYCON_PID_R) return 0;
    int n = pro_motion_decode(s, r, len, now, out);
    for (int i = 0; i < n; i++) {
        float ax = out[i].acc[0], ay = out[i].acc[1], az = out[i].acc[2];
        float gx = out[i].rot[0], gy = out[i].rot[1], gz = out[i].rot[2];
        if (pid == JOYCON_PID_L) {
            // Left Joy-Con rotated 90 degrees counter-clockwise (rail on top)
            out[i].acc[0] = -ay; out[i].acc[1] = ax; out[i].acc[2] = az;
            out[i].rot[0] = -gy; out[i].rot[1] = gx; out[i].rot[2] = gz;
        } else {
            // Right Joy-Con IMU sensor is mounted flipped 180 degrees around X (raw Y and Z inverted
            // relative to Left/Pro, see Linux hid-nintendo.c), then rotated 90 degrees clockwise
            out[i].acc[0] = ay; out[i].acc[1] = ax; out[i].acc[2] = -az;
            out[i].rot[0] = gy; out[i].rot[1] = gx; out[i].rot[2] = -gz;
        }
    }
    return n;
}
