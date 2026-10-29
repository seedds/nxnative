// The Switch 2 controller protocol without Bluetooth: framing, advertisement, input report, calibration and HD-rumble packing (dm/ios/Sources/dm_switch2_proto.h).
// Hardware-independent, so it runs on any machine: make -C dm/host/tests t_switch2
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dm_switch2_proto.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

int main(void) {
    // Command frame: cmd, 0x91, shape 0x01, subcommand, 3 zero bytes with the payload length in byte 5, then the payload.
    uint8_t f[32]; uint8_t payload[3] = {0xA7, 0, 0};
    size_t n = ds2_command(f, sizeof f, DS2_CMD_FEATURE, DS2_SUB_FEATURE_ENABLE, payload, 3);
    assert(n == 11);
    assert(f[0] == 0x0C && f[1] == 0x91 && f[2] == 0x01 && f[3] == 0x04 && f[4] == 0 && f[5] == 3 && f[6] == 0 && f[7] == 0);
    assert(f[8] == 0xA7 && f[9] == 0 && f[10] == 0);
    assert(ds2_command(f, 8, DS2_CMD_LEDS, DS2_SUB_LEDS_SET, payload, 3) == 0);   // no room for the payload

    // Memory read payload: len, 0x7E, 0, 0, address little-endian.
    uint8_t m[8]; ds2_memory_payload(m, 0x40, DS2_ADDR_INFO);
    assert(m[0] == 0x40 && m[1] == 0x7E && m[2] == 0 && m[3] == 0);
    assert(m[4] == 0x00 && m[5] == 0x30 && m[6] == 0x01 && m[7] == 0x00);

    // Advertisement after the company id: vendor at 3, product at 5, reconnect host at 10 (little-endian), zero = pairing mode.
    uint8_t adv[16] = {0}; put16(adv + 3, DS2_VENDOR_ID); put16(adv + 5, DS2_PID_PRO_CONTROLLER_2);
    const uint8_t host[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    memcpy(adv + 10, host, 6);
    ds2_advert a;
    assert(ds2_advert_parse(adv, sizeof adv, &a) && a.vid == DS2_VENDOR_ID && a.pid == DS2_PID_PRO_CONTROLLER_2);
    assert(a.host == 0x665544332211ull);
    adv[10] = adv[11] = adv[12] = adv[13] = adv[14] = adv[15] = 0;
    assert(ds2_advert_parse(adv, sizeof adv, &a) && a.host == 0);   // Sync held: pairing mode
    put16(adv + 3, 0x1234);
    assert(!ds2_advert_parse(adv, sizeof adv, &a));                 // not Nintendo
    assert(!ds2_advert_parse(adv, 15, &a));                         // too short

    // Input report: 63 bytes. Buttons at 4, two packed 12-bit stick pairs at 10 and 13, accel at 0x30, gyro at 0x36, battery at 0x1F.
    uint8_t r[63] = {0};
    put32(r, 0x12345678);
    put32(r + 4, DS2_BTN_A | DS2_BTN_ZR | DS2_BTN_PLUS | DS2_BTN_UP | DS2_BTN_L);
    uint32_t ls = ((uint32_t)2048) | ((uint32_t)3000 << 12);
    uint32_t rs = ((uint32_t)1000) | ((uint32_t)2048 << 12);
    r[10] = (uint8_t)ls; r[11] = (uint8_t)(ls >> 8); r[12] = (uint8_t)(ls >> 16);
    r[13] = (uint8_t)rs; r[14] = (uint8_t)(rs >> 8); r[15] = (uint8_t)(rs >> 16);
    put16(r + 0x1F, 3800);
    put16(r + 0x30, 4096); put16(r + 0x32, 0); put16(r + 0x34, -4096);
    put16(r + 0x36, 100); put16(r + 0x38, -200); put16(r + 0x3A, 300);
    ds2_report rep;
    assert(ds2_report_parse(r, sizeof r, &rep));
    assert(rep.timestamp == 0x12345678 && rep.battery_mv == 3800);
    assert(rep.lx == 2048 && rep.ly == 3000 && rep.rx == 1000 && rep.ry == 2048);
    assert(rep.accel[0] == 4096 && rep.accel[2] == -4096 && rep.gyro[1] == -200);
    uint64_t b = ds2_buttons(rep.buttons);
    assert((b & DM_BTN_A) && (b & DM_BTN_ZR) && (b & DM_BTN_PLUS) && (b & DM_BTN_UP) && (b & DM_BTN_L));
    assert(!(b & DM_BTN_B) && !(b & DM_BTN_DOWN));
    assert(!ds2_report_parse(r, 0x3B, &rep));                        // too short
    uint8_t audio[112] = {0x20, 0x20};
    assert(!ds2_report_parse(audio, sizeof audio, &rep));            // headset-audio packet, not an input report

    // Every raw button maps to exactly its NpadButton.
    struct { uint32_t raw; uint64_t bit; } map[] = {
        {DS2_BTN_A, DM_BTN_A}, {DS2_BTN_B, DM_BTN_B}, {DS2_BTN_X, DM_BTN_X}, {DS2_BTN_Y, DM_BTN_Y},
        {DS2_BTN_L, DM_BTN_L}, {DS2_BTN_R, DM_BTN_R}, {DS2_BTN_ZL, DM_BTN_ZL}, {DS2_BTN_ZR, DM_BTN_ZR},
        {DS2_BTN_PLUS, DM_BTN_PLUS}, {DS2_BTN_MINUS, DM_BTN_MINUS}, {DS2_BTN_LSTICK, DM_BTN_LS}, {DS2_BTN_RSTICK, DM_BTN_RS},
        {DS2_BTN_LEFT, DM_BTN_LEFT}, {DS2_BTN_UP, DM_BTN_UP}, {DS2_BTN_RIGHT, DM_BTN_RIGHT}, {DS2_BTN_DOWN, DM_BTN_DOWN},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        uint64_t got = ds2_buttons(map[i].raw);
        assert(got == map[i].bit);
    }
    assert(ds2_buttons(DS2_BTN_HOME | DS2_BTN_CAPTURE | DS2_BTN_C | DS2_BTN_GL | DS2_BTN_GR) == 0);   // no NpadButton / iOS system gesture

    // Calibration block: centre, max, min, each a packed 12-bit pair. Full travel reaches +-1; the deadzone holds at the centre.
    uint8_t cal[11] = {0};
    uint32_t c = 2048u | (2048u << 12), mx = 2048u | (2048u << 12), mn = 2048u | (2048u << 12);
    cal[0] = (uint8_t)c; cal[1] = (uint8_t)(c >> 8); cal[2] = (uint8_t)(c >> 16);
    cal[3] = (uint8_t)mx; cal[4] = (uint8_t)(mx >> 8); cal[5] = (uint8_t)(mx >> 16);
    cal[6] = (uint8_t)mn; cal[7] = (uint8_t)(mn >> 8); cal[8] = (uint8_t)(mn >> 16);
    ds2_cal k; assert(ds2_cal_parse(cal, 11, &k) && k.valid);
    float x, y;
    ds2_cal_apply(&k, 4095, 0, 40, &x, &y);
    assert(fabsf(x - 2047.f / 2048.f) < 1e-6f && fabsf(y + 1.f) < 1e-6f);   // y positive is up, as nn::hid and the touch gamepad use it
    ds2_cal_apply(&k, 2048, 2048, 40, &x, &y);
    assert(x == 0 && y == 0);
    ds2_cal_apply(&k, 2020, 2070, 40, &x, &y);                       // inside the deadzone
    assert(x == 0 && y == 0);
    ds2_cal_apply(NULL, 4095, 4095, 0, &x, &y);                     // no calibration: 12-bit values centred on 2048
    assert(fabsf(x - 2047.f / 2048.f) < 1e-6f && fabsf(y - 2047.f / 2048.f) < 1e-6f);
    uint8_t blank[3] = {0xFF, 0xFF, 0xFF};
    assert(ds2_cal_blank(blank, 3) && !ds2_cal_blank(cal, 11));
    // The two sides use their own ranges: the positive side the max pair, the negative side the min pair.
    uint32_t ax = 1024u | (1024u << 12), an = 3072u | (3072u << 12);
    cal[3] = (uint8_t)ax; cal[4] = (uint8_t)(ax >> 8); cal[5] = (uint8_t)(ax >> 16);
    cal[6] = (uint8_t)an; cal[7] = (uint8_t)(an >> 8); cal[8] = (uint8_t)(an >> 16);
    assert(ds2_cal_parse(cal, 11, &k));
    ds2_cal_apply(&k, 4095, 0, 0, &x, &y);
    assert(fabsf(x - 1.f) < 1e-6f && fabsf(y + 2048.f / 3072.f) < 1e-6f);

    // HD rumble: 0x00, two blocks of 0x50|seq and three identical 5-byte samples; 9-bit Hz and 10-bit amplitude, little-endian.
    ds2_vibration v = {225, 0x3FF, 400, 0x200};
    uint8_t sample[5]; ds2_vibration_pack(&v, sample);
    uint64_t packed = 0;
    for (int i = 0; i < 5; i++) packed |= (uint64_t)sample[i] << (8 * i);
    assert((packed & 0x1FF) == 225 && ((packed >> 10) & 0x3FF) == 0x3FF);
    assert(((packed >> 20) & 0x1FF) == 400 && ((packed >> 30) & 0x3FF) == 0x200);
    uint8_t packet[40];
    size_t pn = ds2_motor_packet(packet, &v, 0x03);
    assert(pn == 33 && packet[0] == 0x00);
    assert(packet[1] == 0x53 && packet[17] == 0x53);
    assert(memcmp(packet + 2, sample, 5) == 0 && memcmp(packet + 7, sample, 5) == 0 && memcmp(packet + 12, sample, 5) == 0);
    assert(memcmp(packet + 18, sample, 5) == 0 && memcmp(packet + 23, sample, 5) == 0 && memcmp(packet + 28, sample, 5) == 0);

    puts("t_switch2: PASS");
    return 0;
}
