// Xbox One controllers over USB that macOS has no driver for (a PDP Wired Controller, 0e6f:02a8): the Gaming Input Protocol (GIP) they speak, as Linux's xpad driver
// speaks it. Pure, so tests/t_pad_gip.c checks it without a controller; pad_gip_mac.h finds the devices and moves the packets.
// macOS's own Xbox driver (XboxGamepad.dext) matches Microsoft's pads only, by product ID, so another vendor's GIP pad stays unconfigured: nothing powers it on, its light
// stays off and GameController never lists it. A GIP pad sends nothing until the host powers it on; some vendors' pads also want a few more packets first (pad_gip_init).
// Packets: a 4-byte header (command, options, sequence number, payload length) and the payload. The input report (0x20) carries the face buttons, the d-pad, the bumpers,
// the stick clicks, the 10-bit triggers and the four 16-bit stick axes (Y up); the Xbox button comes in a report of its own (0x07), which the pad wants acknowledged.
// Buttons by position, as front_mac.m maps GameController (right A, bottom B, top X, left Y): Xbox A is Switch B, Xbox B Switch A, Xbox X Switch Y, Xbox Y Switch X. Menu is Plus,
// View is Minus, LB / RB are L / R, the triggers press ZL / ZR past half. The sticks take pad_hid.h's round dead zone.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dm_input.h"
#include "pad_hid.h"

enum { GIP_CMD_ACK = 0x01, GIP_CMD_ANNOUNCE = 0x02, GIP_CMD_STATUS = 0x03, GIP_CMD_POWER = 0x05, GIP_CMD_AUTH = 0x06, GIP_CMD_VIRTUAL_KEY = 0x07,
       GIP_CMD_RUMBLE = 0x09, GIP_CMD_LED = 0x0a, GIP_CMD_INPUT = 0x20 };
#define GIP_OPT_ACK 0x10
#define GIP_OPT_INTERNAL 0x20
#define GIP_PACKET_MAX 16
// The USB interface a GIP pad has (vendor-specific class 0xFF, subclass 0x47, protocol 0xD0)
#define GIP_IF_CLASS 0xFF
#define GIP_IF_SUBCLASS 0x47
#define GIP_IF_PROTOCOL 0xD0

typedef struct {
    uint8_t seq;         // the next outgoing sequence number
    uint64_t buttons;    // Switch buttons (dm_input.h), the Xbox button not among them
    int32_t s[4];        // lx, ly, rx, ry
    int guide;           // the Xbox button is held (front_mac.m: Home)
    int heard;           // an input report has come
} pad_gip;

// The packets a pad is sent at its start, in order (xpad's xboxone_init_packets): vid / pid 0 match every vendor / product. The sequence number (byte 2) is the pad's own
typedef struct { uint16_t vid, pid; uint8_t len, p[GIP_PACKET_MAX]; } pad_gip_packet;
static const pad_gip_packet pad_gip_init_packets[] = {
    {0, 0, 5, {GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 1, 0x00}},                                        // power on
    {0x045e, 0x02ea, 5, {GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 1, 0x06}},                              // Xbox One S: start sending input
    {0x045e, 0x0b00, 6, {0x4d, GIP_OPT_ACK, 0, 2, 0x07, 0x00}},                                      // Elite Series 2: report the paddles
    {0x0e6f, 0, 7, {GIP_CMD_LED, GIP_OPT_INTERNAL, 0, 3, 0x00, 0x01, 0x14}},                         // PDP: its light on, without which it sends no input
    {0x0e6f, 0, 6, {GIP_CMD_AUTH, GIP_OPT_INTERNAL, 0, 2, 0x01, 0x00}},                              // PDP: authentication done
    {0x24c6, 0x541a, 13, {GIP_CMD_RUMBLE, 0, 0, 9, 0, 0x0f, 0, 0, 0x1d, 0x1d, 0xff, 0, 0}},          // PowerA: a rumble start and stop, without which it sends no input
    {0x24c6, 0x542a, 13, {GIP_CMD_RUMBLE, 0, 0, 9, 0, 0x0f, 0, 0, 0x1d, 0x1d, 0xff, 0, 0}},
    {0x24c6, 0x543a, 13, {GIP_CMD_RUMBLE, 0, 0, 9, 0, 0x0f, 0, 0, 0x1d, 0x1d, 0xff, 0, 0}},
    {0x24c6, 0x541a, 13, {GIP_CMD_RUMBLE, 0, 0, 9, 0, 0x0f, 0, 0, 0, 0, 0, 0, 0}},
    {0x24c6, 0x542a, 13, {GIP_CMD_RUMBLE, 0, 0, 9, 0, 0x0f, 0, 0, 0, 0, 0, 0, 0}},
    {0x24c6, 0x543a, 13, {GIP_CMD_RUMBLE, 0, 0, 9, 0, 0x0f, 0, 0, 0, 0, 0, 0, 0}},
};
#define PAD_GIP_INIT_N (sizeof pad_gip_init_packets / sizeof pad_gip_init_packets[0])

