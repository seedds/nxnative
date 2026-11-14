// Xbox One controllers over USB (pad_gip.h): the init packets per vendor, the input report's buttons, triggers and sticks, the Xbox button and its acknowledgement
#include <stdio.h>
#include "../pad_gip.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int init_count(pad_gip *g, int vid, int pid, uint8_t first[GIP_PACKET_MAX]) {
    size_t at = 0, n; int k = 0; uint8_t out[GIP_PACKET_MAX];
    while ((n = pad_gip_init(g, vid, pid, &at, out))) { if (!k) memcpy(first, out, n); k++; }
    return k;
}

int main(void) {
    pad_gip g = {0}; uint8_t first[GIP_PACKET_MAX], ack[GIP_PACKET_MAX]; size_t ack_len;
    CHECK(init_count(&g, 0x0e6f, 0x02a8, first) == 3);                       // PDP: power on, light on, authentication
    CHECK(first[0] == GIP_CMD_POWER && first[2] == 0 && g.seq == 3);         // sequence numbers count up
    CHECK(init_count(&g, 0x045e, 0x02ea, first) == 2 && first[2] == 3);      // Xbox One S: power on and its start packet
    CHECK(init_count(&g, 0x24c6, 0x541a, first) == 3);                       // PowerA: power on and a rumble start and stop
    CHECK(init_count(&g, 0x1234, 0x5678, first) == 1);                       // any other vendor: power on only

    // A PDP Wired Controller's first report at rest (0e6f:02a8): no button, sticks inside the dead zone
    const uint8_t rest[18] = {0x20, 0x00, 0x03, 0x0e, 0, 0, 0, 0, 0, 0, 0xdb, 0x01, 0x7a, 0x01, 0x73, 0x05, 0x1a, 0xfd};
    pad_gip_reset(&g);
    CHECK(pad_gip_decode(&g, rest, sizeof rest, ack, &ack_len) == 1 && !ack_len);   // the first report counts as a change
    CHECK(g.heard && g.buttons == 0 && !g.s[0] && !g.s[1] && !g.s[2] && !g.s[3]);
    CHECK(pad_gip_decode(&g, rest, sizeof rest, ack, &ack_len) == 0);              // the same again: nothing to read
    uint8_t r[18]; memcpy(r, rest, sizeof r);
    r[4] = 1 << 4; CHECK(pad_gip_decode(&g, r, sizeof r, ack, &ack_len) == 1 && g.buttons == DM_BTN_B);   // Xbox A (bottom) is Switch B
    r[4] = 1 << 5; pad_gip_decode(&g, r, sizeof r, ack, &ack_len); CHECK(g.buttons == DM_BTN_A);           // Xbox B (right) is Switch A
    r[4] = 1 << 6 | 1 << 7; pad_gip_decode(&g, r, sizeof r, ack, &ack_len); CHECK(g.buttons == (DM_BTN_Y | DM_BTN_X));
    r[4] = 1 << 2 | 1 << 3; pad_gip_decode(&g, r, sizeof r, ack, &ack_len); CHECK(g.buttons == (DM_BTN_PLUS | DM_BTN_MINUS));
    r[4] = 0; r[5] = 0xff; pad_gip_decode(&g, r, sizeof r, ack, &ack_len);
    CHECK(g.buttons == (DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_L | DM_BTN_R | DM_BTN_LS | DM_BTN_RS));
    r[5] = 0; r[6] = 0x00; r[7] = 0x02; r[8] = 0xff; r[9] = 0x01; pad_gip_decode(&g, r, sizeof r, ack, &ack_len);
    CHECK(g.buttons == DM_BTN_ZL);                                           // 512 presses, 511 does not
    r[6] = r[7] = r[8] = r[9] = 0;
    r[10] = 0xff; r[11] = 0x7f; r[12] = 0x00; r[13] = 0x80; r[14] = 0x00; r[15] = 0x80; r[16] = 0xff; r[17] = 0x7f;
    pad_gip_decode(&g, r, sizeof r, ack, &ack_len);
    CHECK(g.s[0] == DM_STICK_MAX && g.s[1] == -DM_STICK_MAX && g.s[2] == -DM_STICK_MAX && g.s[3] == DM_STICK_MAX);   // Y is up already
    CHECK(pad_gip_decode(&g, r, 10, ack, &ack_len) == 0);                    // a short input report is skipped

    // The Xbox button: its own report, acknowledged when the pad asks, with the pad's sequence number
    const uint8_t guide[6] = {GIP_CMD_VIRTUAL_KEY, GIP_OPT_ACK | GIP_OPT_INTERNAL, 0x2a, 2, 0x01, 0x5b};
    CHECK(pad_gip_decode(&g, guide, sizeof guide, ack, &ack_len) == 1 && g.guide);
    CHECK(ack_len == 13 && ack[0] == GIP_CMD_ACK && ack[2] == 0x2a && ack[5] == GIP_CMD_VIRTUAL_KEY);
    const uint8_t up[6] = {GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x2b, 2, 0x00, 0x5b};
    CHECK(pad_gip_decode(&g, up, sizeof up, ack, &ack_len) == 1 && !g.guide && !ack_len);
    CHECK(!(g.buttons & ~(uint64_t)0xffff));                                 // the Xbox button is no Switch button

    const uint8_t announce[4] = {GIP_CMD_ANNOUNCE, GIP_OPT_INTERNAL, 0x01, 0x1c};
    CHECK(pad_gip_decode(&g, announce, sizeof announce, ack, &ack_len) == 2);   // announced again: init again
    const uint8_t status[8] = {GIP_CMD_STATUS, GIP_OPT_INTERNAL, 0x05, 0x04, 0x80, 0, 0, 0};
    CHECK(pad_gip_decode(&g, status, sizeof status, ack, &ack_len) == 0);       // its heartbeat: nothing to do
    pad_gip_reset(&g); CHECK(!g.heard && !g.buttons && g.seq == 9);           // a reset keeps the sequence going

    printf("t_pad_gip: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
