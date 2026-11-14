// Joy-Con (Switch 1) report 0x30, standard full mode: 49 bytes, buttons at 3..5, sticks at 6..11, then three
// 12-byte IMU samples at 13 (accel XYZ then gyro XYZ, int16 LE, 5 ms apart, the third the newest). macOS keeps
// this report flowing at 60 Hz over Bluetooth Classic but zeroes the IMU until the host sends EnableIMU (0x40),
// so a passive reader sees a live stream of nothing: the transport in pad_jc1_mac.h sends the subcommands.
//
// The chip's axes (dekuNukem's notes, the two 400x400 axis diagrams): +X runs along the controller toward its
// top, +Y is the short axis, and +Z leaves the face on the left Joy-Con but the back on the right one. The
// host's Switch frame for a Joy-Con is x the short axis (right), y the long axis (toward the top), z out of the
// face, so the map is (-Y, +X, +Z) for the left and (+Y, +X, -Z) for the right; it is a rotation, so the gyro
// takes it too. The host wants the gravity direction, not the accelerometer's specific force: hence the negation.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

enum {
    JC1_REPORT_LEN = 49,
    JC1_IMU_PERIOD_NS = 5000000,
    JC1_IMU_SAMPLES = 3,
    JC1_SUB_DEVICE_INFO = 0x02,
    JC1_SUB_INPUT_MODE = 0x03,
    JC1_SUB_SPI_READ = 0x10,
    JC1_SUB_ENABLE_IMU = 0x40,
    JC1_INPUT_MODE_FULL = 0x30,
    JC1_SPI_IMU_CAL = 0x6020,
    JC1_IMU_CAL_LEN = 0x18,
};
// Carried by every output report 0x01 so the actuators stay quiet while the host talks to the controller.
static const uint8_t jc1_neutral_rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

// Output report 0x01: id, rolling packet counter, 8 rumble bytes, the subcommand, then its argument (38 bytes at most).
static inline void jc1_output_report(uint8_t out[JC1_REPORT_LEN], uint8_t counter, uint8_t sub, const uint8_t *arg, size_t n) {
    memset(out, 0, JC1_REPORT_LEN);
    out[0] = 0x01;
    out[1] = (uint8_t)(counter & 0x0f);
    memcpy(out + 2, jc1_neutral_rumble, sizeof jc1_neutral_rumble);
    out[10] = sub;
    if (arg && n) memcpy(out + 11, arg, n > 38 ? 38 : n);
}

// SPI 0x6020 is the IMU's factory calibration: accel origin, accel coeff, gyro origin, gyro coeff, three int16
// each (little endian). The reverse-engineering notes' scales are 4 / (coeff - origin) G per count and
// 816 / (coeff - origin) deg/s per count (±2000 dps) or 936 / (coeff - origin) (the saturation-free LSM6DS3 value);
// the gyro origin is the still offset, not a scale. Units in the wild store the coefficient in either scheme:
// one of a pair here had 0x3BE7 (15335) against the other's default 0x343B (13371), exactly 936/816 apart, and
// with one constant the two then integrate at rates 13% apart and a glove's orientation drifts during a round.
// So each axis takes whichever constant puts it nearest the part's measured sensitivity (0.0607 deg/s per count,
// the same LSM6DS3 family as the Joy-Con 2), which keeps a real per-unit calibration and drops the scheme.
typedef struct { float accel_scale[3], gyro_scale[3], gyro_offset[3]; int have; } jc1_cal;
static inline int16_t jc1_s16(const uint8_t *b) { return (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8); }
static inline void jc1_cal_default(jc1_cal *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 3; i++) { c->accel_scale[i] = 1.f / 4096.f; c->gyro_scale[i] = 0.061f; }
}
// A 0x21 subcommand reply, of which the SPI read answers 0x90 0x10 <address LE> <length> <data>. 1: the IMU block, parsed.
static inline int jc1_parse_cal(jc1_cal *c, const uint8_t *r, size_t len) {
    if (len < 20 + JC1_IMU_CAL_LEN || r[0] != 0x21 || r[13] != 0x90 || r[14] != JC1_SUB_SPI_READ) return 0;
    uint32_t address = (uint32_t)r[15] | (uint32_t)r[16] << 8 | (uint32_t)r[17] << 16 | (uint32_t)r[18] << 24;
    if (address != JC1_SPI_IMU_CAL || r[19] < JC1_IMU_CAL_LEN) return 0;
    const uint8_t *d = r + 20;
    for (int i = 0; i < 3; i++) {
        float accel_origin = (float)jc1_s16(d + 2 * i), accel_coeff = (float)jc1_s16(d + 6 + 2 * i);
        float gyro_origin = (float)jc1_s16(d + 12 + 2 * i), gyro_coeff = (float)jc1_s16(d + 18 + 2 * i);
        if (fabsf(accel_coeff - accel_origin) > 1.f) c->accel_scale[i] = 4.f / (accel_coeff - accel_origin);
        float denom = gyro_coeff - gyro_origin;
        if (fabsf(denom) > 1.f) {
            float accurate = 816.f / denom, saturation_free = 936.f / denom;
            c->gyro_scale[i] = fabsf(accurate - 0.061f) <= fabsf(saturation_free - 0.061f) ? accurate : saturation_free;
        }
        c->gyro_offset[i] = gyro_origin;
    }
    c->have = 1;
    return 1;
}

