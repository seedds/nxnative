// A Joy-Con's IMU reports. Pure (no IOKit, no host state), so tests/t_joycon_imu.c checks it on any machine.
// Apple gives a Joy-Con pair no motion at all (GCController.motion is nil), and macOS drives each unit in report mode 0x3F, which carries no
// IMU. In 0x30 a report holds the buttons and sticks, then three sensor frames 5 ms apart, oldest first: int16 acceleration (x, y, z) and
// int16 rotation (x, y, z) at 13 + 12k. Raw counts are 4096 per G and 13371 per revolution/s (Nintendo's own unit); dm_motion_set_hid wants
// G and rad/s in the Switch frame, and the acceleration as gravity, not as the specific force the sensor reports, so a unit lying flat
// reads (0, 0, -1). Frame: the right unit's IMU is mounted mirrored - Y and Z change sign, X does not (Linux hid-nintendo.c), never permute;
// the left unit's own frame has not been measured, so it is passed through and used only while no right unit is open.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

enum { JC_IMU_PERIOD_NS = 5000000, JC_IMU_STALE_NS = 300000000 };
typedef struct { int right, live; uint8_t counter; uint64_t received; } jc_imu;
typedef struct { float acc[3], rot[3]; uint64_t ns; } jc_imu_sample;

// Three samples from one 0x30 report (0: not a report for us, a repeat, an empty stream, or a stale gap)
static inline int joycon_imu_decode(jc_imu *s, const uint8_t *r, size_t len, uint64_t now, jc_imu_sample out[3]) {
    if (len < 49 || r[0] != 0x30 || now < 2 * (uint64_t)JC_IMU_PERIOD_NS) return 0;
    if (s->received && (now < s->received || now - s->received > JC_IMU_STALE_NS)) { s->live = 0; s->counter = 0; s->received = 0; }
    if (s->received && s->counter == r[1]) return 0;   // the same report delivered twice
    int16_t v[3][6]; int data = 0;
    for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) {
        const uint8_t *b = r + 13 + 12 * k + 2 * i;
        v[k][i] = (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8); data |= v[k][i] != 0;
    }
    s->counter = r[1]; s->received = now;
    if (!s->live && !data) return 0;   // enabled but empty is not a source: a unit at rest still reads about 1 G
    s->live = 1;
    const float g = 1.f / 4096.f, rad = (float)(2.0 * M_PI) / 13371.f;   // counts per G, counts per revolution/s
    float sx = s->right ? -1.f : 1.f;
    for (int k = 0; k < 3; k++) {
        out[k] = (jc_imu_sample){{-(float)v[k][0] * g, -sx * (float)v[k][1] * g, -sx * (float)v[k][2] * g},   // gravity: flat reads (0, 0, -1)
                                 {(float)v[k][3] * rad, sx * (float)v[k][4] * rad, sx * (float)v[k][5] * rad},
                                 now - (uint64_t)(2 - k) * (uint64_t)JC_IMU_PERIOD_NS};
    }
    return 3;
}
