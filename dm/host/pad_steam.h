// Valve's first Steam Controller (2015, the one before the 2026 controller), read from its own reports, as Linux's hid-steam driver and SDL's HIDAPI driver read it. Pure,
// so tests/t_pad_steam.c checks it without a controller; pad_hid_mac.h finds the devices, sends the commands and hands the state to the players.
// macOS lists it as a keyboard, a mouse and a vendor interface (usage page 0xFF00), and GameController does not support it. It starts in "lizard mode": its firmware moves
// the mouse with the right pad and types keys with some buttons. Its vendor interface sends 64-byte input reports without a report ID: version (1, 16-bit), type, length,
// then the type's payload. Type 1 is the controller's state, sent when something changes: a sequence number (bytes 4..7), 24 button bits (8..10, PAD_STEAM_*), the analog
// triggers (11 left, 12 right, 0..255), the left X / Y (16..19: the joystick's, or the left pad's while PAD_STEAM_LPAD_TOUCH is set), the right pad's X / Y (20..23), signed
// 16-bit with up positive, as on the Switch. Type 3 is a wireless receiver's event at byte 4 (1 disconnected, 2 connected), type 4 the battery, once a second.
// Commands are 64-byte feature reports, report ID 0: the command, the length of its data, its data. Gamepad mode turns the keys and the mouse off: clear the digital
// mappings, then settings (three bytes each: the setting, its value low, high) with both pads in mode 7 (none). Lizard mode back: the default digital mappings, the
// default settings, the right pad as the mouse (SDL's CloseSteamController).
// Switch buttons by position, as front_mac.m maps an Xbox pad: right A, bottom B, top X, left Y. The left pad clicked in a direction is the d-pad (the firmware's direction
// bits, which it sets only while the pad is clicked, in mode 7 too: seen on a wired controller; the click is checked as well), the right pad touched is the right stick (where the thumb is, a round dead zone at its centre), its click RS. The grips and the Steam
// button have no Switch button. Not read: the gyro (off unless the settings turn it on), BLE (the Bluetooth firmware's 28de:1106 speaks another report format).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"
#include "pad_hid.h"   // pad_hid_stick: the raw HID pads' round dead zone

#define PAD_STEAM_VID 0x28DE
#define PAD_STEAM_PID_WIRED 0x1102      // over USB
#define PAD_STEAM_PID_RECEIVER 0x1142   // the wireless receiver: an interface of usage page 0xFF00 per controller it pairs
#define PAD_STEAM_PAGE 0xFF00
#define PAD_STEAM_LEN 64
enum { PAD_STEAM_MSG_STATE = 0x01, PAD_STEAM_MSG_WIRELESS = 0x03, PAD_STEAM_MSG_STATUS = 0x04 };
enum {
    PAD_STEAM_CMD_CLEAR_MAPPINGS = 0x81, PAD_STEAM_CMD_DEFAULT_MAPPINGS = 0x85, PAD_STEAM_CMD_SETTINGS = 0x87, PAD_STEAM_CMD_DEFAULT_SETTINGS = 0x8E,
    PAD_STEAM_CMD_WIRELESS_STATE = 0xB4,   // a receiver answers with a type 3 event: whether a controller is connected
};
enum { PAD_STEAM_SET_LPAD_MODE = 7, PAD_STEAM_SET_RPAD_MODE = 8, PAD_STEAM_SET_SMOOTH_MOUSE = 24, PAD_STEAM_SET_WIRELESS_PACKET = 49 };
enum { PAD_STEAM_PAD_MOUSE = 0, PAD_STEAM_PAD_NONE = 7 };
// Button bits: report bytes 8, 9 and 10 as one little-endian 24-bit number
enum {
    PAD_STEAM_RT = 1u << 0, PAD_STEAM_LT = 1u << 1, PAD_STEAM_RB = 1u << 2, PAD_STEAM_LB = 1u << 3,   // a trigger's bit: pulled through its click
    PAD_STEAM_Y = 1u << 4, PAD_STEAM_B = 1u << 5, PAD_STEAM_X = 1u << 6, PAD_STEAM_A = 1u << 7,
    PAD_STEAM_LPAD_UP = 1u << 8, PAD_STEAM_LPAD_RIGHT = 1u << 9, PAD_STEAM_LPAD_LEFT = 1u << 10, PAD_STEAM_LPAD_DOWN = 1u << 11,
    PAD_STEAM_BACK = 1u << 12, PAD_STEAM_STEAM = 1u << 13, PAD_STEAM_START = 1u << 14, PAD_STEAM_LGRIP = 1u << 15,
    PAD_STEAM_RGRIP = 1u << 16, PAD_STEAM_LPAD_CLICK = 1u << 17, PAD_STEAM_RPAD_CLICK = 1u << 18, PAD_STEAM_LPAD_TOUCH = 1u << 19,
    PAD_STEAM_RPAD_TOUCH = 1u << 20, PAD_STEAM_STICK_CLICK = 1u << 22,
    PAD_STEAM_LPAD_AND_STICK = 1u << 23,   // the left pad touched with the joystick moved: reports take turns carrying each, PAD_STEAM_LPAD_TOUCH says which
};
#define PAD_STEAM_TRIGGER_HALF 128

