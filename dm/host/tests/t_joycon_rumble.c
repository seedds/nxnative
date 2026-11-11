// The Joy-Con encoder (joycon_rumble.h) against dekuNukem's rumble_data_table.md: the code each band's frequency rounds to,
// how the high band's field follows from its code, and the four bytes output report 0x01 carries for one actuator.
#include "joycon_rumble.h"
#include <assert.h>
#include <string.h>

static void bytes(uint8_t out[4], float amp_low, float freq_low, float amp_high, float freq_high, float blend) {
    dm_vibration v = {amp_low, freq_low, amp_high, freq_high};
    joycon_rumble_bytes(out, &v, 1, blend);
}
static int same(const uint8_t b[4], int b0, int b1, int b2, int b3) {
    return b[0] == b0 && b[1] == b1 && b[2] == b2 && b[3] == b3;
}
int main(void) {
    // Rows of the table: the low band's code for a frequency, thirty-two codes an octave from 40 Hz
    static const struct { float hz; int code; } rows[] = {
        {41, 0x01}, {47, 0x07}, {57, 0x10}, {80, 0x20}, {113, 0x30}, {149.93f, 0x3D},
        {160, 0x40}, {226, 0x50}, {320, 0x60}, {453, 0x70}, {626.29f, 0x7F}
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++)
        assert(joycon_freq_code(rows[i].hz, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH) == rows[i].code);
    // The high band carries on past the low band's last code: 640 Hz is 0x80, 1252 Hz 0x9F, its field 0x1FC
    assert(joycon_freq_code(640, JOYCON_FREQ_HF_LOW, JOYCON_FREQ_HF_HIGH) == 0x80);
    assert(joycon_freq_code(JOYCON_FREQ_HF_HIGH, JOYCON_FREQ_HF_LOW, JOYCON_FREQ_HF_HIGH) == 0x9F);
    assert(joycon_freq_hf(0x9F) == 0x1FC);
    assert(joycon_freq_hf(0x3D) == 0x74);   // SDL's high band
    assert(joycon_freq_hf(0x21) == 0x04);   // the band's first step, 80 Hz
    // A frequency outside a band holds at its end, as the console's own encoder does
    assert(joycon_freq_code(20, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH) == 0x01);
    assert(joycon_freq_code(3000, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH) == 0x7F);
    assert(joycon_freq_code(3000, JOYCON_FREQ_HF_LOW, JOYCON_FREQ_HF_HIGH) == 0x9F);

    uint8_t b[4];
    // Idle: the neutral value an un-driven actuator needs
    bytes(b, 0, 160, 0, 320, 1);
    assert(same(b, 0x00, 0x01, 0x40, 0x40));
    // Full strength at the game's own 160 and 320 Hz, then at SDL's fixed pair (the value before the frequency was passed on)
    bytes(b, 1, 160, 1, 320, 1);
    assert(same(b, 0x00, 0xC9, 0x40, 0x72));
    bytes(b, 1, 160, 1, 320, 0);
    assert(same(b, 0x74, 0xC8, 0x3D, 0x72));
    // A low band at 62 Hz with a high band past the low band's range at 1252 Hz
    bytes(b, 1, 62, 1, 1252, 1);
    assert(same(b, 0xFC, 0xC9, 0x14, 0x72));
    // A single band: the other's amplitude is zero and its field still carries its frequency
    bytes(b, 1, 160, 0, 320, 1);
    assert(same(b, 0x00, 0x01, 0x40, 0x72));

    // Throughout the amplitude table the bands' frequencies stay where they were put: the amplitude's ninth bit sits in the
    // low band's frequency byte and the high band's in its amplitude byte, and neither disturbs the codes
    for (int step = 1; step <= 100; step++) {
        float a = (float)joycon_rumble_amp[step] / 65535.0f;
        bytes(b, a, 160, a, 320, 1);
        assert((b[2] & 0x7F) == 0x40);   // 160 Hz's code, with the amplitude's ninth bit above it
        assert((b[1] & 0x01) == 0x01);   // 320 Hz's field is 0x100, whose ninth bit lands here
        assert((b[1] & 0xFE) % 2 == 0);  // the amplitude code itself, even as the table has it
    }
    // A mix between the two sits between them, and the ends are the two fixed answers
    unsigned fixed = joycon_freq_mix(320, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH, 0);
    unsigned game = joycon_freq_mix(320, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH, 1);
    unsigned half = joycon_freq_mix(320, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH, 0.5f);
    assert(fixed == JOYCON_RUMBLE_LF && game == 0x60 && half > fixed && half < game);
    return 0;
}
