#include "../motion_8bitdo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }
static void word(uint8_t *r, int i, int v) { uint16_t n = (uint16_t)v; r[15 + 2 * i] = n; r[16 + 2 * i] = n >> 8; }
int main(void) {
    // A report from an Ultimate 2 Wireless (firmware 1.03+) lying face up on a desk, over Bluetooth on macOS 26.7
    const uint8_t flat[34] = {0x01, 0x0f, 0x7f, 0x7f, 0x7f, 0x7f, 0, 0, 0, 0, 0, 0, 0x12, 0x60, 0x49,
                              0x6b, 0x01, 0x63, 0x03, 0x37, 0x10, 0xf8, 0xff, 0x18, 0x00, 0xf4, 0xff};
    float acc[3], rot[3];
    assert(abitdo_motion_decode(flat, sizeof flat, acc, rot));
    assert(near(acc[0], 0.21167f) && near(acc[1], -0.08862f) && near(acc[2], -1.01343f));   // gravity (0, 0, -1) face up
    assert(near(rot[0], -0.025566f) && near(rot[1], -0.008522f) && near(rot[2], -0.012783f));
    assert(!abitdo_motion_decode(flat, 12, acc, rot));   // firmware 1.02's 12-byte report has no sensors
    uint8_t r[34] = {0x03};
    assert(!abitdo_motion_decode(r, sizeof r, acc, rot));   // not report 0x01
    r[0] = 0x01;
    word(r, 0, 4096); assert(abitdo_motion_decode(r, sizeof r, acc, rot) && near(acc[1], -1) && near(acc[0], 0)); word(r, 0, 0);   // raw x: toward the shoulders
    word(r, 1, 4096); assert(abitdo_motion_decode(r, sizeof r, acc, rot) && near(acc[0], 1) && near(acc[1], 0)); word(r, 1, 0);    // raw y: left
    word(r, 4, 32767); assert(abitdo_motion_decode(r, sizeof r, acc, rot) && near(rot[0], -2000.f * (float)M_PI / 180.f)); word(r, 4, 0);   // full scale 2000 deg/s
    word(r, 5, -32767); assert(abitdo_motion_decode(r, sizeof r, acc, rot) && near(rot[2], -2000.f * (float)M_PI / 180.f));
    puts("t_motion_8bitdo: ok");
    return 0;
}
