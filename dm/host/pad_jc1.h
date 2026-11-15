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
typedef struct { float accel_scale[3], gyro_scale[3], gyro_origin[3]; int have; } jc1_cal;
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
        c->gyro_origin[i] = gyro_origin;
    }
    c->have = 1;
    return 1;
}

// The gyro's still offset. The factory's origin is close but not the unit's own: one here read 5/25/2 counts against a rest offset of tens of counts, and what is left over (a
// degree or two per second) integrates into a drift the game reads as the player's hands moving. A second of samples in which the gyro and the accelerometer hardly vary is a
// controller lying or held still; their mean, against the factory's origin, is the rest of the offset (blended with the one before). A mean past 3.7 degrees per second (60
// counts) is not taken: that is a slow turn, not rest. The Switch 2 transport learns the same thing for its Joy-Con 2 (s2_rest_update).
#define JC1_REST_N 200   // about a second of samples, 5 ms apart
typedef struct { int n, have; int32_t sum[3]; int16_t gmin[3], gmax[3], amin[3], amax[3]; float bias[3]; } jc1_rest;
static inline void jc1_rest_update(jc1_rest *r, const int16_t res[6]) {   // res: accel counts, then gyro counts against the factory's origin
    if (!r->n) for (int i = 0; i < 3; i++) { r->sum[i] = 0; r->gmin[i] = r->gmax[i] = res[3 + i]; r->amin[i] = r->amax[i] = res[i]; }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += res[3 + i];
        if (res[3 + i] < r->gmin[i]) r->gmin[i] = res[3 + i];
        if (res[3 + i] > r->gmax[i]) r->gmax[i] = res[3 + i];
        if (res[i] < r->amin[i]) r->amin[i] = res[i];
        if (res[i] > r->amax[i]) r->amax[i] = res[i];
        if (r->gmax[i] - r->gmin[i] > 100 || r->amax[i] - r->amin[i] > 300) { r->n = 0; return; }   // moving: the window starts again
    }
    if (++r->n < JC1_REST_N) return;
    r->n = 0;
    float m[3]; for (int i = 0; i < 3; i++) { m[i] = (float)r->sum[i] / JC1_REST_N; if (fabsf(m[i]) > 60.f) return; }
    for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
    r->have = 1;
}

// One controller's live state: a repeated report counter is the same three samples again. clock is the newest sample's time, advanced 15 ms a report and slewed toward the
// arrival: stamped from the arrival alone, a report that comes early puts its three samples before the last one's, and the host drops those (t <= last) - a stall and a jump
// in fast play. data_ns: the arrival of the last report that carried sensor data (jc1_live, and the watchdog's jc1_quiet).
typedef struct { int seen; uint8_t counter; uint64_t received, clock, emitted, data_ns; } jc1_stream;
typedef struct { float acc[3], rot[3]; uint64_t ns; } jc1_sample;
enum { JC1_STALE_NS = 300000000 };
// Everything the host knows of one Joy-Con's sensor: its calibration (the factory's, from SPI), its learned rest and its stream.
typedef struct { jc1_cal cal; jc1_rest rest; jc1_stream stream; } jc1_unit;
static inline void jc1_unit_init(jc1_unit *u) { memset(u, 0, sizeof *u); jc1_cal_default(&u->cal); }
// A 0x21 reply: 1 when it is the IMU calibration. A rest learned against another origin (the default 0, before the reply came) is of no use against this one and starts again;
// the same calibration read again (the watchdog's handshake after a wake) keeps it.
static inline int jc1_unit_cal(jc1_unit *u, const uint8_t *r, size_t len) {
    jc1_cal c = u->cal;
    if (!jc1_parse_cal(&c, r, len)) return 0;
    for (int i = 0; i < 3; i++) if (c.gyro_origin[i] != u->cal.gyro_origin[i]) { memset(&u->rest, 0, sizeof u->rest); break; }
    u->cal = c;
    return 1;
}
static inline int jc1_live(const jc1_stream *s, uint64_t now) { return s->data_ns && now >= s->data_ns && now - s->data_ns < JC1_STALE_NS; }
// The watchdog's test: no sensor data for wait since the later of the last data and the last handshake (asked). That is both a controller that sends nothing and one whose
// reports come with the sensor off (all zero: macOS resets the report mode in some wakes).
static inline int jc1_quiet(const jc1_stream *s, uint64_t asked, uint64_t wait, uint64_t now) {
    uint64_t since = s->data_ns > asked ? s->data_ns : asked;
    return now >= since && now - since >= wait;
}