static void pad_gip_reset(pad_gip *g) { uint8_t seq = g->seq; memset(g, 0, sizeof *g); g->seq = seq; }
// The init packet at or after table index *at for this pad, with its sequence number: its length, and *at past it; 0 when none is left
static size_t pad_gip_init(pad_gip *g, int vid, int pid, size_t *at, uint8_t out[GIP_PACKET_MAX]) {
    for (; *at < PAD_GIP_INIT_N; (*at)++) {
        const pad_gip_packet *k = &pad_gip_init_packets[*at];
        if ((k->vid && k->vid != vid) || (k->pid && k->pid != pid)) continue;
        memcpy(out, k->p, k->len); out[2] = g->seq++; (*at)++;
        return k->len;
    }
    return 0;
}
static float pad_gip_axis(const uint8_t *p) { int16_t v = (int16_t)(p[0] | p[1] << 8); return v < 0 ? v / 32768.0f : v / 32767.0f; }
// One packet from the pad. Returns what the host does next: 1 the input changed (read it), 2 the pad announced itself again (send the init packets again), 0 nothing.
// *ack_len > 0: send ack (the Xbox button's report asks for one)
static int pad_gip_decode(pad_gip *g, const uint8_t *r, size_t n, uint8_t ack[GIP_PACKET_MAX], size_t *ack_len) {
    *ack_len = 0;
    if (n < 4) return 0;
    if (r[0] == GIP_CMD_ANNOUNCE) return 2;
    if (r[0] == GIP_CMD_VIRTUAL_KEY && n >= 5) {
        if (r[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL)) {
            static const uint8_t a[13] = {GIP_CMD_ACK, GIP_OPT_INTERNAL, 0, 9, 0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02};
            memcpy(ack, a, sizeof a); ack[2] = r[2]; *ack_len = sizeof a;
        }
        int was = g->guide; g->guide = r[4] & 3;
        return g->guide != was;
    }
    if (r[0] != GIP_CMD_INPUT || n < 18) return 0;
    static const struct { uint8_t byte, bit; uint64_t to; } map[] = {
        {4, 2, DM_BTN_PLUS}, {4, 3, DM_BTN_MINUS}, {4, 4, DM_BTN_B}, {4, 5, DM_BTN_A}, {4, 6, DM_BTN_Y}, {4, 7, DM_BTN_X},
        {5, 0, DM_BTN_UP}, {5, 1, DM_BTN_DOWN}, {5, 2, DM_BTN_LEFT}, {5, 3, DM_BTN_RIGHT}, {5, 4, DM_BTN_L}, {5, 5, DM_BTN_R}, {5, 6, DM_BTN_LS}, {5, 7, DM_BTN_RS},
    };
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (r[map[i].byte] >> map[i].bit & 1) m |= map[i].to;
    if ((r[6] | r[7] << 8) > 511) m |= DM_BTN_ZL;   // triggers 0..1023
    if ((r[8] | r[9] << 8) > 511) m |= DM_BTN_ZR;
    int32_t s[4];
    pad_hid_stick(pad_gip_axis(r + 10), pad_gip_axis(r + 12), &s[0], &s[1]);
    pad_hid_stick(pad_gip_axis(r + 14), pad_gip_axis(r + 16), &s[2], &s[3]);
    int changed = !g->heard || m != g->buttons || memcmp(s, g->s, sizeof s);
    g->buttons = m; memcpy(g->s, s, sizeof s); g->heard = 1;
    return changed;
}
