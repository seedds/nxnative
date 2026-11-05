// Joy-Con 0x30 IMU packets share the Pro Controller's layout and nominal scales.
// The right sensor's Y/Z axes are reversed (Nintendo protocol notes / hid-nintendo).
// Factory scale/offset formulas: dekuNukem/Nintendo_Switch_Reverse_Engineering/imu_sensor_notes.md.
#pragma once
#include "motion_pro.h"
typedef struct { float accel_gain[3], gyro_gain[3], gyro_offset[3]; int valid; } joy_calibration;
static inline int joy_i16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static inline int joy_calibration_parse(joy_calibration *out, const uint8_t *p, size_t len) {
    if (len != 24) return 0;
    joy_calibration c = {0};
    for (int i = 0; i < 3; i++) {
        int ao = joy_i16(p + 2*i), as = joy_i16(p + 6 + 2*i);
        int go = joy_i16(p + 12 + 2*i), gs = joy_i16(p + 18 + 2*i);
        if (as <= 0 || gs <= 0 || as <= ao || gs <= go) return 0;
        c.accel_gain[i] = 4.f / (as - ao);
        c.gyro_gain[i] = 936.f / (gs - go) * (float)M_PI / 180.f;
        c.gyro_offset[i] = go;
    }
    c.valid = 1; *out = c; return 1;
}
static inline int joy_motion_decode(pro_motion *s, int side, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3], const joy_calibration *cal) {
    if (side < 0 || side > 1) return 0;
    int n = pro_motion_decode(s, r, len, now, out);
    if (cal && cal->valid) for (int k = 0; k < n; k++) {
        const uint8_t *p = r + 13 + 12*k; float a[3], w[3];
        for (int i = 0; i < 3; i++) {
            a[i] = joy_i16(p + 2*i) * cal->accel_gain[i];
            w[i] = (joy_i16(p + 6 + 2*i) - cal->gyro_offset[i]) * cal->gyro_gain[i];
        }
        out[k].acc[0] = a[1]; out[k].acc[1] = -a[0]; out[k].acc[2] = -a[2];
        out[k].rot[0] = -w[1]; out[k].rot[1] = w[0]; out[k].rot[2] = w[2];
    }
    if (side == 1) for (int i = 0; i < n; i++) {
        out[i].acc[0] = -out[i].acc[0]; out[i].acc[2] = -out[i].acc[2];
        out[i].rot[0] = -out[i].rot[0]; out[i].rot[2] = -out[i].rot[2];
    }
    return n;
}