// One report 0x30 in: its three samples out (n of them, 0 to 3). 0 for a short or foreign report, a repeated one, or one whose sensor block is all zero: that is the sensor
// off (before EnableIMU, or after a wake reset it), never a reading, since a real gyro is never exactly 0 on all three axes three times running, even in free fall. Each
// fresh report also teaches the rest (jc1_rest_update). acc is the gravity direction in G and rot radians/s, in the host's Switch frame; ns is each sample's own time.
static inline int jc1_input(jc1_unit *u, const uint8_t *r, size_t len, int left, uint64_t now, jc1_sample out[JC1_IMU_SAMPLES]) {
    jc1_stream *s = &u->stream;
    if (len < JC1_REPORT_LEN || r[0] != 0x30 || now < 2 * (uint64_t)JC1_IMU_PERIOD_NS) return 0;
    if (s->seen && (now < s->received || now - s->received > JC1_STALE_NS)) *s = (jc1_stream){0};
    if (s->seen && s->counter == r[1]) return 0;
    int16_t v[JC1_IMU_SAMPLES][6]; int data = 0;
    for (int k = 0; k < JC1_IMU_SAMPLES; k++) for (int i = 0; i < 6; i++) { v[k][i] = jc1_s16(r + 13 + 12 * k + 2 * i); data |= v[k][i] != 0; }
    s->seen = 1; s->counter = r[1]; s->received = now;
    if (!data) return 0;
    s->data_ns = now;
    for (int k = 0; k < JC1_IMU_SAMPLES; k++) {
        int16_t res[6];
        for (int i = 0; i < 3; i++) { res[i] = v[k][i]; res[3 + i] = (int16_t)(v[k][3 + i] - (int)u->cal.gyro_origin[i]); }
        jc1_rest_update(&u->rest, res);
    }
    float offset[3];
    for (int i = 0; i < 3; i++) offset[i] = u->cal.gyro_origin[i] + (u->rest.have ? u->rest.bias[i] : 0.f);
    int64_t drift = (int64_t)now - (int64_t)s->clock;
    if (!s->clock || drift > 100000000 || drift < -100000000) s->clock = now;   // first report, or after a stall
    else s->clock = (uint64_t)((int64_t)s->clock + drift / 4);                   // 15 ms a report, pulled a quarter toward the arrival
    // The three samples end at the clock, not later than the report arrived, and none repeats one already given: an early
    // report (a burst after a hiccup) leaves fewer than three, where stamping from the arrival gave samples the host drops.
    uint64_t newest = s->clock > now ? now : s->clock;
    s->clock += (uint64_t)JC1_IMU_SAMPLES * JC1_IMU_PERIOD_NS;
    const float deg = (float)M_PI / 180.f;
    const jc1_cal *cal = &u->cal;
    int n = 0;
    for (int k = 0; k < JC1_IMU_SAMPLES; k++) {
        uint64_t t = newest - (uint64_t)(2 - k) * JC1_IMU_PERIOD_NS;
        if (s->emitted && t <= s->emitted) continue;
        float ax = v[k][0] * cal->accel_scale[0], ay = v[k][1] * cal->accel_scale[1], az = v[k][2] * cal->accel_scale[2];
        float gx = (v[k][3] - offset[0]) * cal->gyro_scale[0];
        float gy = (v[k][4] - offset[1]) * cal->gyro_scale[1];
        float gz = (v[k][5] - offset[2]) * cal->gyro_scale[2];
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

// Routing. GameController lists a pair as one controller (or each Joy-Con as its own) and gives it no HID identity, so with more than one Joy-Con controller the host cannot
// tell from IOKit which controller, and so which player, a Joy-Con belongs to. A button press tells it: when one Joy-Con alone is pressing, and has been long enough for
// GameController to have seen the same report (JC1_PRESS_SETTLE_NS), and no other has just let go, the one Joy-Con controller pressed with it is its own. Each press is judged
// once, and one that stays ambiguous (two players pressing together, or a button GameController does not show) gives no verdict, so a later press decides; a press that names
// another controller than the one the Joy-Con was routed to moves it. The buttons are bytes 3 (right), 4 (shared; bit 7 is the charging grip, not a button) and 5 (left).
static inline uint32_t jc1_buttons(const uint8_t *r) { return (uint32_t)r[3] | (uint32_t)(r[4] & 0x3f) << 8 | (uint32_t)r[5] << 16; }
typedef struct { int pressing, judged; uint64_t press_ns, release_ns; } jc1_press;
enum { JC1_PRESS_SETTLE_NS = 30000000, JC1_PRESS_GIVE_UP_NS = 400000000 };
enum { JC1_PRESS_NONE, JC1_PRESS_WAIT, JC1_PRESS_UNSURE, JC1_PRESS_BIND };
static inline void jc1_press_update(jc1_press *p, int pressing, uint64_t now) {
    if (pressing && !p->pressing) { p->press_ns = now; p->judged = 0; }
    if (!pressing && p->pressing) p->release_ns = now;
    p->pressing = pressing;
}
// all: every Joy-Con's press (self is the one judged); controllers_pressed: the Joy-Con controllers that can hold self's side and show a button pressed now. BIND: the one
// pressed controller is self's; UNSURE: this press decides nothing (the caller marks it judged); WAIT: ask again at its next report; NONE: no press to judge.
static inline int jc1_press_judge(const jc1_press *all, int n, int self, int controllers_pressed, uint64_t now) {
    const jc1_press *p = &all[self];
    if (!p->pressing || p->judged) return JC1_PRESS_NONE;
    int late = now - p->press_ns >= JC1_PRESS_GIVE_UP_NS;
    if (now - p->press_ns < JC1_PRESS_SETTLE_NS) return JC1_PRESS_WAIT;
    for (int i = 0; i < n; i++) if (i != self && (all[i].pressing || (all[i].release_ns && now - all[i].release_ns < JC1_PRESS_SETTLE_NS)))
        return late ? JC1_PRESS_UNSURE : JC1_PRESS_WAIT;
    if (controllers_pressed == 1) return JC1_PRESS_BIND;
    return late ? JC1_PRESS_UNSURE : JC1_PRESS_WAIT;
}
