// A Joy-Con's HD rumble: the game's VibrationValue as the four bytes output report 0x01 carries for one actuator, which
// pad_joycon_mac.h writes. The tables and the packing are SDL's (src/joystick/hidapi/SDL_hidapi_switch.c), measured from
// dekuNukem's rumble_data_table.md. A Joy-Con's linear resonant actuator takes an amplitude code and, per band, a frequency
// code. SDL sends one fixed pair of frequencies (about 150 Hz for both bands) and drops the game's own; here the game's
// frequency is coded as that table measured it, so the two bands keep the pitch the game gave them (a punch's click and a
// block's thud differ in frequency, not only in amplitude).
#pragma once
#include <math.h>
#include <stdint.h>
#include "hle_haptics.h"

// SDL's fixed low band code: 0x3D, about 150 Hz. Its high band's 0x0074 follows from it ((0x3D - 0x20) * 4), as both bands
// are the one table read at one frequency
#define JOYCON_RUMBLE_LF 0x003Du
#define JOYCON_FREQ_OCTAVE 40.0f   // the code's reference frequency: 40 Hz at code 0, 32 codes an octave (the table's rows)
// The bands' own ranges (dekuNukem's table): the low band 41 to 626 Hz, codes 0x01 to 0x7F; the high band 81.75 to 1252 Hz,
// codes 0x21 to 0x9F, its field four to the step from 0x04 to 0x1FC
#define JOYCON_FREQ_LF_LOW 41.0f
#define JOYCON_FREQ_LF_HIGH 626.29f
#define JOYCON_FREQ_HF_LOW 81.75f
#define JOYCON_FREQ_HF_HIGH 1252.57f

// The 101 amplitudes both bands are measured at, in the units SDL's own 0..65535 already uses. The codes are not stored:
// the high band's is 2 per step (0x00 to 0xC8), and the low band's is 0x40 upwards, one step per two amplitudes with its
// 9th bit on the even ones, which is what those 101 pairs in SDL come to. The last amplitude carries code 0x72.
static const uint16_t joycon_rumble_amp[101] = {
      0,  514,  775,  921, 1096, 1303, 1550, 1843, 2192, 2606,
   3100, 3686, 4383, 5213, 6199, 7372, 7698, 8039, 8395, 8767,
   9155, 9560, 9984, 10426, 10887, 11369, 11873, 12398, 12947, 13520,
   14119, 14744, 15067, 15397, 15734, 16079, 16431, 16790, 17158, 17534,
   17918, 18310, 18711, 19121, 19540, 19967, 20405, 20851, 21308, 21775,
   22251, 22739, 23236, 23745, 24265, 24797, 25340, 25894, 26462, 27041,
   27633, 28238, 28856, 29488, 30134, 30794, 31468, 32157, 32861, 33581,
   34316, 35068, 35836, 36620, 37422, 38242, 39079, 39935, 40809, 41703,
   42616, 43549, 44503, 45477, 46473, 47491, 48531, 49593, 50679, 51789,
   52923, 54082, 55266, 56476, 57713, 58977, 60268, 61588, 62936, 64315,
   65535
};
static inline int joycon_rumble_step(uint16_t amplitude) {   // the first amplitude it does not exceed
    for (int i = 0; i < 101; i++) if (amplitude <= joycon_rumble_amp[i]) return i;
    return 100;
}
// A frequency in Hz as the code the table's row for it holds: thirty-two codes an octave from 40 Hz, so 160 Hz is 0x40 and
// 320 Hz is 0x60, and SDL's 150 Hz comes back as 0x3D. A band has its own range (the two above), and a frequency outside it
// holds at the end, as the console's own encoder does
static inline int joycon_freq_code(float hz, float lowest, float highest) {
    if (!isfinite(hz) || hz < lowest) hz = lowest; else if (hz > highest) hz = highest;
    return (int)lrintf(log2f(hz / JOYCON_FREQ_OCTAVE) * 32.0f);
}
// The code a band is driven at: the game's own, or SDL's fixed one, or a mix of the two in code space, which is one octave
// scale either way. A mix below 1 holds a band nearer the frequency SDL drives both at, where a Joy-Con's actuator is
// loudest: the console corrects a band's amplitude for its frequency with tables of its own, which is not done here
static inline unsigned joycon_freq_mix(float hz, float lowest, float highest, float blend) {
    int game = joycon_freq_code(hz, lowest, highest);
    if (!(blend > 0)) return JOYCON_RUMBLE_LF;
    if (blend >= 1) return (unsigned)game;
    long c = lrintf((float)JOYCON_RUMBLE_LF + ((float)game - (float)JOYCON_RUMBLE_LF) * blend);
    return c < 0x01 ? 0x01u : (unsigned)c;
}
// The high band's frequency field from its code: four a step from 0x04 at 80 Hz to 0x1FC at 1252 Hz, the nine bits its two
// bytes hold (SDL's 150 Hz is 0x0074). A code below the band's first would leave the field at zero, which the table never sends
static inline unsigned joycon_freq_hf(unsigned code) {
    int hf = ((int)code - 0x20) * 4;
    return hf < 0x04 ? 0x04u : (unsigned)hf;
}
// Two amplitudes and two frequencies as the actuator's four bytes. Both amplitudes zero is the neutral value an idle Joy-Con
// needs: not the low band's zero code (00 40), which its firmware reads as a command in a report that carries one
// (joycon_enable_imu). blend: 1 drives each band at the game's frequency, 0 at SDL's fixed pair
static inline void joycon_rumble_bytes(uint8_t out[4], const dm_vibration *v, float gain, float blend) {
    float lo = v ? v->amplitude_low * gain : 0, hi = v ? v->amplitude_high * gain : 0;
    lo = isfinite(lo) ? fmaxf(0, fminf(1, lo)) : 0;
    hi = isfinite(hi) ? fmaxf(0, fminf(1, hi)) : 0;
    if (lo == 0 && hi == 0) {
        out[0] = 0x00; out[1] = 0x01; out[2] = 0x40; out[3] = 0x40;
        return;
    }
    int hs = joycon_rumble_step((uint16_t)lrintf(hi * 65535.0f)), ls = joycon_rumble_step((uint16_t)lrintf(lo * 65535.0f));
    uint16_t hf_amp = (uint16_t)(2 * hs);                                  // the high band's code, one byte
    uint16_t lf_amp = ls == 100 ? 0x0072u : (uint16_t)(0x0040 + ls / 2) | (ls % 2 ? 0x8000u : 0);   // the low band's, nine bits
    unsigned hf = joycon_freq_hf(joycon_freq_mix(v ? v->frequency_high : 0, JOYCON_FREQ_HF_LOW, JOYCON_FREQ_HF_HIGH, blend));
    unsigned lf = joycon_freq_mix(v ? v->frequency_low : 0, JOYCON_FREQ_LF_LOW, JOYCON_FREQ_LF_HIGH, blend);
    out[0] = (uint8_t)(hf & 0xff);                                         // the high band's frequency
    out[1] = (uint8_t)(hf_amp | ((hf >> 8) & 0x01));                       // its amplitude, and the frequency's 9th bit
    out[2] = (uint8_t)(lf | ((lf_amp >> 8) & 0x80));                       // the low band's frequency, and the amplitude's 9th bit
    out[3] = (uint8_t)(lf_amp & 0xff);
}
