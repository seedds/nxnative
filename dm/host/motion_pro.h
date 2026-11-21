// Nintendo Switch Pro (passive), Switch 1 Joy-Con L/R (IMU enable + SPI cal) and DualShock 4 (passive) raw HID IMU reports (#296, #1770).
// Layout / nominal scales: dekuNukem/Nintendo_Switch_Reverse_Engineering, imu_sensor_notes.md. The LSM6DS3's ±2000 dps sensitivity is 70 mdps/count,
// not 4000/65536 (61 mdps/count). Joy-Con 1 uses SPI gyro calibration (0x8026 user / 0x6020 factory) when loaded; never learn an offset from a slow deliberate turn.
// Samples follow byte order, 5 ms apart, as in Linux hid-nintendo and the protocol notes. SDL reverses them; hardware confirmation remains open.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

enum { PRO_MOTION_PERIOD_NS = 5000000, PRO_MOTION_STALE_NS = 300000000 };
enum { PRO_IMU_SPI_FACTORY = 0x6020, PRO_IMU_SPI_FACTORY_LEN = 24, PRO_IMU_SPI_USER = 0x8026, PRO_IMU_SPI_USER_LEN = 26 };
typedef struct { int valid; int16_t gyro_off[3]; float gyro_rad[3]; } pro_imu_cal;   // valid: 1 factory, 2 user
typedef struct { int live, seen, flip_r; uint8_t counter; uint64_t received; pro_imu_cal cal; } pro_motion;
typedef struct { float acc[3], rot[3]; uint64_t ns; } pro_motion_sample;
static inline void pro_motion_reset(pro_motion *s) { s->live = s->seen = 0; s->counter = 0; s->received = 0; }
// Output report 0x01 with neutral rumble and a short subcommand (up to 5 arg bytes: e.g. 0x40 enable IMU, 0x10 SPI read). Returns length, 0 if args too long
static inline size_t pro_subcmd_build(uint8_t out[16], uint8_t counter, uint8_t subcmd, const uint8_t *args, size_t n) {
    if (n > 5) return 0;
    static const uint8_t rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    out[0] = 0x01; out[1] = counter & 0x0f;
    for (int i = 0; i < 8; i++) out[2 + i] = rumble[i];
    out[10] = subcmd;
    for (size_t i = 0; i < n; i++) out[11 + i] = args[i];
    return 11 + n;
}
// Reply to subcommand 0x10 (report 0x21) carrying Switch IMU calibration (user 0x8026 with magic 0xB2 0xA1 wins over factory 0x6020).
static inline int pro_imu_cal_parse(pro_imu_cal *cal, const uint8_t *r, size_t len) {
    if (len < 20 || r[0] != 0x21 || !(r[13] & 0x80) || r[14] != 0x10) return 0;
    uint32_t addr = (uint32_t)r[15] | ((uint32_t)r[16] << 8) | ((uint32_t)r[17] << 16) | ((uint32_t)r[18] << 24);
    size_t n = r[19];
    if (len < 20 + n) return 0;
    const uint8_t *d = NULL; int kind = 0;
    if (addr == PRO_IMU_SPI_USER && n >= PRO_IMU_SPI_USER_LEN && r[20] == 0xb2 && r[21] == 0xa1) { d = r + 22; kind = 2; }
    else if (addr == PRO_IMU_SPI_FACTORY && n >= PRO_IMU_SPI_FACTORY_LEN && cal->valid < 2) { d = r + 20; kind = 1; }
    if (!d) return 0;
    pro_imu_cal tmp = {0};
    for (int i = 0; i < 3; i++) {
        int16_t goff  = (int16_t)((uint16_t)d[12 + 2 * i] | ((uint16_t)d[13 + 2 * i] << 8));
        int16_t gsens = (int16_t)((uint16_t)d[18 + 2 * i] | ((uint16_t)d[19 + 2 * i] << 8));
        int32_t denom = (int32_t)gsens - (int32_t)goff;
        if (denom < 4000 || denom > 30000) return 0;
        tmp.gyro_off[i] = goff;
        tmp.gyro_rad[i] = (936.0f / (float)denom) * ((float)M_PI / 180.0f);
    }
    tmp.valid = kind;
    *cal = tmp;
    return 1;
}
// Whether a pad's HID entry has a pad behind it: macOS keeps a Pro Controller in full mode, which sends report 0x30 every 15 ms whether or not anything moves
// (bluetooth_hid_notes.md: 60 Hz; the raw motion above lives on it), and a DualShock 4 over Bluetooth streams its 0x11 the same way, so heard is when its last motion report
// came and an entry silent for PRO_SILENT_NS counts as no pad.
// After a long Bluetooth disconnect, IOKit can keep a second entry of the pad's model that sends nothing and whose output fails with MACH_SEND_INVALID_DEST
// (0x10000003): counted, one pad was two (GitHub issue #2237)
enum { PRO_SILENT_NS = 1000000000 };
static inline int pro_live(uint64_t heard, uint64_t now) { return heard && now >= heard && now - heard < PRO_SILENT_NS; }
static inline int pro_motion_player(int devices, int controllers, int player) {
    // GameController has no public per-controller HID identity. Only one physical pad and one GC controller of a model can be paired without guessing.
    return devices == 1 && controllers == 1 ? player : -1;
}
static inline int pro_motion_decode(pro_motion *s, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (len < 49 || r[0] != 0x30 || now < 2 * PRO_MOTION_PERIOD_NS) return 0;
    if (s->seen && (now < s->received || now - s->received > PRO_MOTION_STALE_NS)) pro_motion_reset(s);
    if (s->seen && s->counter == r[1]) return 0;   // repeated report, not another three samples
    int16_t v[3][6]; int data = 0;
    for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) {
        const uint8_t *b = r + 13 + 12 * k + 2 * i;
        v[k][i] = (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8); data |= v[k][i] != 0;
    }
    s->seen = 1; s->counter = r[1]; s->received = now;
    if (!s->live && !data) return 0;   // an advertised but empty stream is not a motion source
    s->live = 1;
    const float g = 1.f / 4096.f, rad = 0.070f * (float)M_PI / 180.f;
    for (int k = 0; k < 3; k++) {
        float fx = v[k][0] * g, fy = v[k][1] * g, fz = v[k][2] * g;
        float gx = s->cal.valid ? (v[k][3] - s->cal.gyro_off[0]) * s->cal.gyro_rad[0] : v[k][3] * rad;
        float gy = s->cal.valid ? (v[k][4] - s->cal.gyro_off[1]) * s->cal.gyro_rad[1] : v[k][4] * rad;
        float gz = s->cal.valid ? (v[k][5] - s->cal.gyro_off[2]) * s->cal.gyro_rad[2] : v[k][5] * rad;
        uint64_t ts = now - (uint64_t)(2 - k) * PRO_MOTION_PERIOD_NS;
        if (s->flip_r) {
            // Joy-Con (R) held upright: IMU PCB is flipped 180 deg around raw X (+x forward, +y right, +z down).
            out[k] = (pro_motion_sample){{-fy, -fx, fz}, {gy, gx, -gz}, ts};
        } else {
            // Pro Controller & Joy-Con (L) upright: x toward shoulders, y left, z out of face; acceleration is specific force. Switch: x right, y forward, gravity down.
            out[k] = (pro_motion_sample){{fy, -fx, -fz}, {-gy, gx, gz}, ts};
        }
    }
    return 3;
}
// DualShock 4 (054C:05C4 and 09CC) over Bluetooth (issue #1770): its GCMotion sends only zeroes, but its input report 0x11 carries the sensor, as Linux hid-playstation and SDL read it:
// the USB report 0x01's fields two bytes later, so six int16 from byte 15 (the id is byte 0), rotation x, y, z then acceleration x, y, z; nominal 8192 counts/G and 2000 deg/s
// over 32767, without the factory calibration (a feature report the OS driver owns). Raw axes: x right, y out of the face, z toward the player; acceleration is specific force.
// Into the Switch frame as pro_motion_decode does; the signs were measured on a pad (MacNativeGames PR #1306: the raw roll channel reads negative rolling right).
// One sample per report, stamped on arrival
static inline int ds4_motion_decode(pro_motion *s, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (len < 78 || r[0] != 0x11) return 0;
    if (s->seen && (now < s->received || now - s->received > PRO_MOTION_STALE_NS)) *s = (pro_motion){0};
    int16_t v[6]; int data = 0;
    for (int i = 0; i < 6; i++) { v[i] = (int16_t)((uint16_t)r[15 + 2 * i] | (uint16_t)r[16 + 2 * i] << 8); data |= v[i] != 0; }
    s->seen = 1; s->received = now;
    if (!s->live && !data) return 0;   // an advertised but empty stream is not a motion source
    s->live = 1;
    const float g = 1.f / 8192.f, rad = 2000.f / 32767.f * (float)M_PI / 180.f;
    out[0] = (pro_motion_sample){{-v[3] * g, v[5] * g, -v[4] * g}, {v[0] * rad, -v[2] * rad, v[1] * rad}, now};
    return 1;
}
