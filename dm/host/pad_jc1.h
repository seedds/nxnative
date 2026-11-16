// Joy-Con (Switch 1) report 0x30, standard full mode: 49 bytes, buttons at 3..5, sticks at 6..11, then three
// 12-byte IMU samples at 13 (accel XYZ then gyro XYZ, int16 LE, 5 ms apart, the third the newest). macOS keeps
// this report flowing at 60 Hz over Bluetooth Classic but zeroes the IMU until the host sends EnableIMU (0x40),
// so a passive reader sees a live stream of nothing: the transport in pad_jc1_mac.h sends the subcommands.
// Everything here is pure, so tests/t_jc1.c checks it without a controller: the sensor (below), then the buttons,
// the stick, a Joy-Con held sideways, HD rumble, the player lights and how Joy-Cons make a pad.
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
#include "pad_pro.h"
#include "hle_haptics.h"

enum {
    JC1_REPORT_LEN = 49,
    JC1_IMU_PERIOD_NS = 5000000,
    JC1_IMU_SAMPLES = 3,
    JC1_SUB_DEVICE_INFO = 0x02,
    JC1_SUB_INPUT_MODE = 0x03,
    JC1_SUB_SPI_READ = 0x10,
    JC1_SUB_LEDS = 0x30,
    JC1_SUB_ENABLE_IMU = 0x40,
    JC1_SUB_VIBRATION = 0x48,
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


// ---- Buttons and the stick (dekuNukem's bluetooth_hid_notes.md, report 0x30) ----
// Byte 3 is the right Joy-Con's (Y X B A SR SL R ZR from bit 0), byte 4 the shared one (Minus Plus RStick LStick Home Capture; bit 7 is the charging grip, not a button),
// byte 5 the left one's (Down Up Right Left SR SL L ZL). A Joy-Con's own buttons as NpadButtons, its rail buttons included (DM_BTN_LSL..RSR), for dm_joy_set
typedef struct { uint8_t at, bit; uint32_t btn; } jc1_bit;
static inline uint64_t jc1_joy_buttons(const uint8_t *r, int right) {
    static const jc1_bit left[] = {
        {5, 0x01, DM_BTN_DOWN}, {5, 0x02, DM_BTN_UP}, {5, 0x04, DM_BTN_RIGHT}, {5, 0x08, DM_BTN_LEFT}, {5, 0x10, DM_BTN_LSR}, {5, 0x20, DM_BTN_LSL},
        {5, 0x40, DM_BTN_L}, {5, 0x80, DM_BTN_ZL}, {4, 0x01, DM_BTN_MINUS}, {4, 0x08, DM_BTN_LS},
    }, rightmap[] = {
        {3, 0x01, DM_BTN_Y}, {3, 0x02, DM_BTN_X}, {3, 0x04, DM_BTN_B}, {3, 0x08, DM_BTN_A}, {3, 0x10, DM_BTN_RSR}, {3, 0x20, DM_BTN_RSL},
        {3, 0x40, DM_BTN_R}, {3, 0x80, DM_BTN_ZR}, {4, 0x02, DM_BTN_PLUS}, {4, 0x04, DM_BTN_RS},
    };
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof left / sizeof *left; i++) { const jc1_bit *e = right ? &rightmap[i] : &left[i]; if (r[e->at] & e->bit) m |= e->btn; }
    return m;
}
static inline int jc1_home(const uint8_t *r) { return (r[4] & 0x10) != 0; }   // the right Joy-Con's Home (byte 4 bit 4); Capture (bit 5) has no use in the host
// The Joy-Con's stick, -32767..32767 with y up: the left one's is the report's left stick (bytes 6..8), the right one's its right stick (9..11). pad_pro.h decodes both and keeps the
// calibration (the SPI blocks the Pro Controller has: factory 0x603D, user 0x8010; a left Joy-Con fills the left stick's block, a right one the right's), so only the Joy-Con's half is read
static inline int jc1_joy_stick(pad_pro *s, const uint8_t *r, size_t len, int right, int32_t out[2]) {
    uint64_t m; int32_t st[4];
    if (!pad_pro_decode(s, r, len, &m, st)) return 0;
    out[0] = st[right ? 2 : 0]; out[1] = st[right ? 3 : 1];
    return 1;
}

