#include "../motion_imu.h"
#include "../motion_pro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void word(uint8_t *r, int i, int v) { uint16_t n = (uint16_t)v; r[15 + 2 * i] = n; r[16 + 2 * i] = n >> 8; }
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }
int main(void) {
    const imu_profile *pr = imu_profile_find(0x2dc8, 0x6012);
    assert(pr && pr->report_id == 1 && pr->length == 34 && pr->offset == 15);
    assert(!imu_profile_find(0x2dc8, 0x301b) && !imu_profile_find(0x057e, 0x2009));
    imu_motion s = {0}; imu_motion_sample o; uint8_t r[64] = {0x01}; uint64_t t = 2000000000;
    assert(!imu_motion_decode(pr, &s, 1, r, 12, t, t, &o));     // firmware 1.02: 12-byte reports without the sensor
    assert(!imu_motion_decode(pr, &s, 2, r, 34, t, t, &o));     // another report
    assert(!imu_motion_decode(pr, &s, 1, r, 34, t, t, &o));     // advertised but empty stream
    assert(!imu_motion_decode(NULL, &s, 1, r, 34, t, t, &o));
    // Face up at rest: z = +1 G raw (4096), a turn about the vertical axis at 200 deg/s, a roll and a pitch
    word(r, 0, 2048); word(r, 1, -1024); word(r, 2, 4096); word(r, 3, 3277); word(r, 4, -1638); word(r, 5, 3277);
    assert(imu_motion_decode(pr, &s, 1, r, 34, t - 3000000, t, &o));
    assert(near(o.acc[0], -.25f) && near(o.acc[1], -.5f) && near(o.acc[2], -1.f));   // the frame change of pro_motion_decode
    const float dps = (float)M_PI / 180.f * 2000.f / 32767.f;
    assert(near(o.rot[0], 1638 * dps) && near(o.rot[1], 3277 * dps) && near(o.rot[2], 3277 * dps));
    assert(o.ns == t - 3000000);                                 // the kernel's receive time
    // Same transform as a Pro Controller sample with the same raw words (only the scale differs)
    pro_motion ps = {0}; pro_motion_sample po[3]; uint8_t pr0[64] = {0x30, 1};
    for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) { uint16_t n = (uint16_t)(int16_t)((int16_t)(r[15 + 2 * i] | r[16 + 2 * i] << 8)); pr0[13 + 12 * k + 2 * i] = n; pr0[14 + 12 * k + 2 * i] = n >> 8; }
    assert(pro_motion_decode(&ps, pr0, 49, t, po) == 3);
    for (int i = 0; i < 3; i++) { assert(near(o.acc[i], po[2].acc[i])); assert((o.rot[i] > 0) == (po[2].rot[i] > 0)); }
    // Times: implausible kernel stamps fall back to now, and every sample is later than the one before
    assert(imu_motion_decode(pr, &s, 1, r, 34, 0, t + 1000, &o) && o.ns == t + 1000);
    assert(imu_motion_decode(pr, &s, 1, r, 34, t + 5000000, t + 2000, &o) && o.ns == t + 2000);   // ahead of now
    assert(imu_motion_decode(pr, &s, 1, r, 34, 1, t + 3000, &o) && o.ns == t + 3000);              // over 1 s old
    assert(imu_motion_decode(pr, &s, 1, r, 34, t + 500, t + 4000, &o) && o.ns == t + 3001);        // bunched callbacks keep order
    // Live data, then real zeroes (free fall) are a sample; a stale stream must show data again
    memset(r + 15, 0, 12);
    assert(imu_motion_decode(pr, &s, 1, r, 34, t + 10000000, t + 10000000, &o) && o.acc[2] == 0);
    assert(!imu_motion_decode(pr, &s, 1, r, 34, 0, t + 10000000 + IMU_MOTION_STALE_NS + 1, &o));
    word(r, 2, -32768); word(r, 5, -32768);
    assert(imu_motion_decode(pr, &s, 1, r, 64, 0, t + 900000000, &o) && near(o.acc[2], 8.f) && o.rot[2] < 0);
    puts("t_motion_imu: passed");
}
