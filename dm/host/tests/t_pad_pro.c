// A Pro Controller's report 0x30 as buttons and sticks (pad_pro.h): each button's bit, the 12-bit sticks, a rest off the middle, the dead zone, a reach that grows, and what is refused;
// then its stick calibration read from SPI flash: the read command, the factory's and the user's, and what is refused
#include <stdio.h>
#include <string.h>
#include "../pad_pro.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void report(uint8_t *r, int lx, int ly, int rx, int ry) {
    memset(r, 0, 49); r[0] = 0x30;
    r[6] = lx & 0xff; r[7] = (lx >> 8 & 0x0f) | (ly & 0x0f) << 4; r[8] = ly >> 4;
    r[9] = rx & 0xff; r[10] = (rx >> 8 & 0x0f) | (ry & 0x0f) << 4; r[11] = ry >> 4;
}

int main(void) {
    uint8_t r[49]; uint64_t m = 99; int32_t st[4] = {0};
    static const struct { int at, bit; uint32_t btn; } each[] = {
        {3, 0x01, DM_BTN_Y}, {3, 0x02, DM_BTN_X}, {3, 0x04, DM_BTN_B}, {3, 0x08, DM_BTN_A}, {3, 0x40, DM_BTN_R}, {3, 0x80, DM_BTN_ZR},
        {4, 0x01, DM_BTN_MINUS}, {4, 0x02, DM_BTN_PLUS}, {4, 0x04, DM_BTN_RS}, {4, 0x08, DM_BTN_LS},
        {5, 0x01, DM_BTN_DOWN}, {5, 0x02, DM_BTN_UP}, {5, 0x04, DM_BTN_RIGHT}, {5, 0x08, DM_BTN_LEFT}, {5, 0x40, DM_BTN_L}, {5, 0x80, DM_BTN_ZL},
    };
    for (size_t i = 0; i < sizeof each / sizeof *each; i++) {
        report(r, 2048, 2048, 2048, 2048); r[each[i].at] = (uint8_t)each[i].bit;
        CHECK(pad_pro_buttons(r) == each[i].btn);
    }
    report(r, 2048, 2048, 2048, 2048); r[3] = 0x30; r[4] = 0xb0; r[5] = 0x30;   // SR, SL, Home, Capture and the grip are no Switch button of a Pro Controller here
    CHECK(pad_pro_buttons(r) == 0);

    pad_pro s = {0};
    report(r, 2000, 2100, 1900, 2200);                                          // the first report is each axis's rest
    CHECK(pad_pro_decode(&s, r, sizeof r, &m, st) == 1);
    CHECK(m == 0 && st[0] == 0 && st[1] == 0 && st[2] == 0 && st[3] == 0);
    CHECK(s.rest[0] == 2000 && s.rest[1] == 2100 && s.rest[2] == 1900 && s.rest[3] == 2200);

    report(r, 2000 + 50, 2100 - 50, 1900, 2200);                                // inside the dead zone
    pad_pro_decode(&s, r, sizeof r, &m, st); CHECK(st[0] == 0 && st[1] == 0);
    report(r, 2000 + 1200, 2100 - 1200, 1900 + 600, 2200);                      // a full reach, each way, and x and y do not mix
    pad_pro_decode(&s, r, sizeof r, &m, st);
    CHECK(st[0] == DM_STICK_MAX); CHECK(st[1] == -DM_STICK_MAX);
    CHECK(st[2] > DM_STICK_MAX * 4 / 10 && st[2] < DM_STICK_MAX * 5 / 10); CHECK(st[3] == 0);
    report(r, 2000 + 1500, 2100, 1900, 2200);                                   // a longer reach is the new full, so the first one is no longer full
    pad_pro_decode(&s, r, sizeof r, &m, st); CHECK(st[0] == DM_STICK_MAX); CHECK(st[1] == 0);
    report(r, 2000 + 1200, 2100, 1900, 2200);
    pad_pro_decode(&s, r, sizeof r, &m, st); CHECK(st[0] > 0 && st[0] < DM_STICK_MAX);

    pad_pro off = {0};
    report(r, 100, 4000, 2048, 2048);                                           // a stick held at the first report: the middle stands in for its rest
    pad_pro_decode(&off, r, sizeof r, &m, st); CHECK(off.rest[0] == PAD_PRO_MIDDLE && off.rest[1] == PAD_PRO_MIDDLE);
    CHECK(st[0] == -DM_STICK_MAX); CHECK(st[1] == DM_STICK_MAX);

    m = 99; st[0] = 1234; report(r, 2048, 2048, 2048, 2048); r[0] = 0x3f;       // another report, or a short one, changes nothing
    CHECK(pad_pro_decode(&s, r, sizeof r, &m, st) == 0); CHECK(m == 99 && st[0] == 1234);
    r[0] = 0x30; CHECK(pad_pro_decode(&s, r, 11, &m, st) == 0); CHECK(m == 99 && st[0] == 1234);

    // the SPI read command: report 0x01, counter, neutral rumble, subcommand 0x10, address little-endian, length
    uint8_t out[16]; CHECK(pad_pro_spi_read(out, 0x13, PAD_PRO_SPI_FACTORY, PAD_PRO_SPI_FACTORY_LEN) == 16);
    CHECK(out[0] == 0x01 && out[1] == 0x03 && out[2] == 0x00 && out[3] == 0x01 && out[10] == 0x10 && out[11] == 0x3d && out[12] == 0x60 && out[13] == 0 && out[15] == 18);

    // a factory calibration: left as above, center, below; right as center, below, above (x, y of 12 bits in three bytes)
    uint8_t rep[64] = {0}; rep[0] = 0x21; rep[13] = 0x90; rep[14] = 0x10; rep[15] = 0x3d; rep[16] = 0x60; rep[19] = 18;
    #define XY(p, x, y) do { (p)[0] = (x) & 0xff; (p)[1] = ((x) >> 8 & 0x0f) | ((y) & 0x0f) << 4; (p)[2] = (y) >> 4; } while (0)
    XY(rep + 20, 1500, 1400); XY(rep + 23, 2010, 2080); XY(rep + 26, 1300, 1350);   // left: above, rest, below
    XY(rep + 29, 1990, 2030); XY(rep + 32, 1250, 1260); XY(rep + 35, 1450, 1460);   // right: rest, below, above
    pad_pro c = {0};
    CHECK(pad_pro_spi_reply(&c, rep, sizeof rep) == 3);
    CHECK(c.cal[0] == 1 && c.cal[1] == 1 && c.seen == 1);
    CHECK(c.rest[0] == 2010 && c.rest[1] == 2080 && c.reach[0][0] == 1300 && c.reach[1][0] == 1350 && c.reach[0][1] == 1500 && c.reach[1][1] == 1400);
    CHECK(c.rest[2] == 1990 && c.rest[3] == 2030 && c.reach[2][0] == 1250 && c.reach[3][0] == 1260 && c.reach[2][1] == 1450 && c.reach[3][1] == 1460);
    report(r, 2010 + 1500, 2080 - 1350, 1990, 2030);                             // a full push each way reads full, at the calibrated reach of that direction
    CHECK(pad_pro_decode(&c, r, sizeof r, &m, st) == 1); CHECK(st[0] == DM_STICK_MAX && st[1] == -DM_STICK_MAX && st[2] == 0 && st[3] == 0);
    CHECK(c.rest[0] == 2010);                                                    // the first 0x30 report no longer sets the rest
    report(r, 2010 + 750, 2080, 1990, 2030); pad_pro_decode(&c, r, sizeof r, &m, st); CHECK(st[0] > DM_STICK_MAX * 4 / 10 && st[0] < DM_STICK_MAX / 2);
    report(r, 2010 + 1900, 2080, 1990, 2030); pad_pro_decode(&c, r, sizeof r, &m, st); CHECK(st[0] == DM_STICK_MAX && c.reach[0][1] == 1500);   // past it: full, reach kept

    // the user's calibration, with its magic, wins over the factory's whatever their order; without the magic it is not taken
    uint8_t u[64] = {0}; u[0] = 0x21; u[13] = 0x90; u[14] = 0x10; u[15] = 0x10; u[16] = 0x80; u[19] = 22;
    u[20] = 0xb2; u[21] = 0xa1; XY(u + 22, 1600, 1600); XY(u + 25, 2000, 2000); XY(u + 28, 1600, 1600);   // left, factory order
    CHECK(pad_pro_spi_reply(&c, u, sizeof u) == 1); CHECK(c.cal[0] == 2 && c.rest[0] == 2000 && c.reach[0][1] == 1600);
    CHECK(pad_pro_spi_reply(&c, rep, sizeof rep) == 2); CHECK(c.rest[0] == 2000 && c.rest[2] == 1990);   // the factory's then sets only the right stick

    // refused: no ack, another subcommand, too short, or values no stick has (an erased flash reads 0xff)
    pad_pro z = {0}; rep[13] = 0x00; CHECK(pad_pro_spi_reply(&z, rep, sizeof rep) == 0); rep[13] = 0x90;
    rep[14] = 0x02; CHECK(pad_pro_spi_reply(&z, rep, sizeof rep) == 0); rep[14] = 0x10;
    CHECK(pad_pro_spi_reply(&z, rep, 30) == 0);
    memset(rep + 20, 0xff, 18); CHECK(pad_pro_spi_reply(&z, rep, sizeof rep) == 0); CHECK(z.cal[0] == 0 && z.seen == 0);

    printf("t_pad_pro: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