// ---- A Joy-Con held sideways ----
// Held on its own, a Joy-Con lies across the hands with its rail away from the player and its stick on the left: the left one turned a quarter counterclockwise (its top to the
// left), the right one a quarter clockwise (its top to the right). A vector in the upright Joy-Con's frame (x right, y toward its top, z out of the face) is then (-y, x, z) for the
// left one and (y, -x, z) for the right one: the stick, and the six-axis sensor for a game that reads the pad as one whole controller (dm_motion_set_hid; hle_hid.c takes that stream
// in the frame of a Joy-Con held sideways). The face buttons turn with it, so the one on the right is A: the left one's Down, the right one's X.
static inline void jc1_sideways_vec(const float in[3], int right, float out[3]) {
    float x = in[0], y = in[1];
    out[0] = right ? y : -y; out[1] = right ? -x : x; out[2] = in[2];
}
static inline void jc1_sideways_stick(const int32_t in[2], int right, int32_t out[2]) {
    int32_t x = in[0], y = in[1];
    out[0] = right ? y : -y; out[1] = right ? -x : x;
}
// Its buttons as a whole controller's: the four face buttons by their place, the rail buttons as L and R, its stick button as the left stick's. Shoulders, Plus and Minus stay
static inline uint64_t jc1_sideways_buttons(uint64_t b, int right) {
    static const uint64_t from_l[4] = {DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT, DM_BTN_UP}, from_r[4] = {DM_BTN_X, DM_BTN_A, DM_BTN_Y, DM_BTN_B};   // what becomes A, B, X, Y
    static const uint64_t to[4] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y};
    const uint64_t *from = right ? from_r : from_l;
    uint64_t m = b & (DM_BTN_L | DM_BTN_R | DM_BTN_ZL | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_MINUS);
    for (int i = 0; i < 4; i++) if (b & from[i]) m |= to[i];
    if (b & (DM_BTN_LSL | DM_BTN_RSL)) m |= DM_BTN_L;
    if (b & (DM_BTN_LSR | DM_BTN_RSR)) m |= DM_BTN_R;
    if (b & (DM_BTN_LS | DM_BTN_RS)) m |= DM_BTN_LS;
    return m;
}
// A pad's buttons and sticks as one whole controller, for a game that supports no Joy-Con style (dm_pad_set_player): a left and a right Joy-Con together are a Pro Controller (the
// left's stick the left stick, the right's the right stick, no rail buttons), one alone is held sideways. b and st: each side's jc1_joy_buttons and stick. 0: the pad holds none
static inline int jc1_pad_input(const int have[2], const uint64_t b[2], const int32_t st[2][2], uint64_t *m, int32_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0; *m = 0;
    if (have[0] && have[1]) {
        *m = (b[0] | b[1]) & 0xffffu;
        out[0] = st[0][0]; out[1] = st[0][1]; out[2] = st[1][0]; out[3] = st[1][1];
        return 1;
    }
    for (int s = 0; s < 2; s++) if (have[s]) { *m = jc1_sideways_buttons(b[s], s); jc1_sideways_stick(st[s], s, out); return 1; }
    return 0;
}
// The Joy-Con that feeds the pad's merged six-axis stream: the right one of a pair (side 1; prefer 0 picks the left, DM_JC1_SIDE=L), unless it is not streaming and the left one
// is; the one there is when it is alone. -1: none
static inline int jc1_merged_side(const int have[2], const int live[2], int prefer) {
    if (have[0] && have[1]) return live[prefer] || !live[!prefer] ? prefer : !prefer;
    return have[1] ? 1 : have[0] ? 0 : -1;
}