static inline int pad_steam_is(int vid, int pid) { return vid == PAD_STEAM_VID && (pid == PAD_STEAM_PID_WIRED || pid == PAD_STEAM_PID_RECEIVER); }

typedef struct {
    int16_t stick[2];   // the joystick's last position: a report carrying the left pad keeps it
    uint32_t bits;      // the last state report's button bits, PAD_STEAM_LPAD_TOUCH while a thumb is on the left pad (DM_HID_LOG logs their changes, pad_hid_mac.h)
    int16_t lpad[2];    // where the thumb last was on the left pad
} pad_steam;

static int16_t pad_steam_s16(const uint8_t *p) { return (int16_t)(uint16_t)(p[0] | p[1] << 8); }
static float pad_steam_axis(int16_t v) { return v < -32767 ? -1.f : v / 32767.f; }
// A state report as Switch buttons and sticks (lx, ly, rx, ry); 0 for any other report
static int pad_steam_state(pad_steam *p, const uint8_t *r, size_t n, uint64_t *m, int32_t s[4]) {
    if (n < 24 || r[0] != 1 || r[1] != 0 || r[2] != PAD_STEAM_MSG_STATE) return 0;
    uint32_t b = r[8] | r[9] << 8 | (uint32_t)r[10] << 16;
    if (!(b & PAD_STEAM_LPAD_TOUCH)) { p->stick[0] = pad_steam_s16(r + 16); p->stick[1] = pad_steam_s16(r + 18); }
    else {
        p->lpad[0] = pad_steam_s16(r + 16); p->lpad[1] = pad_steam_s16(r + 18);
        if (!(b & PAD_STEAM_LPAD_AND_STICK)) p->stick[0] = p->stick[1] = 0;   // the left pad alone: the joystick is at rest
    }
    p->bits = b & PAD_STEAM_LPAD_AND_STICK ? b | PAD_STEAM_LPAD_TOUCH : b;   // a thumb on the pad in either of the reports that take turns
    // older firmware reports the joystick's click as the left pad's while the pad is not touched (SDL's FormatStatePacketUntilGyro)
    if ((b & PAD_STEAM_LPAD_CLICK) && !(b & (PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_AND_STICK))) b = (b & ~PAD_STEAM_LPAD_CLICK) | PAD_STEAM_STICK_CLICK;
    static const struct { uint32_t from; uint64_t to; } map[] = {
        {PAD_STEAM_B, DM_BTN_A}, {PAD_STEAM_A, DM_BTN_B}, {PAD_STEAM_Y, DM_BTN_X}, {PAD_STEAM_X, DM_BTN_Y},
        {PAD_STEAM_STICK_CLICK, DM_BTN_LS}, {PAD_STEAM_RPAD_CLICK, DM_BTN_RS}, {PAD_STEAM_LB, DM_BTN_L}, {PAD_STEAM_RB, DM_BTN_R},
        {PAD_STEAM_LT, DM_BTN_ZL}, {PAD_STEAM_RT, DM_BTN_ZR}, {PAD_STEAM_START, DM_BTN_PLUS}, {PAD_STEAM_BACK, DM_BTN_MINUS},
    };
    uint64_t o = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (b & map[i].from) o |= map[i].to;
    if (r[11] >= PAD_STEAM_TRIGGER_HALF) o |= DM_BTN_ZL;   // past half, as the raw HID pads' analog triggers
    if (r[12] >= PAD_STEAM_TRIGGER_HALF) o |= DM_BTN_ZR;
    if (b & PAD_STEAM_LPAD_CLICK) {
        if (b & PAD_STEAM_LPAD_UP) o |= DM_BTN_UP;
        if (b & PAD_STEAM_LPAD_DOWN) o |= DM_BTN_DOWN;
        if (b & PAD_STEAM_LPAD_LEFT) o |= DM_BTN_LEFT;
        if (b & PAD_STEAM_LPAD_RIGHT) o |= DM_BTN_RIGHT;
    }
    *m = o;
    pad_hid_stick(pad_steam_axis(p->stick[0]), pad_steam_axis(p->stick[1]), &s[0], &s[1]);
    if (b & PAD_STEAM_RPAD_TOUCH) pad_hid_stick(pad_steam_axis(pad_steam_s16(r + 20)), pad_steam_axis(pad_steam_s16(r + 22)), &s[2], &s[3]);
    else s[2] = s[3] = 0;   // the thumb off the right pad: the right stick at rest
    return 1;
}
// A wireless receiver's event: 1 a controller connected, -1 disconnected, 0 another report
static int pad_steam_wireless(const uint8_t *r, size_t n) {
    if (n < 5 || r[0] != 1 || r[1] != 0 || r[2] != PAD_STEAM_MSG_WIRELESS) return 0;
    return r[4] == 2 ? 1 : r[4] == 1 ? -1 : 0;
}

