// The six-axis sensor in the input reports of a pad GameController adopts with no motion. Passive: no commands, no calibration writes.
// The 8BitDo Ultimate 2 Wireless (2dc8:6012, Bluetooth, firmware 1.03 or later) is a plain extended gamepad to GameController (productCategory "HID",
// motion nil), so a game found no gyro on it. Its input report 1 (34 bytes, the report ID as byte 0) carries the sensor: int16 little endian from byte 15,
// acceleration x y z then rotation x y z, 4096 counts = 1 G and +-2000 deg/s over 32767 (SDL's SDL_hidapi_8bitdo.c; measured on the pad in
// ly05010419/MacNativeGames PR #309, MIT: about 63 reports a second over Bluetooth, the gyro at rest within 0.5 deg/s of zero, so no calibration).
// Its axes are the Switch Pro Controller's raw axes (x toward the top edge, y left, z out of the face), so the frame change is pro_motion_decode's.
// Firmware 1.02 sends 12-byte reports without the sensor: no sample, and the transport says so once. One line in the table is one more such pad.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

enum { IMU_MOTION_STALE_NS = 300000000 };
typedef struct { uint16_t vendor, product; uint8_t report_id, length, offset; float counts_per_g, counts_per_dps; const char *name; } imu_profile;
static const imu_profile imu_profiles[] = {
    {0x2dc8, 0x6012, 0x01, 34, 15, 4096.f, 32767.f / 2000.f, "8BitDo Ultimate 2 Wireless"},
};
enum { IMU_PROFILES = sizeof imu_profiles / sizeof *imu_profiles };
typedef struct { int live, seen; uint64_t received, last; } imu_motion;
typedef struct { float acc[3], rot[3]; uint64_t ns; } imu_motion_sample;

static inline const imu_profile *imu_profile_find(int vendor, int product) {
    for (int i = 0; i < IMU_PROFILES; i++) if (imu_profiles[i].vendor == vendor && imu_profiles[i].product == product) return &imu_profiles[i];
    return NULL;
}
// The time of a sample (ns, now_ns's clock): the kernel's receive time of its report when plausible (not 0, at most now, at most 1 s old), else now, and always
// after the previous one. Callback times bunch up while the main run loop is busy; the kernel's stay even, and the fusion integrates over the time between samples.
static inline uint64_t imu_motion_time(imu_motion *s, uint64_t kernel_ns, uint64_t now) {
    uint64_t t = kernel_ns && kernel_ns <= now && now - kernel_ns <= 1000000000ull ? kernel_ns : now;
    if (t <= s->last) t = s->last + 1;
    if (t > now) return 0;   // no room for an increasing time: the sample is dropped
    return s->last = t;
}
// One input report (the report ID as byte 0) to a sample in the host's frame (as pro_motion_decode): 1, or 0 for another report, a short one, or an
// advertised but empty stream (all zeroes before any data).
static inline int imu_motion_decode(const imu_profile *pr, imu_motion *s, uint32_t report_id, const uint8_t *r, size_t len, uint64_t kernel_ns, uint64_t now, imu_motion_sample *out) {
    if (!pr || report_id != pr->report_id || len < pr->length || r[0] != pr->report_id) return 0;
    if (s->seen && (now < s->received || now - s->received > IMU_MOTION_STALE_NS)) *s = (imu_motion){0};
    int16_t v[6]; int data = 0;
    for (int i = 0; i < 6; i++) { const uint8_t *b = r + pr->offset + 2 * i; v[i] = (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8); data |= v[i] != 0; }
    s->seen = 1; s->received = now;
    if (!s->live && !data) return 0;
    uint64_t t = imu_motion_time(s, kernel_ns, now); if (!t) return 0;
    s->live = 1;
    const float g = 1.f / pr->counts_per_g, rad = (float)M_PI / 180.f / pr->counts_per_dps;
    // Raw axes: x toward the shoulders, y left, z out of the face; acceleration is specific force. Switch: x right, y forward, gravity down (motion_pro.h)
    *out = (imu_motion_sample){{v[1] * g, -v[0] * g, -v[2] * g}, {-v[4] * rad, v[3] * rad, v[5] * rad}, t};
    return 1;
}