// ---- How Joy-Cons make a pad (one player) ----
// A Joy-Con that connects joins a pad that holds the other side alone (the first such in the list), else it is a pad of its own; the npad layer then shows the game a pair as
// one dual npad or two single ones, as the game asks (hle_hid.c). sides[i]: pad i's Joy-Cons (bit 0 left, bit 1 right). Returns the pad to join, -1 for a new one
static inline int jc1_pair_pick(const uint32_t *sides, int n, int right) {
    uint32_t other = right ? 1u : 2u;
    for (int i = 0; i < n; i++) if (sides[i] == other) return i;
    return -1;
}
// Holding SL and SR on a Joy-Con of a pair for three seconds makes it a pad of its own; holding L on a left one alone and R on a right one alone, together for three seconds, pairs
// them: the Switch 2 transport's gestures (switch2_proto.h s2_gesture), on NpadButtons
#define JC1_GESTURE_NS 3000000000ull
enum { JC1_GESTURE_NONE, JC1_GESTURE_SPLIT, JC1_GESTURE_JOIN };
static inline int jc1_gesture(uint64_t b, int right, int paired) {
    uint64_t rails = right ? (DM_BTN_RSL | DM_BTN_RSR) : (DM_BTN_LSL | DM_BTN_LSR);
    if (paired) return (b & rails) == rails ? JC1_GESTURE_SPLIT : JC1_GESTURE_NONE;
    return (b & (right ? DM_BTN_R : DM_BTN_L)) && !(b & rails) ? JC1_GESTURE_JOIN : JC1_GESTURE_NONE;
}
typedef struct { int kind, done; uint64_t since; } jc1_hold;
static inline int jc1_hold_update(jc1_hold *h, int kind, uint64_t now) {   // 1 while kind has been held JC1_GESTURE_NS without a break
    if (kind != h->kind) { h->kind = kind; h->since = now; h->done = 0; }
    return kind != JC1_GESTURE_NONE && now - h->since >= JC1_GESTURE_NS;
}

// ---- Player lights: subcommand 0x30, the low nibble lit, the high nibble flashing ----
// The console's patterns for players 1..8 (the Switch 2 transport's s2_led_pattern too); a Joy-Con with no player yet flashes all four, as the console's controller screen does
static inline uint8_t jc1_led_pattern(int player) {
    static const uint8_t p[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};
    return player >= 0 && player < 8 ? p[player] : 0xF0;
}

