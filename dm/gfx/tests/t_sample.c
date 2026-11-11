// GLSLC's sample position byte (nvn_sample.h): decoded as each GLSLC reads it, a byte gives back the sample's position. Expected values are worked out by hand.
#include "../nvn/nvn_sample.h"
#include <stdio.h>

static int fail, total;
static void t(const char *name, double want, double got) {
    total++; fail += want != got;
    printf("%-62s expected %+.4f got %+.4f %s\n", name, want, got, want == got ? "ok" : "FAIL");
}
static double nvn2_offset(int nibble) { return ((nibble ^ 8) - 8) / 16.0; }   // NVN2: a signed offset from the centre, in 1/16 pixel
static double nvn1_position(int nibble) { return nibble / 16.0; }   // Switch 1: the position from the corner

int main(void) {
    t("one sample, Switch 1: 0x88", 0x88, nvn_sample_byte(0.5f, 0.5f, 0));
    t("one sample, NVN2: 0x00, no offset from the centre", 0x00, nvn_sample_byte(0.5f, 0.5f, 1));
    // Metal's default positions for 4 samples
    static const float pos[4][2] = {{0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f}};
    for (int i = 0; i < 4; i++) {
        uint8_t b2 = nvn_sample_byte(pos[i][0], pos[i][1], 1), b1 = nvn_sample_byte(pos[i][0], pos[i][1], 0);
        char n[64];
        snprintf(n, sizeof n, "4 samples, sample %d x from the centre, NVN2", i); t(n, pos[i][0] - 0.5, nvn2_offset(b2 & 15));
        snprintf(n, sizeof n, "4 samples, sample %d y from the centre, NVN2", i); t(n, pos[i][1] - 0.5, nvn2_offset(b2 >> 4));
        snprintf(n, sizeof n, "4 samples, sample %d x from the corner, Switch 1", i); t(n, pos[i][0], nvn1_position(b1 & 15));
        snprintf(n, sizeof n, "4 samples, sample %d y from the corner, Switch 1", i); t(n, pos[i][1], nvn1_position(b1 >> 4));
    }
    // GLSLC's gl_SamplePosition on NVN2, ((byte ^ 0x88) & 15) / 16 - 0.5, is the same offset
    t("NVN2 gl_SamplePosition reading of sample 0's x", -0.125, ((nvn_sample_byte(0.375f, 0.125f, 1) ^ 0x88) & 15) / 16.0 - 0.5);
    printf("== %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