// Commands (64-byte feature reports): one without data, and the settings of gamepad mode and of lizard mode
static void pad_steam_cmd(uint8_t out[PAD_STEAM_LEN], uint8_t cmd) { memset(out, 0, PAD_STEAM_LEN); out[0] = cmd; }
static void pad_steam_setting(uint8_t out[PAD_STEAM_LEN], uint8_t setting, uint16_t value) {
    uint8_t *e = out + 2 + out[1]; e[0] = setting; e[1] = (uint8_t)value; e[2] = (uint8_t)(value >> 8); out[1] += 3;
}
static void pad_steam_gamepad_settings(uint8_t out[PAD_STEAM_LEN]) {   // SDL's ResetSteamController without its mouse mode
    pad_steam_cmd(out, PAD_STEAM_CMD_SETTINGS);
    pad_steam_setting(out, PAD_STEAM_SET_WIRELESS_PACKET, 2);
    pad_steam_setting(out, PAD_STEAM_SET_LPAD_MODE, PAD_STEAM_PAD_NONE);
    pad_steam_setting(out, PAD_STEAM_SET_RPAD_MODE, PAD_STEAM_PAD_NONE);
    pad_steam_setting(out, PAD_STEAM_SET_SMOOTH_MOUSE, 0);
}
static void pad_steam_lizard_settings(uint8_t out[PAD_STEAM_LEN]) {
    pad_steam_cmd(out, PAD_STEAM_CMD_SETTINGS);
    pad_steam_setting(out, PAD_STEAM_SET_RPAD_MODE, PAD_STEAM_PAD_MOUSE);
}

// Whether a device's user client is Steam's: IOKit names the process that opened it in IOUserClientCreator, "pid <N>, <name>"; Steam's is steam_osx
static int pad_steam_creator(const char *creator) { const char *c = creator ? strrchr(creator, ',') : NULL; return c && !strcmp(c, ", steam_osx"); }
// Whether SDL_GAMECONTROLLER_IGNORE_DEVICES ("0x28de/0x1102,0x045e/0x028e", SDL's format) names the device. Steam sets it for a game it starts with Steam Input on: the
// controllers it turns into its virtual pad, which the game is to read instead (pad_hid_mac.h). A "@file" list is not read
static int pad_steam_listed(const char *list, int vid, int pid) {
    for (const char *s = list; s && *s;) {
        char *end; unsigned long v = strtoul(s, &end, 16);
        if (*end == '/') { unsigned long p = strtoul(end + 1, &end, 16); if ((int)v == vid && (int)p == pid) return 1; }
        s = strchr(end, ','); if (s) s++;
    }
    return 0;
}