// ---- HD rumble (dekuNukem's rumble_data_table.md; Linux hid-nintendo and SDL encode it the same way) ----
// Output report 0x10: id, the packet counter, then 4 bytes for the left actuator and 4 for the right; a Joy-Con plays its own half (both carry its sample here). A sample is two
// bands, each a frequency and an amplitude on log scales: the frequency code is round(32 log2(f / 10 Hz)), the high band 0x61..0xDF (81.7..1252 Hz) stored as (code - 0x60) * 4
// in 9 bits, the low band 0x41..0xBF (40.9..626 Hz) stored as code - 0x40 in 7 bits. The amplitude code e is 0..100 (1.0 at 100, off at 0; jc1_amp_of); the high band stores 2e in its byte, the low band (e >> 1) + 0x40 with e's low bit in bit 15. Bytes: high frequency's
// low 8 bits; high amplitude + its 9th bit; low frequency + the low amplitude's bit 15; the low amplitude's low byte. Neutral (nothing, at the default 160 / 320 Hz): 00 01 40 40.
static inline int jc1_freq_code(float hz, int lo, int hi) {
    if (!(hz > 0.f)) hz = 160.f;
    long c = lrintf(32.f * log2f(hz / 10.f));
    return c < lo ? lo : c > hi ? hi : (int)c;
}
// The amplitude of code e (the table's): 2^(e/4) / 120 for 1..15, 2^(e/16) / 17 for 16..31, 2^(e/32) / 8.7 for 32..100 (0.0099, 0.1121, 0.1176, 0.2253, 0.2299, 1.0028)
static inline float jc1_amp_of(int e) { return e <= 0 ? 0.f : e < 16 ? exp2f(e / 4.f) / 120.f : e < 32 ? exp2f(e / 16.f) / 17.f : exp2f(e / 32.f) / 8.7f; }
// The code whose amplitude is nearest a on the log scale the codes follow; below half of code 1's, none
static inline int jc1_amp_code(float a) {
    if (!(a >= jc1_amp_of(1) * 0.70710678f)) return 0;
    int lo = 1, hi = 100;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (jc1_amp_of(mid) <= a) lo = mid; else hi = mid - 1; }   // the last code at or below a
    return lo < 100 && a * a > jc1_amp_of(lo) * jc1_amp_of(lo + 1) ? lo + 1 : lo;
}
static inline void jc1_rumble_encode(const dm_vibration *v, uint8_t out[4]) {
    int hf = (jc1_freq_code(v->frequency_high, 0x61, 0xDF) - 0x60) * 4, lf = jc1_freq_code(v->frequency_low, 0x41, 0xBF) - 0x40;
    int eh = jc1_amp_code(v->amplitude_high), el = jc1_amp_code(v->amplitude_low);
    out[0] = (uint8_t)(hf & 0xff);
    out[1] = (uint8_t)(2 * eh + (hf >> 8));
    out[2] = (uint8_t)(lf | (el & 1) << 7);
    out[3] = (uint8_t)((el >> 1) + 0x40);
}
static inline int jc1_vibration_on(const dm_vibration *v) { return jc1_amp_code(v->amplitude_low) || jc1_amp_code(v->amplitude_high); }
enum { JC1_RUMBLE_LEN = 10 };
static inline void jc1_rumble_report(uint8_t out[JC1_RUMBLE_LEN], uint8_t counter, const uint8_t sample[4]) {
    out[0] = 0x10; out[1] = (uint8_t)(counter & 0x0f);
    memcpy(out + 2, sample, 4); memcpy(out + 6, sample, 4);
}
// What one Joy-Con plays of its player's two motors (m[0] the left handle, m[1] the right): its own side's in a pair, the stronger of the two in each band when it is alone (it is the
// whole controller then), as the Switch 2 transport's s2_rumble_pick
static inline dm_vibration jc1_rumble_pick(const dm_vibration m[2], int right, int paired) {
    if (paired) return m[right];
    dm_vibration v = m[0];
    if (m[1].amplitude_low > v.amplitude_low) { v.amplitude_low = m[1].amplitude_low; v.frequency_low = m[1].frequency_low; }
    if (m[1].amplitude_high > v.amplitude_high) { v.amplitude_high = m[1].amplitude_high; v.frequency_high = m[1].frequency_high; }
    return v;
}
// A report carries one sample per actuator and goes every 15 ms, while a game sets its vibration about every 5 ms: a pulse set and cleared between two reports would never play. So
// each report plays the strongest value of each motor set since the last one (held for that report), then what is set now
typedef struct { dm_vibration cur[2], peak[2]; } jc1_rumble_hold;
static inline float jc1_vib_strength(const dm_vibration *v) { return v->amplitude_low + v->amplitude_high; }
static inline void jc1_rumble_set(jc1_rumble_hold *h, const dm_vibration m[2]) {
    for (int k = 0; k < 2; k++) { h->cur[k] = m[k]; if (jc1_vib_strength(&m[k]) >= jc1_vib_strength(&h->peak[k])) h->peak[k] = m[k]; }
}
static inline void jc1_rumble_take(jc1_rumble_hold *h, dm_vibration out[2]) {
    for (int k = 0; k < 2; k++) { out[k] = h->peak[k]; h->peak[k] = h->cur[k]; }
}
