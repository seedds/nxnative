// Raw HID gamepads that GameController does not list (an 8BitDo in its own mode: a Bluetooth HID gamepad macOS pairs, GitHub issue #637): one device's buttons, hat and axes
// as HID reports them, turned into Switch buttons and sticks. Pure, so tests/t_pad_hid.c checks it without a controller; pad_hid_mac.h reads the devices.
// Buttons by position, as front_mac.m maps GameController (right A, bottom B, top X, left Y). The default map is 8BitDo's D-input numbering (HID button usages 1 A, 2 B, 4 X,
// 5 Y, 7 / 8 LB / RB, 9 / 10 LT / RT, 11 select, 12 start, 14 / 15 stick clicks, by Xbox labels); DM_HID_MAP=A=2,B=1,… replaces entries, DM_HID_LOG=1 logs each usage pressed.
// Sticks: X / Y left, Z / Rz right (Rx / Ry on a device without Z); analog triggers (Brake / Accelerator) press ZL / ZR past half.
// Nintendo's report descriptor (a Pro Controller clone of another vendor, such as a PowerA NANO, GitHub PR #989) is told by its reports, not its name or vendor (pad_hid_nin_bit):
// a pad that declares the console's protocol is driven by it (pad_hid_mac.h: the handshake into report 0x30, read as pad_pro.h reads a Pro Controller); its simple report 0x3F,
// which such a pad sends until then or when it does not answer, has its own button numbering (pad_hid_init's nintendo) and carries the sticks raw (pad_hid_raw_axis).
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dm_input.h"

#define PAD_HID_DEFAULT_VENDOR 0x2DC8   // 8BitDo, whose numbering the default map is (another vendor's pad gets a log line saying so, pad_hid_mac.h)
enum { PH_X, PH_Y, PH_Z, PH_RX, PH_RY, PH_RZ, PH_BRAKE, PH_ACCEL, PH_AXES };
typedef struct {
    uint32_t buttons;          // bit u - 1: HID button usage u held (usages 1..32)
    int hat;                   // -1 centered, 0 up, then clockwise to 7 up-left
    float axis[PH_AXES];       // -1..1 (a trigger rests at -1)
    uint8_t has[PH_AXES];      // the device has the axis
    uint8_t map[16];           // Switch button bit (dm_input.h order) -> HID button usage, 0 none
    uint8_t raw;               // sticks go through pad_hid_raw_axis (report 0x3F)
    uint8_t rest_seen[PH_AXES];
    float rest[PH_AXES], reach[PH_AXES][2];   // a raw axis's rest, and its reach below and above it
} pad_hid;

// What a report descriptor's element says of Nintendo's protocol (dekuNukem/Nintendo_Switch_Reverse_Engineering, bluetooth_hid_notes.md). output: an output element; id: its
// report; page: its usage page. A pad declares the protocol (PAD_NIN_PROTOCOL) with all four of: buttons in the simple input report 0x3F, the subcommand reply 0x21, the full
// report 0x30, and the subcommand output report 0x01; the rumble output report 0x10 is apart
enum { PAD_NIN_SIMPLE = 1, PAD_NIN_REPLY = 2, PAD_NIN_FULL = 4, PAD_NIN_SUBCMD = 8, PAD_NIN_RUMBLE = 16, PAD_NIN_PROTOCOL = 15 };
#define PAD_HID_NIN_REPORT 0x3F   // the Pro Controller's simple input report: 16 buttons, a hat, four 16-bit axes
static inline int pad_hid_nin_bit(int output, uint32_t id, uint32_t page) {
    if (output) return id == 0x01 ? PAD_NIN_SUBCMD : id == 0x10 ? PAD_NIN_RUMBLE : 0;
    return id == PAD_HID_NIN_REPORT && page == 9 ? PAD_NIN_SIMPLE : id == 0x21 ? PAD_NIN_REPLY : id == 0x30 ? PAD_NIN_FULL : 0;
}
static inline int pad_hid_nin_protocol(int bits) { return (bits & PAD_NIN_PROTOCOL) == PAD_NIN_PROTOCOL; }
// Player lights need the subcommand channel, not full input mode or an answer to device info. Some clones implement the lights alone.
static inline int pad_hid_nin_lights(int bits) { int need = PAD_NIN_SIMPLE | PAD_NIN_REPLY | PAD_NIN_SUBCMD; return (bits & need) == need; }

