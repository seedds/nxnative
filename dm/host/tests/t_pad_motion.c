#include "../pad_motion.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    pad_motion_state m = {0}; float acc[3]={0,0,-1}, gyro[3]={0,0,0.25f};
    assert(pad_motion_update(&m, 65000, 32, acc, gyro));
    assert(!pad_motion_update(&m, 65000, 32, acc, gyro));
    assert(pad_motion_update(&m, 2464, 32, acc, gyro)); // 16-bit wrap, 96 ms
    assert(fabsf(m.value.angle[2] - 0.024f) < 1e-5f);
    assert(fabsf(m.value.dir[0] - cosf(0.024f*2*M_PI)) < 1e-5f);
    assert(fabsf(m.value.dir[1] - sinf(0.024f*2*M_PI)) < 1e-5f);
    float angle = m.value.angle[2]; pad_motion_update(&m, 50000, 32, acc, gyro); // gap: no invented motion
    assert(m.value.angle[2] == angle);
    pad_motion_reset(&m); assert(!m.have_tick);
    pad_motion_update(&m, 0xfffffff0, 1, acc, gyro); pad_motion_update(&m, 984, 1, acc, gyro);
    assert(fabsf(m.value.angle[2] - 0.00025f) < 1e-6f);
    gyro[1] = NAN; assert(!pad_motion_update(&m, 1984, 1, acc, gyro));
    puts("motion timestamp wrap, units, orientation and stale-gap handling: OK");
}
