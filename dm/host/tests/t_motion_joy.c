// Compare the raw decoder with the complete SDL -> Ryujinx coordinate path.
// References: libsdl-org/SDL src/joystick/hidapi/SDL_hidapi_switch.c,
// NextendoNetwork/Ryujinx-Nextendo (9bdbe65) SDL3JoyCon.cs,
// NpadController.UpdateControllerMotion and MotionInput.Update.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../motion_joy.h"

static void reference_frame(const float raw[3], int right, int accel, float out[3]) {
    // SDL's Switch HIDAPI sensor frame, then standalone horizontal Joy-Con mode.
    float s[3] = {-raw[1], raw[2], -raw[0]};
    if (right) { s[0] = -s[0]; s[1] = -s[1]; }
    float h[3] = {right ? -s[2] : s[2], s[1], right ? s[0] : -s[0]};
    // Ryujinx restores the vertical frame before its common SDL-to-Switch map.
    float v[3] = {right ? h[2] : -h[2], h[1], right ? -h[0] : h[0]};
    out[0] = v[0]; out[1] = -v[2]; out[2] = v[1];
    if (accel) for (int i = 0; i < 3; i++) out[i] = -out[i];
}
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint16_t)v >> 8; }
int main(void) {
    int checks = 0;
    for (int right = 0; right < 2; right++) for (int calibrated = 0; calibrated < 2; calibrated++) {
        uint8_t factory[24] = {0};
        for (int i = 0; i < 3; i++) { put16(factory+6+2*i, 16384); put16(factory+18+2*i, 13371); put16(factory+12+2*i, 10); }
        joy_calibration cal = {0}; assert(joy_calibration_parse(&cal, factory, sizeof factory));
        for (int axis = 0; axis < 3; axis++) {
            uint8_t r[49] = {0x30}; pro_motion state = {0}; pro_motion_sample samples[3];
            for (int k = 0; k < 3; k++) for (int i = 0; i < 3; i++) {
                put16(r+13+12*k+2*i, i == axis ? 1024*(k+1) : 0);
                put16(r+19+12*k+2*i, (calibrated ? 10 : 0) + (i == axis ? 100*(k+1) : 0));
            }
            assert(joy_motion_decode(&state, right, r, sizeof r, 1000000000, samples, calibrated ? &cal : NULL) == 3);
            for (int k = 0; k < 3; k++) {
                float a[3] = {0}, w[3] = {0}, ar[3], wr[3];
                a[axis] = 1024*(k+1)/4096.f;
                w[axis] = 100*(k+1)*(calibrated ? 936.f/(13371-10) : .070f)*(float)M_PI/180.f;
                reference_frame(a, right, 1, ar); reference_frame(w, right, 0, wr);
                for (int i = 0; i < 3; i++) {
                    assert(fabsf(samples[k].acc[i]-ar[i]) < .00001f);
                    assert(fabsf(samples[k].rot[i]-wr[i]) < .00001f); checks += 2;
                }
                assert(samples[k].ns == 990000000ull + (uint64_t)k*5000000); checks++;
            }
        }
    }
    printf("Joy-Con decoder: %d SDL/Ryujinx frame and sample checks passed\n", checks);
    return 0;
}