static const char *const pad_hid_names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
static const uint8_t pad_hid_nin_map[16] = {2, 1, 4, 3, 11, 12, 5, 6, 7, 8, 10, 9};

// Identical pads (front_mac.m, DM_PAD_IDENTICAL): whether every pad of one model is read as a player of its own, instead of through GameController.
// n: the pads of the model IOKit has; listed: GameController's controllers of it; read: whether they are read that way already; due: a wait after the last change is over.
// 1 read them, 0 do not, -1 not yet: a pad connected a moment ago may simply not be listed yet, so a model starts only after the wait, and stops at once
static inline int pad_dupes_want(int n, int listed, int read, int due) { return n < 2 || n <= listed ? 0 : read || due ? 1 : -1; }

// DM_HID_MAP: comma-separated NAME=usage entries over the default map (an unknown name or a usage past 32 is skipped). nintendo: the buttons come in report 0x3F, whose
// numbering is the default instead (usages 1 B, 2 A, 3 Y, 4 X, 5 / 6 L / R, 7 / 8 ZL / ZR, 9 minus, 10 plus, 11 / 12 stick clicks, 13 Home, 14 Capture), and the sticks are raw
static void pad_hid_init(pad_hid *h, const char *map, int nintendo) {
    static const uint8_t def[16] = {2, 1, 5, 4, 14, 15, 7, 8, 9, 10, 12, 11};
    memset(h, 0, sizeof *h); h->hat = -1; h->raw = (uint8_t)!!nintendo; memcpy(h->map, nintendo ? pad_hid_nin_map : def, sizeof def);
    for (int a = 0; a < PH_AXES; a++) h->axis[a] = a >= PH_BRAKE ? -1 : 0;
    for (const char *s = map; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : "") {
        const char *eq = strchr(s, '='), *end = strchr(s, ','); if (!eq || (end && eq > end)) continue;
        int u = atoi(eq + 1);
        for (int b = 0; b < 16; b++) if ((size_t)(eq - s) == strlen(pad_hid_names[b]) && !strncasecmp(s, pad_hid_names[b], eq - s) && u >= 0 && u <= 32) h->map[b] = (uint8_t)u;
    }
}
static int pad_hid_axis_of(uint32_t page, uint32_t usage) {   // -1: not a gamepad axis
    if (page == 1 && usage >= 0x30 && usage <= 0x35) return PH_X + (int)(usage - 0x30);
    if (page == 2 && usage == 0xC5) return PH_BRAKE;
    if (page == 2 && usage == 0xC4) return PH_ACCEL;
    return -1;
}
static float pad_hid_norm(long v, long min, long max) { return max > min ? (float)(2.0 * (v - min) / (max - min) - 1) : 0; }
// A raw axis (-1..1, report 0x3F) measured from its rest, a full deflection reaching 1: report 0x3F carries the sticks uncalibrated, off centre and short of the axis's range
// (a PowerA NANO's rest 9 and 11% off centre, its reach about 0.78 of the axis from there: a full push read 61 to 74%). The first value is the rest, unless it is too far out to
// be one (a stick held while the pad connects: the axis's middle stands in). Each side's reach starts at PAD_HID_RAW_REACH and only grows. Report 0x30 has the pad's own
// calibration instead (pad_pro.h)
#define PAD_HID_RAW_REACH 0.7f
static float pad_hid_raw_axis(pad_hid *h, int a, float v) {
    if (!h->rest_seen[a]) { h->rest_seen[a] = 1; h->rest[a] = fabsf(v) < 0.25f ? v : 0; h->reach[a][0] = h->reach[a][1] = PAD_HID_RAW_REACH; }
    float d = v - h->rest[a]; int side = d > 0;
    if (fabsf(d) > h->reach[a][side]) h->reach[a][side] = fabsf(d);
    return d / h->reach[a][side];
}
static int pad_hid_hat(long v, long min, long max) { return v >= min && v <= max && v - min < 8 ? (int)(v - min) : -1; }   // past the 8 directions: centered
// One stick: a round dead zone against drift (PAD_HID_DEAD of full deflection), and past it the deflection scaled back to start at 0, so the stick neither jumps to the dead zone's
// edge nor loses a small x while y is deflected. Each axis is clamped after the scale: a square-gated stick still reaches full x and y in its corners.
#define PAD_HID_DEAD 0.12f
static void pad_hid_stick(float x, float y, int32_t *ox, int32_t *oy) {
    float r = sqrtf(x * x + y * y);
    if (r <= PAD_HID_DEAD) { *ox = *oy = 0; return; }
    float k = (r - PAD_HID_DEAD) / (1 - PAD_HID_DEAD) / r; x *= k; y *= k;
    *ox = (int32_t)((x < -1 ? -1 : x > 1 ? 1 : x) * DM_STICK_MAX); *oy = (int32_t)((y < -1 ? -1 : y > 1 ? 1 : y) * DM_STICK_MAX);
}

