// A Joy-Con's IMU reports (joycon_imu.h) from synthetic 0x30 reports: three frames per report 5 ms apart, counts to G and rad/s, the
// right unit's mirrored frame, and the filters that keep a repeat, an empty stream or a stale gap from being taken as motion. No
// controller is needed.
#include "joycon_imu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *b, int16_t v) { b[0] = (uint8_t)((uint16_t)v & 0xff); b[1] = (uint8_t)((uint16_t)v >> 8); }

// A 0x30 report with the same IMU frame three times, as a unit at rest or turning sends it
static size_t report(uint8_t *r, uint8_t counter, int16_t ax, int16_t ay, int16_t az, int16_t gx, int16_t gy, int16_t gz) {
    memset(r, 0, 64);
    r[0] = 0x30; r[1] = counter;
    for (int k = 0; k < 3; k++) {
        uint8_t *b = r + 13 + 12 * k;
        put16(b + 0, ax); put16(b + 2, ay); put16(b + 4, az);
        put16(b + 6, gx); put16(b + 8, gy); put16(b + 10, gz);
    }
    return 64;
}

int main(void) {
    uint8_t r[64]; jc_imu_sample s[3];
    jc_imu right = {.right = 1}, left = {.right = 0};
    const uint64_t t0 = 1000000000ull;   // 1 s in, so the sample timestamps are positive

    // Flat and still: raw z = -1 G in counts, everything else 0 -> gravity (0, 0, -1) and no rotation
    size_t n = report(r, 1, 0, 0, -4096, 0, 0, 0);
    assert(joycon_imu_decode(&right, r, n, t0, s) == 3);
    for (int k = 0; k < 3; k++) {
        assert(s[k].acc[0] == 0 && s[k].acc[1] == 0 && s[k].acc[2] == -1);
        assert(s[k].rot[0] == 0 && s[k].rot[1] == 0 && s[k].rot[2] == 0);
        assert(s[k].ns == t0 - (uint64_t)(2 - k) * JC_IMU_PERIOD_NS);   // oldest first, 5 ms apart
    }

    // A full turn per second about the raw x axis, and +1 G on raw y: counts -> rad/s and the right unit's mirror
    n = report(r, 2, 0, 4096, 0, 13371, 0, 0);
    assert(joycon_imu_decode(&right, r, n, t0 + JC_IMU_PERIOD_NS, s) == 3);
    assert(s[2].rot[0] > 6.28f && s[2].rot[0] < 6.29f);   // 13371 counts/s = 1 rev/s = 2*pi rad/s
    assert(s[2].acc[1] == 1);                             // +rawY is +1 G on the right unit (y unchanged by the mirror? no: see below)

    // The same report on a left unit: its frame is not mirrored, so the same counts come out with the opposite sign on y and z
    left.received = 0; left.counter = 0;
    assert(joycon_imu_decode(&left, r, n, t0 + JC_IMU_PERIOD_NS, s) == 3);
    assert(s[2].acc[1] == -1);
    assert(s[2].rot[0] > 6.28f);

    // A repeat of the same counter is the same three samples, not another three
    right.received = 0; right.counter = 0;
    assert(joycon_imu_decode(&right, r, n, t0, s) == 3);
    assert(joycon_imu_decode(&right, r, n, t0 + 1000, s) == 0);

    // An enabled but empty stream is not a source (zeros are "sensor off", a unit at rest still reads about 1 G)
    jc_imu empty = {.right = 1};
    n = report(r, 9, 0, 0, 0, 0, 0, 0);
    assert(joycon_imu_decode(&empty, r, n, t0, s) == 0);
    assert(!empty.live);
    n = report(r, 10, 0, 0, -4096, 0, 0, 0);
    assert(joycon_imu_decode(&empty, r, n, t0 + JC_IMU_PERIOD_NS, s) == 3);
    assert(empty.live);

    // A gap longer than the stale window starts the stream again rather than integrating across it
    n = report(r, 11, 0, 0, -4096, 0, 0, 0);
    assert(joycon_imu_decode(&empty, r, n, t0 + JC_IMU_STALE_NS + 2 * JC_IMU_PERIOD_NS, s) == 3);

    // Not an IMU report, or one too short: nothing
    jc_imu other = {.right = 1};
    n = report(r, 12, 0, 0, -4096, 0, 0, 0);
    r[0] = 0x21;
    assert(joycon_imu_decode(&other, r, n, t0, s) == 0);
    r[0] = 0x30;
    assert(joycon_imu_decode(&other, r, 20, t0, s) == 0);
    printf("t_joycon_imu OK\n");
    return 0;
}
