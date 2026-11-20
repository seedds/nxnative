// Passive Nintendo Switch Pro IMU reports (#296). No commands or calibration writes: macOS owns the device and enables report 0x30.
// Layout / nominal scales: dekuNukem/Nintendo_Switch_Reverse_Engineering, imu_sensor_notes.md. The LSM6DS3's ±2000 dps sensitivity is 70 mdps/count,
// not 4000/65536 (61 mdps/count). Factory offsets / sensitivity are not available without SPI commands; never learn an offset from a slow deliberate turn.
// Samples follow byte order, 5 ms apart, as in Linux hid-nintendo and the protocol notes. SDL reverses them; hardware confirmation remains open.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

enum { PRO_MOTION_PERIOD_NS = 5000000, PRO_MOTION_STALE_NS = 300000000 };
typedef struct { int live, seen; uint8_t counter; uint64_t received; } pro_motion;
typedef struct { float acc[3], rot[3]; uint64_t ns; } pro_motion_sample;
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
    if (s->seen && (now < s->received || now - s->received > PRO_MOTION_STALE_NS)) *s = (pro_motion){0};
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
        // Raw axes: x toward shoulders, y left, z out of face; acceleration is specific force. Switch: x right, y forward, gravity down.
        out[k] = (pro_motion_sample){{v[k][1] * g, -v[k][0] * g, -v[k][2] * g},
                                    {-v[k][4] * rad, v[k][3] * rad, v[k][5] * rad}, now - (uint64_t)(2 - k) * PRO_MOTION_PERIOD_NS};
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