// One controller's live state: a repeated report counter is the same three samples again, and macOS's zeroed
// IMU (before EnableIMU) is not a stream of a controller in free fall. clock is the newest sample's time, advanced
// 15 ms a report and slewed toward the arrival: stamped from the arrival alone, a report that comes early puts its
// three samples before the last one's, and the host drops those (t <= last) - a stall and a jump in fast play.
typedef struct { int live, seen; uint8_t counter; uint64_t received, clock, emitted; } jc1_stream;
typedef struct { float acc[3], rot[3]; uint64_t ns; } jc1_sample;
enum { JC1_STALE_NS = 300000000 };

// The gyro's still offset. The factory's origin is close but not the unit's own: one here read 5/25/2 counts against a rest offset of tens of counts, and what is left over (a
// degree or two per second) integrates into a drift the game reads as the player's hands moving. A second of samples in which the gyro and the accelerometer hardly vary is a
// controller lying or held still; their mean is the offset (blended with the one before). An offset past 3.7 degrees per second (60 counts) is not taken: that is a slow turn,
// not rest. The Switch 2 transport learns the same thing for its Joy-Con 2 (s2_rest_update).
#define JC1_REST_N 200   // about a second of samples, 5 ms apart
typedef struct { int n, have; int32_t sum[3]; int16_t gmin[3], gmax[3], amin[3], amax[3]; float bias[3]; } jc1_rest;
static inline void jc1_rest_update(jc1_rest *r, const int16_t res[6]) {
    if (!r->n) for (int i = 0; i < 3; i++) { r->sum[i] = 0; r->gmin[i] = r->gmax[i] = res[3 + i]; r->amin[i] = r->amax[i] = res[i]; }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += res[3 + i];
        if (res[3 + i] < r->gmin[i]) r->gmin[i] = res[3 + i]; if (res[3 + i] > r->gmax[i]) r->gmax[i] = res[3 + i];
        if (res[i] < r->amin[i]) r->amin[i] = res[i]; if (res[i] > r->amax[i]) r->amax[i] = res[i];
        if (r->gmax[i] - r->gmin[i] > 100 || r->amax[i] - r->amin[i] > 300) { r->n = 0; return; }   // moving: the window starts again
    }
    if (++r->n < JC1_REST_N) return;
    r->n = 0;
    float m[3]; for (int i = 0; i < 3; i++) { m[i] = (float)r->sum[i] / JC1_REST_N; if (fabsf(m[i]) > 60.f) return; }
    for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
    r->have = 1;
}
// The offset the samples are taken against: the factory's origin plus what the controller's own rest shows.
static inline void jc1_rest_apply(jc1_cal *c, const jc1_rest *r, const int16_t origin[3]) {
    for (int i = 0; i < 3; i++) c->gyro_offset[i] = (float)origin[i] + (r->have ? r->bias[i] : 0.f);
}

// Decodes three samples; 0 for a short or repeated report, or for the zeroed IMU of a controller whose sensor
// is off (until it has been on once: later zeros are a real fall). acc is the gravity direction in G and rot
// radians/s, in the host's Switch frame; ns is each sample's own time, the third block the newest.
static inline int jc1_decode(jc1_stream *s, const uint8_t *r, size_t len, int left, const jc1_cal *cal, uint64_t now, jc1_sample out[JC1_IMU_SAMPLES]) {
    if (len < JC1_REPORT_LEN || r[0] != 0x30) return 0;
    if (now < 2 * (uint64_t)JC1_IMU_PERIOD_NS) return 0;
    if (s->seen && (now < s->received || now - s->received > JC1_STALE_NS)) *s = (jc1_stream){0};
    if (s->seen && s->counter == r[1]) return 0;
    int data = 0;
    for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) data |= r[13 + 12 * k + 2 * i] || r[14 + 12 * k + 2 * i];
    if (!s->live && !data) return 0;
    s->seen = 1; s->counter = r[1]; s->received = now; s->live = 1;
    int64_t drift = (int64_t)now - (int64_t)s->clock;
    if (!s->clock || drift > 100000000 || drift < -100000000) s->clock = now;   // first report, or after a stall
    else s->clock = (uint64_t)((int64_t)s->clock + drift / 4);                   // 15 ms a report, pulled a quarter toward the arrival
    // The three samples end at the clock, not later than the report arrived, and none repeats one already given: an early
    // report (a burst after a hiccup) leaves fewer than three, where stamping from the arrival gave samples the host drops.
    uint64_t newest = s->clock > now ? now : s->clock;
    s->clock += (uint64_t)JC1_IMU_SAMPLES * JC1_IMU_PERIOD_NS;
    const float deg = (float)M_PI / 180.f;
    int n = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t t = newest - (uint64_t)(2 - k) * JC1_IMU_PERIOD_NS;
        if (s->emitted && t <= s->emitted) continue;
        const uint8_t *b = r + 13 + 12 * k;
        float ax = jc1_s16(b) * cal->accel_scale[0], ay = jc1_s16(b + 2) * cal->accel_scale[1], az = jc1_s16(b + 4) * cal->accel_scale[2];
        float gx = (jc1_s16(b + 6) - cal->gyro_offset[0]) * cal->gyro_scale[0];
        float gy = (jc1_s16(b + 8) - cal->gyro_offset[1]) * cal->gyro_scale[1];
        float gz = (jc1_s16(b + 10) - cal->gyro_offset[2]) * cal->gyro_scale[2];
        out[n].acc[0] = -(left ? -ay : ay);   // gravity, not specific force
        out[n].acc[1] = -ax;
        out[n].acc[2] = -(left ? az : -az);
        out[n].rot[0] = (left ? -gy : gy) * deg;
        out[n].rot[1] = gx * deg;
        out[n].rot[2] = (left ? gz : -gz) * deg;
        out[n].ns = t;
        s->emitted = t;
        n++;
    }
    return n;
}
