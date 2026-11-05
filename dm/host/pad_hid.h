// Raw HID gamepads that GameController does not list (an 8BitDo in its own mode: a Bluetooth HID gamepad macOS pairs, GitHub issue #637): one device's buttons, hat and axes
// as HID reports them, turned into Switch buttons and sticks. Pure, so tests/t_pad_hid.c checks it without a controller; pad_hid_mac.h reads the devices.
// Buttons by position, as front_mac.m maps GameController (right A, bottom B, top X, left Y). The default map is 8BitDo's D-input numbering (HID button usages 1 A, 2 B, 4 X,
// 5 Y, 7 / 8 LB / RB, 9 / 10 LT / RT, 11 select, 12 start, 14 / 15 stick clicks, by Xbox labels); a device whose buttons come in Nintendo's report 0x3F (Pro Controller clones,
// whatever their vendor) has that report's numbering instead. DM_HID_MAP=A=2,B=1,… replaces entries, DM_HID_LOG=1 logs each usage pressed.
// Sticks: X / Y left, Z / Rz right (Rx / Ry on a device without Z); analog triggers (Brake / Accelerator) press ZL / ZR past half.
// Report 0x3F carries the sticks raw, off centre and short of the axis's range: pad_hid_raw_axis measures them from their rest, against the reach seen so far.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dm_input.h"

#define PAD_HID_DEFAULT_VENDOR 0x2DC8   // 8BitDo, whose numbering the default map is (another vendor's pad gets a log line saying so, pad_hid_mac.h)
#define PAD_HID_NINTENDO_REPORT 0x3F    // the Pro Controller's simple input report: 16 buttons, a hat, four 16-bit axes
#define PAD_HID_RAW_REACH 0.7f          // a raw stick's reach from its rest until it has gone further (the PowerA NANO's reaches 0.75 to 0.81 of the axis)
enum { PH_X, PH_Y, PH_Z, PH_RX, PH_RY, PH_RZ, PH_BRAKE, PH_ACCEL, PH_AXES };
typedef struct {
    uint32_t buttons;          // bit u - 1: HID button usage u held (usages 1..32)
    int hat;                   // -1 centered, 0 up, then clockwise to 7 up-left
    float axis[PH_AXES];       // -1..1 (a trigger rests at -1)
    uint8_t has[PH_AXES];      // the device has the axis
    uint8_t map[16];           // Switch button bit (dm_input.h order) -> HID button usage, 0 none
    uint8_t raw;               // sticks go through pad_hid_raw_axis
    uint8_t rest_seen[PH_AXES];
    float rest[PH_AXES], reach[PH_AXES][2];   // an axis's rest, and its reach below and above it
} pad_hid;

static const char *const pad_hid_names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};

// DM_HID_MAP: comma-separated NAME=usage entries over the default map (an unknown name or a usage past 32 is skipped); nintendo: report 0x3F's numbering is that default
static void pad_hid_init(pad_hid *h, const char *map, int nintendo) {
    static const uint8_t def[16] = {2, 1, 5, 4, 14, 15, 7, 8, 9, 10, 12, 11}, nin[16] = {2, 1, 4, 3, 11, 12, 5, 6, 7, 8, 10, 9};
    memset(h, 0, sizeof *h); h->hat = -1; h->raw = (uint8_t)!!nintendo; memcpy(h->map, nintendo ? nin : def, sizeof def);
    for (int a = 0; a < PH_AXES; a++) h->axis[a] = a >= PH_BRAKE ? -1 : 0;
    for (const char *s = map; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : "") {
        const char *eq = strchr(s, '='), *end = strchr(s, ','); if (!eq || (end && eq > end)) continue;
        int u = atoi(eq + 1);
        for (int b = 0; b < 16; b++) if ((size_t)(eq - s) == strlen(pad_hid_names[b]) && !strncasecmp(s, pad_hid_names[b], eq - s) && u >= 0 && u <= 32) h->map[b] = (uint8_t)u;
    }
}
// The report that shows player p on a Pro Controller's four lights, which a pad runs in a chase until the console gives it a player: subcommand 0x30 of output report 0x01, after neutral
// rumble data, with the console's pattern for each player. It leaves the pad in its current input report.
#define PAD_HID_LIGHTS_SIZE 49
static void pad_hid_lights(uint8_t r[PAD_HID_LIGHTS_SIZE], int p) {
    static const uint8_t neutral[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40}, lights[8] = {0x1, 0x3, 0x7, 0xF, 0x9, 0x5, 0xD, 0x6};
    memset(r, 0, PAD_HID_LIGHTS_SIZE); r[0] = 0x01; memcpy(r + 2, neutral, sizeof neutral); r[10] = 0x30; r[11] = lights[p & 7];
}
static int pad_hid_axis_of(uint32_t page, uint32_t usage) {   // -1: not a gamepad axis
    if (page == 1 && usage >= 0x30 && usage <= 0x35) return PH_X + (int)(usage - 0x30);
    if (page == 2 && usage == 0xC5) return PH_BRAKE;
    if (page == 2 && usage == 0xC4) return PH_ACCEL;
    return -1;
}
static float pad_hid_norm(long v, long min, long max) { return max > min ? (float)(2.0 * (v - min) / (max - min) - 1) : 0; }
// A raw axis (-1..1) measured from its rest, a full deflection reaching 1. The first value is the rest, unless it is too far out to be one (a stick held while the pad connects: the axis's
// middle stands in). The reach only grows, each side on its own.
static float pad_hid_raw_axis(pad_hid *h, int a, float v) {
    if (!h->rest_seen[a]) {
        h->rest_seen[a] = 1; h->rest[a] = fabsf(v) < 0.25f ? v : 0; h->reach[a][0] = h->reach[a][1] = PAD_HID_RAW_REACH;
    }
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

static uint64_t pad_hid_buttons(const pad_hid *h) {
    static const uint64_t hat[8] = {DM_BTN_UP, DM_BTN_UP | DM_BTN_RIGHT, DM_BTN_RIGHT, DM_BTN_RIGHT | DM_BTN_DOWN, DM_BTN_DOWN, DM_BTN_DOWN | DM_BTN_LEFT, DM_BTN_LEFT, DM_BTN_LEFT | DM_BTN_UP};
    uint64_t m = 0;
    for (int b = 0; b < 16; b++) if (h->map[b] && h->buttons >> (h->map[b] - 1) & 1) m |= 1ull << b;
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