static uint64_t pad_hid_map_buttons(const pad_hid *h, uint32_t buttons) {
    uint64_t m = 0;
    for (int b = 0; b < 16; b++) if (h->map[b] && buttons >> (h->map[b] - 1) & 1) m |= 1ull << b;
    return m;
}
// Full report buttons, decoded by pad_pro.h, use the same HID usages and DM_HID_MAP as report 0x3F. system is report byte 4 (Home and Capture).
// Its d-pad remains a hat; stale simple-report axes and hat values never enter the full report's state.
static uint64_t pad_hid_nin_buttons(const pad_hid *h, uint64_t buttons, uint8_t system) {
    uint32_t usages = 0;
    for (int b = 0; b < 12; b++) if (buttons >> b & 1) usages |= 1u << (pad_hid_nin_map[b] - 1);
    if (system & 0x10) usages |= 1u << 12;
    if (system & 0x20) usages |= 1u << 13;
    return pad_hid_map_buttons(h, usages) | (buttons & (DM_BTN_LEFT | DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN));
}
static uint64_t pad_hid_buttons(const pad_hid *h) {
    static const uint64_t hat[8] = {DM_BTN_UP, DM_BTN_UP | DM_BTN_RIGHT, DM_BTN_RIGHT, DM_BTN_RIGHT | DM_BTN_DOWN, DM_BTN_DOWN, DM_BTN_DOWN | DM_BTN_LEFT, DM_BTN_LEFT | DM_BTN_UP};
    uint64_t m = pad_hid_map_buttons(h, h->buttons);
    if (h->hat >= 0 && h->hat < 8) m |= hat[h->hat];
    if (h->has[PH_BRAKE] && h->axis[PH_BRAKE] > 0) m |= DM_BTN_ZL;
    if (h->has[PH_ACCEL] && h->axis[PH_ACCEL] > 0) m |= DM_BTN_ZR;
    return m;
}
static void pad_hid_sticks(const pad_hid *h, int32_t s[4]) {   // lx, ly, rx, ry; HID's Y grows downward, the Switch's upward
    int z = h->has[PH_Z];
    pad_hid_stick(h->axis[PH_X], -h->axis[PH_Y], &s[0], &s[1]);
    pad_hid_stick(h->axis[z ? PH_Z : PH_RX], -h->axis[z ? PH_RZ : PH_RY], &s[2], &s[3]);
}
