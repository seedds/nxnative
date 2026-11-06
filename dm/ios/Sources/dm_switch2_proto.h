// Nintendo Switch 2 controller BLE protocol: identification, GATT identifiers, command framing, input-report decoding, stick calibration, HD rumble packing and the pad a player holds.
// Pure C with no platform headers, so tests check it on any machine and DmSwitch2BLE.m is only the CoreBluetooth transport.
//
// Protocol credit: the GATT identifiers, framing, report/calibration layout and rumble packing are a port of "Finally the Controller Works"
// by Peter Sharma (github.com/Peterksharma/switch2mac), which publishes its protocol notes and Swift CoreBluetooth implementation. It in turn
// credits ndeadly/switch2_controller_research, trevlars/switch2-controllers-linux (MIT), Nadeflore/switch2-controllers and the wider Switch 2
// reverse-engineering community. The Joy-Con 2 rules (the sideways hold, the grip pair, the rail-button bits, the right half's stick split and
// the per-form motion maps) follow JakePre's hardware-verified seedds/nxnative PR 1346, which carries recorded Joy-Con reports; this port keeps
// that evidence in dm/ios/tests (switch2_ios.c). The NSO GameCube rules are from switch2mac's published notes (its product id, its analog triggers
// at report offsets 0x3C/0x3D, and the rule that its motor characteristic must never be written).
// Evidence: the shipped "Finally the Controller Works" 0.2.0 binary (com.petersharma.finallythecontrollerworks) was opened in Ghidra through REA.
// The decompiled Switch2.buildCommand stores 0x91 in header byte 1 and the caller's flag, command and subcommand in bytes 0, 2 and 3 with a zero
// length and two zero trailer bytes; Switch2.InputReport.init reads offsets 0x10, 0x12, 0x14, 0x16, 0x19-0x1F, 0x21, 0x22, 0x2E, 0x30-0x3A and
// rejects fewer than 0x3C bytes; Switch2.motorPacket stores packetID & 0xF | 0x50; Switch2.memoryReadPayload stores 0x7E at byte 1; the
// ControllerSession.stepFeatures closure calls writeCommand(0x0C, 0x04, ...); stepBond calls writeCommand(0x15, 0x01, ...). The four GATT UUIDs are
// in the binary as strings. The same author publishes the protocol notes and Swift source as github.com/Peterksharma/switch2mac, which this
// header follows for field offsets and feature masks; the binary checks above confirm the installed build carries the same protocol.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include "dm_input.h"

// BLE advertising: Nintendo's manufacturer company id, USB-style vendor id and the controller model ids.
#define DS2_COMPANY_ID 0x0553
#define DS2_VENDOR_ID  0x057E
#define DS2_PID_JOYCON2_R     0x2066
#define DS2_PID_JOYCON2_L     0x2067
#define DS2_PID_PRO_2         0x2069
#define DS2_PID_NSO_GAMECUBE  0x2073

// GATT characteristics of the proprietary service (all writes are Write Without Response).
#define DS2_UUID_INPUT    "AB7DE9BE-89FE-49AD-828F-118F09DF7FD2"
#define DS2_UUID_CMD_WR   "649D4AC9-8EB7-4E6C-AF44-1EA54FE5F005"
#define DS2_UUID_CMD_RSP  "C765A961-D9D8-4D36-A20A-5315B111836A"
#define DS2_UUID_RUMBLE       "CC483F51-9258-427D-A939-630C31F72B05"  // Pro Controller 2. The NSO GameCube shares the lane but it must never be written: see the motor packet below.
#define DS2_UUID_RUMBLE_JC_R  "FA19B0FB-CD1F-46A7-84A1-BBB09E00C149"  // right Joy-Con 2
#define DS2_UUID_RUMBLE_JC_L  "289326CB-A471-485D-A8F4-240C14F18241"  // left Joy-Con 2

enum { DS2_CMD_MEMORY = 0x02, DS2_CMD_LEDS = 0x09, DS2_CMD_FEATURE = 0x0C, DS2_CMD_PAIR = 0x15 };
enum { DS2_SUB_MEMORY_READ = 0x04, DS2_SUB_LEDS_SET = 0x07, DS2_SUB_FEATURE_SET = 0x02, DS2_SUB_FEATURE_ENABLE = 0x04 };

// Feature mask: buttons, sticks, motion, battery current, magnetometer. The console's mask for every model; 0xFF makes a Joy-Con 2 report
// phantom ZL/ZR, and the mouse bit (0x10, Joy-Con 2's optical sensor) is left off: mouse mode is not part of this host.
#define DS2_FEATURES (0x01 | 0x02 | 0x04 | 0x20 | 0x80)

enum {
    DS2_ADDR_INFO = 0x00013000u,
    DS2_ADDR_FACTORY_STICK1 = 0x000130A8u,
    DS2_ADDR_FACTORY_STICK2 = 0x000130E8u,
    DS2_ADDR_USER_STICK1 = 0x001FC042u,
    DS2_ADDR_USER_STICK2 = 0x001FC062u,
};

// Raw button bits, 32-bit little-endian at report offset 4. The rail buttons: the right Joy-Con holds 0x10 / 0x20 and the left 0x100000 / 0x200000,
// in the Switch 1's order SR before SL (observed on a left and a right Joy-Con 2; switch2mac's protocol notes list the left's two the other way round).
enum {
    DS2_BTN_Y = 0x00000001u, DS2_BTN_X = 0x00000002u, DS2_BTN_B = 0x00000004u, DS2_BTN_A = 0x00000008u,
    DS2_BTN_SR_R = 0x00000010u, DS2_BTN_SL_R = 0x00000020u,
    DS2_BTN_R = 0x00000040u, DS2_BTN_ZR = 0x00000080u, DS2_BTN_MINUS = 0x00000100u, DS2_BTN_PLUS = 0x00000200u,
    DS2_BTN_RSTICK = 0x00000400u, DS2_BTN_LSTICK = 0x00000800u, DS2_BTN_HOME = 0x00001000u,
    DS2_BTN_CAPTURE = 0x00002000u, DS2_BTN_C = 0x00004000u,
    DS2_BTN_DOWN = 0x00010000u, DS2_BTN_UP = 0x00020000u, DS2_BTN_RIGHT = 0x00040000u, DS2_BTN_LEFT = 0x00080000u,
    DS2_BTN_SR_L = 0x00100000u, DS2_BTN_SL_L = 0x00200000u,
    DS2_BTN_L = 0x00400000u, DS2_BTN_ZL = 0x00800000u,
    DS2_BTN_GR = 0x01000000u, DS2_BTN_GL = 0x02000000u,
};

// Console-style player LED patterns for players 1-8.
static const uint8_t ds2_led_pattern[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};

static inline uint16_t ds2_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint32_t ds2_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline int16_t ds2_s16(const uint8_t *p) { return (int16_t)ds2_u16(p); }

typedef struct {
    uint32_t timestamp;
    uint32_t buttons;
    uint16_t lx, ly, rx, ry;    // packed 12-bit pairs, as reported
    int16_t accel[3], gyro[3];  // raw little-endian counts
    uint16_t battery_mv;
    int16_t battery_current;    // signed, + charging (feature mask 0x20)
    uint8_t charge_state;       // at 0x21; enumeration not published, charge signing above is the usable signal
    int16_t temperature_raw;    // IMU die temperature at 0x2E: degrees C = 25 + raw / 127 (switch2mac's protocol notes, citing Switch2Connect)
    uint8_t trigger_l, trigger_r;   // the NSO GameCube pad's analog triggers at 0x3C / 0x3D; other models leave them unparsed (0)
} ds2_report;

// A 63/64-byte input notification. The input characteristic can also carry 112-byte headset-audio packets whose first byte is a sequence number;
// those do not start with our report shape and are ignored by length: a report is at least 0x3C bytes and shorter than the audio packet.
static inline int ds2_report_parse(const uint8_t *d, size_t n, ds2_report *r) {
    if (!d || !r || n < 0x3C || n > 0x50) return 0;
    memset(r, 0, sizeof *r);
    r->timestamp = ds2_u32(d);
    r->buttons = ds2_u32(d + 4);
    uint32_t a = (uint32_t)d[10] | (uint32_t)d[11] << 8 | (uint32_t)d[12] << 16;
    uint32_t b = (uint32_t)d[13] | (uint32_t)d[14] << 8 | (uint32_t)d[15] << 16;
    r->lx = (uint16_t)(a & 0xFFF); r->ly = (uint16_t)(a >> 12);
    r->rx = (uint16_t)(b & 0xFFF); r->ry = (uint16_t)(b >> 12);
    r->battery_mv = ds2_u16(d + 0x1F);
    r->accel[0] = ds2_s16(d + 0x30); r->accel[1] = ds2_s16(d + 0x32); r->accel[2] = ds2_s16(d + 0x34);
    r->gyro[0] = ds2_s16(d + 0x36); r->gyro[1] = ds2_s16(d + 0x38); r->gyro[2] = ds2_s16(d + 0x3A);
    if (n >= 0x30) r->temperature_raw = ds2_s16(d + 0x2E);
    if (n >= 0x24) { r->battery_current = ds2_s16(d + 0x22); r->charge_state = d[0x21]; }
    if (n >= 0x3E) { r->trigger_l = d[0x3C]; r->trigger_r = d[0x3D]; }
    return 1;
}

// Parse a button string from settings (e.g. DM_PAD_GL / DM_PAD_GR) to its NpadButton bit mask.
static inline uint64_t ds2_btn_from_str(const char *s) {
    if (!s || !*s || !strcmp(s, "none") || !strcmp(s, "0")) return 0;
    if (!strcasecmp(s, "b")) return DM_BTN_B;
    if (!strcasecmp(s, "a")) return DM_BTN_A;
    if (!strcasecmp(s, "y")) return DM_BTN_Y;
    if (!strcasecmp(s, "x")) return DM_BTN_X;
    if (!strcasecmp(s, "lstick") || !strcasecmp(s, "ls") || !strcasecmp(s, "l3")) return DM_BTN_LS;
    if (!strcasecmp(s, "rstick") || !strcasecmp(s, "rs") || !strcasecmp(s, "r3")) return DM_BTN_RS;
    if (!strcasecmp(s, "zl")) return DM_BTN_ZL;
    if (!strcasecmp(s, "zr")) return DM_BTN_ZR;
    if (!strcasecmp(s, "l")) return DM_BTN_L;
    if (!strcasecmp(s, "r")) return DM_BTN_R;
    return 0;
}

// Switch 2 raw buttons to nn::hid NpadButton bits (dm_input.h). Home, Capture, C and the rail SL/SR have no NpadButton here; iOS also
// reserves Home and Capture as system gestures, so they are deliberately not forwarded. GL and GR map to the given button masks (default none).
static inline uint64_t ds2_buttons_mapped(uint32_t b, uint64_t gl_mask, uint64_t gr_mask) {
    uint64_t m = 0;
    if (b & DS2_BTN_A) m |= DM_BTN_A;
    if (b & DS2_BTN_B) m |= DM_BTN_B;
    if (b & DS2_BTN_X) m |= DM_BTN_X;
    if (b & DS2_BTN_Y) m |= DM_BTN_Y;
    if (b & DS2_BTN_L) m |= DM_BTN_L;
    if (b & DS2_BTN_R) m |= DM_BTN_R;
    if (b & DS2_BTN_ZL) m |= DM_BTN_ZL;
    if (b & DS2_BTN_ZR) m |= DM_BTN_ZR;
    if (b & DS2_BTN_PLUS) m |= DM_BTN_PLUS;
    if (b & DS2_BTN_MINUS) m |= DM_BTN_MINUS;
    if (b & DS2_BTN_LSTICK) m |= DM_BTN_LS;
    if (b & DS2_BTN_RSTICK) m |= DM_BTN_RS;
    if (b & DS2_BTN_LEFT) m |= DM_BTN_LEFT;
    if (b & DS2_BTN_UP) m |= DM_BTN_UP;
    if (b & DS2_BTN_RIGHT) m |= DM_BTN_RIGHT;
    if (b & DS2_BTN_DOWN) m |= DM_BTN_DOWN;
    if ((b & DS2_BTN_GL) && gl_mask) m |= gl_mask;
    if ((b & DS2_BTN_GR) && gr_mask) m |= gr_mask;
    return m;
}

static inline uint64_t ds2_buttons(uint32_t b) {
    return ds2_buttons_mapped(b, 0, 0);
}

// A Joy-Con 2's buttons. Alone (sideways hold): the rail buttons are the shoulder buttons. In a grip pair the rail buttons sit under the palms
// and stay unmapped; they are the pair's L / R only when the half is held on its own.
static inline uint64_t ds2_joycon_buttons(uint32_t b, int alone) {
    uint64_t m = ds2_buttons(b);
    if (alone) {
        if (b & (DS2_BTN_SL_L | DS2_BTN_SL_R)) m |= DM_BTN_L;
        if (b & (DS2_BTN_SR_L | DS2_BTN_SR_R)) m |= DM_BTN_R;
    }
    return m;
}

// Stick calibration block (0x0B bytes): centre, max and min each a packed 12-bit pair. A user slot of 0xFFFFFF is unset; the factory slot is used then.
// The travel fields are above and below the centre, not absolute positions; a calibration block whose travel is not positive is not valid
// (a right Joy-Con 2 whose stick-2 slot was read by mistake reads 0xFFFF travels, and a stuck axis is the symptom).
typedef struct { float cx, cy, maxx, maxy, minx, miny; int valid; } ds2_cal;

static inline void ds2_cal_default(ds2_cal *c) { c->cx = c->cy = 2048; c->maxx = c->maxy = c->minx = c->miny = 1200; c->valid = 0; }

static inline int ds2_cal_blank(const uint8_t *d, size_t n) { return !d || n < 3 || (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF); }

static inline int ds2_cal_parse(const uint8_t *d, size_t n, ds2_cal *c) {
    if (!d || !c || n < 9 || ds2_cal_blank(d, n)) return 0;
    uint32_t p = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16;
    uint32_t q = (uint32_t)d[3] | (uint32_t)d[4] << 8 | (uint32_t)d[5] << 16;
    uint32_t r = (uint32_t)d[6] | (uint32_t)d[7] << 8 | (uint32_t)d[8] << 16;
    c->cx = (float)(p & 0xFFF); c->cy = (float)(p >> 12);
    c->maxx = (float)(q & 0xFFF); c->maxy = (float)(q >> 12);
    c->minx = (float)(r & 0xFFF); c->miny = (float)(r >> 12);
    c->valid = c->maxx > 100 && c->maxy > 100 && c->minx > 100 && c->miny > 100;
    return c->valid;
}

// One axis: centre-relative travel scaled by the larger (max) or smaller (min) side, deadzone in raw counts, clamped to -1...1.
static inline float ds2_cal_axis(float v, float centre, float maxRange, float minRange, float dead) {
    float s = v - centre;
    if (s > dead) return maxRange > 0 ? fminf(s / maxRange, 1.f) : 0.f;
    if (s < -dead) return minRange > 0 ? -fminf(-s / minRange, 1.f) : 0.f;
    return 0.f;
}

// Calibrated sticks in -1...1, x right, y up (the same convention as dm_pad_set_player and the touch gamepad).
static inline void ds2_cal_apply(const ds2_cal *c, uint16_t rawx, uint16_t rawy, float dead, float *x, float *y) {
    ds2_cal d; if (!c || !c->valid) { ds2_cal_default(&d); c = &d; }
    *x = ds2_cal_axis((float)rawx, c->cx, c->maxx, c->minx, dead);
    *y = ds2_cal_axis((float)rawy, c->cy, c->maxy, c->miny, dead);
}

// Command frame: cmd, 0x91, transport flag (0x01 Bluetooth), subcommand, 0, length, 0, 0, then the payload. Returns the frame length or 0.
static inline size_t ds2_command(uint8_t *out, size_t cap, uint8_t cmd, uint8_t sub, const uint8_t *payload, size_t n) {
    if (!out || n > 0xFF || cap < 8 + n) return 0;
    out[0] = cmd; out[1] = 0x91; out[2] = 0x01; out[3] = sub; out[4] = 0; out[5] = (uint8_t)n; out[6] = 0; out[7] = 0;
    if (n) memcpy(out + 8, payload, n);
    return 8 + n;
}

// Memory-read payload: len, 0x7E, 0, 0, address little-endian. The controller returns len, 0x7E, 0, 0, address, 0, 0, 0 and then the data.
static inline void ds2_memory_payload(uint8_t out[8], uint8_t len, uint32_t addr) {
    out[0] = len; out[1] = 0x7E; out[2] = 0; out[3] = 0;
    out[4] = (uint8_t)addr; out[5] = (uint8_t)(addr >> 8); out[6] = (uint8_t)(addr >> 16); out[7] = (uint8_t)(addr >> 24);
}

// One HD-rumble sample: 9-bit frequency in Hz, 10-bit amplitude, packed into five little-endian bytes.
typedef struct { uint16_t lf_hz, lf_amp, hf_hz, hf_amp; } ds2_vibration;

static inline void ds2_vibration_pack(const ds2_vibration *v, uint8_t out[5]) {
    uint64_t x = 0;
    x |= (uint64_t)(v->lf_hz & 0x1FF);
    x |= (uint64_t)(v->lf_amp & 0x3FF) << 10;
    x |= (uint64_t)(v->hf_hz & 0x1FF) << 20;
    x |= (uint64_t)(v->hf_amp & 0x3FF) << 30;
    for (int i = 0; i < 5; i++) out[i] = (uint8_t)(x >> (8 * i));
}

// Motor packet: 0x00, then one block of 0x50|seq followed by three identical samples per actuator: one for a Joy-Con 2 (its single grip),
// two for the Pro Controller 2 (left and right motors holding the same tone). The NSO GameCube pad never receives one of these: writing its
// motor characteristic powers the pad off (switch2mac's protocol notes §7), and its built-in presets (command 0x0A/0x02) are not used here.
static inline size_t ds2_motor_packet(uint8_t out[40], const ds2_vibration *v, uint8_t seq, int blocks) {
    uint8_t sample[5]; ds2_vibration_pack(v, sample);
    size_t o = 0;
    out[o++] = 0x00;
    for (int block = 0; block < blocks; block++) {
        out[o++] = (uint8_t)(0x50 | (seq & 0x0F));
        for (int i = 0; i < 3; i++) { memcpy(out + o, sample, 5); o += 5; }
    }
    return o;
}

// Advertising payload AFTER the two company-id bytes. Vendor id at 3, product id at 5, reconnect host (the bonded host's Bluetooth address,
// little-endian) at 10; a zero host is pairing mode (Sync held, or no host bonded).
typedef struct { uint16_t vid, pid; uint64_t host; } ds2_advert;

static inline int ds2_is_controller(uint16_t pid) {
    return pid == DS2_PID_PRO_2 || pid == DS2_PID_JOYCON2_L || pid == DS2_PID_JOYCON2_R || pid == DS2_PID_NSO_GAMECUBE;
}

static inline int ds2_advert_parse(const uint8_t *d, size_t n, ds2_advert *a) {
    if (!d || !a || n < 16) return 0;
    a->vid = ds2_u16(d + 3); a->pid = ds2_u16(d + 5);
    a->host = 0;
    for (int i = 0; i < 6; i++) a->host |= (uint64_t)d[10 + i] << (8 * i);
    return a->vid == DS2_VENDOR_ID && ds2_is_controller(a->pid);
}

// Report motion is raw counts. Opt-in (DM_SW2_MOTION=1) until it is measured on our own hardware; see the per-form maps below for what is measured.
#define DS2_ACCEL_G_PER_COUNT DS2_ACCEL_G
#define DS2_GYRO_RAD_PER_COUNT DS2_GYRO_RAD

// ---- One controller and the pad a player holds ----

// Gyro rest offset: a Joy-Con 2 at rest reads about 1.3 degrees per second off. A second of samples in which the gyro and accelerometer hardly vary
// is a controller lying still; the mean becomes the offset (blended with the previous one). An offset past 4 degrees per second is not taken; that
// is a slow turn, not rest.
#define DS2_REST_N 33
typedef struct { int n, have; int32_t sum[3]; int16_t gmin[3], gmax[3], amin[3], amax[3]; float bias[3]; } ds2_rest;

static inline void ds2_rest_update(ds2_rest *r, const int16_t raw[6]) {
    if (!r->n) for (int i = 0; i < 3; i++) { r->sum[i] = 0; r->gmin[i] = r->gmax[i] = raw[3 + i]; r->amin[i] = r->amax[i] = raw[i]; }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += raw[3 + i];
        if (raw[3 + i] < r->gmin[i]) r->gmin[i] = raw[3 + i];
        if (raw[3 + i] > r->gmax[i]) r->gmax[i] = raw[3 + i];
        if (raw[i] < r->amin[i]) r->amin[i] = raw[i];
        if (raw[i] > r->amax[i]) r->amax[i] = raw[i];
        if (r->gmax[i] - r->gmin[i] > 100 || r->amax[i] - r->amin[i] > 300) { r->n = 0; return; }   // moving: start the second again
    }
    if (++r->n < DS2_REST_N) return;
    r->n = 0;
    float mean[3];
    for (int i = 0; i < 3; i++) { mean[i] = (float)r->sum[i] / DS2_REST_N; if (fabsf(mean[i]) > 60.f) return; }
    for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * mean[i] : mean[i];
    r->have = 1;
}

// The pad state: one full pad member (a Pro Controller 2 or an NSO GameCube pad), or Joy-Con 2 halves in left and right, or one half alone.
// Every member keeps its own last report, calibration and gyro rest learner; the pad is recomposed as each member's report arrives.

// The pad state: one full pad member (a Pro Controller 2 or an NSO GameCube pad), or Joy-Con 2 halves in left and right, or one half alone.
// Every device keeps its own last report and calibration; the pad is recomposed as each member's report arrives.
typedef struct {
    int have;                 // a controller is connected in this member
    uint16_t pid;             // DS2_PID_*
    ds2_report r;             // its last input report
    ds2_cal cal[2];           // stick slots: cal[0] is stick 1 (a Joy-Con's only stick), cal[1] stick 2
    ds2_rest rest;            // gyro rest-offset learner (s2_rest below)
} ds2_dev;
typedef struct { ds2_dev pro, left, right; } ds2_pad_state;

// The NSO GameCube pad's analog triggers are the only analog ones (report offsets 0x3C / 0x3D, 0 to 255). The Nintendo Switch maps them onto
// the Npad's digital ZL / ZR, so a hold counts past a travel threshold (one quarter by default, DM_SW2_GC_TRIGGER);
// the depth itself is lost (dm_pad_set_player carries buttons and sticks only).
#define DS2_TRIGGER_HELD 64

static inline uint64_t ds2_gamecube_buttons(uint32_t b, uint8_t trigger_l, uint8_t trigger_r, uint8_t held) {
    uint64_t m = ds2_buttons(b);
    if (trigger_l >= held) m |= DM_BTN_L;
    if (trigger_r >= held) m |= DM_BTN_R;
    return m;
}

// One device's buttons: a Joy-Con held on its own is used sideways; two together are a Pro Controller, each with its own half of the buttons.
static inline uint64_t ds2_dev_buttons(const ds2_dev *dev, int alone, uint64_t gl_mask, uint64_t gr_mask, uint8_t gc_held) {
    if (dev->pid == DS2_PID_JOYCON2_L || dev->pid == DS2_PID_JOYCON2_R)
        return ds2_joycon_buttons(dev->r.buttons, alone);
    if (dev->pid == DS2_PID_NSO_GAMECUBE)
        return ds2_gamecube_buttons(dev->r.buttons, dev->r.trigger_l, dev->r.trigger_r, gc_held);
    return ds2_buttons_mapped(dev->r.buttons, gl_mask, gr_mask);
}

// A device's sticks into dm_pad_set_player's -32767..32767 slots. A Joy-Con has one physical stick: the left unit reports it in the first field (0x0A),
// the right unit in the second (0x0D), but both keep its calibration in the stick-1 slots (a Joy-Con has no stick-2 calibration; calibrating a right
// Joy-Con from the empty stick-2 slot reads a dead axis). A full pad member (which < 0) fills both pairs from its two calibrated fields.
static inline void ds2_dev_stick(const ds2_dev *dev, float dead, int which, int32_t st[4]) {
    st[0] = st[1] = st[2] = st[3] = 0;
    float x = 0, y = 0;
    if (which < 0) {
        ds2_cal_apply(&dev->cal[0], dev->r.lx, dev->r.ly, dead, &x, &y);
        st[0] = (int32_t)lrintf(x * 32767.f); st[1] = (int32_t)lrintf(y * 32767.f);
        ds2_cal_apply(&dev->cal[1], dev->r.rx, dev->r.ry, dead, &x, &y);
        st[2] = (int32_t)lrintf(x * 32767.f); st[3] = (int32_t)lrintf(y * 32767.f);
        return;
    }
    if (dev->pid == DS2_PID_JOYCON2_R) ds2_cal_apply(&dev->cal[0], dev->r.rx, dev->r.ry, dead, &x, &y);
    else ds2_cal_apply(&dev->cal[0], dev->r.lx, dev->r.ly, dead, &x, &y);
    st[2 * which] = (int32_t)lrintf(x * 32767.f);
    st[2 * which + 1] = (int32_t)lrintf(y * 32767.f);
}

// The pad's buttons and sticks (-32767..32767, y up: the dm_pad_set_player shape): a full pad member's as they are; two Joy-Con halves together
// are a Pro Controller (the left's stick the left stick, the right's the right); one half alone is held sideways, its stick the left stick.
// Returns 0 when the pad holds no controller.
static inline int ds2_pad_input(const ds2_pad_state *p, float dead, uint8_t gc_held, uint64_t gl_mask, uint64_t gr_mask,
                                uint64_t *buttons, int32_t st[4]) {
    st[0] = st[1] = st[2] = st[3] = 0;
    uint64_t m = 0;
    #define DS2_MERGE(dev, alone, which) do { \
        if ((dev)->have) { m |= ds2_dev_buttons(dev, alone, gl_mask, gr_mask, gc_held); \
            int32_t t[4] = {0, 0, 0, 0}; ds2_dev_stick(dev, dead, which, t); \
            for (int i = 0; i < 4; i++) st[i] |= t[i]; } } while (0)
    if (p->pro.have) DS2_MERGE(&p->pro, 0, -1);
    else if (p->left.have && p->right.have) { DS2_MERGE(&p->left, 0, 0); DS2_MERGE(&p->right, 0, 1); }
    else if (p->left.have) DS2_MERGE(&p->left, 1, 0);
    else if (p->right.have) DS2_MERGE(&p->right, 1, 0);
    else return 0;
    #undef DS2_MERGE
    *buttons = m;
    return 1;
}

// Raw sensor counts to the Switch frame (G, rad/s): the output acceleration x y z, then the output rotation x y z. The Switch frame is x right,
// y forward, z up with rotations right-handed about those, and acceleration the direction of gravity (face up and still: 0 0 -1; motion_pro.h).
// map holds six signed channel numbers (1..6 = accel x y z, gyro x y z; negative flips the sign), one per output channel in order.
// Scales: 1/4096 G per count and 0.070 degrees per second per count, the Switch family's nominal values; the Switch 2's gyro scale agrees with
// them to within the accuracy of a hand movement (slow 90 degree turns integrate to 83..125 degrees), not measured better.
#define DS2_ACCEL_G (1.f / 4096.f)
#define DS2_GYRO_RAD (0.070f * 3.14159265358979323846f / 180.f)
// Measured on a right Joy-Con 2 held upright (stick on top, face toward the ceiling): raw x right, y forward, z up, and the gyro channels are
// right-handed rotations about the same axes (tilting the top away: gyro x -83 degrees; turning left: gyro z +115; leaning the right side down:
// gyro y +104). So the pair's map is the identity for rotation and the negated specific force for gravity. A half held alone is turned a quarter
// turn about the up axis: the right one with its top to the player's left, the left one with its top to the right (the Pro Controller 1 map of
// motion_pro.h). The Pro Controller 2's frame is not measured on hardware: it takes that map; this is why motion stays behind DM_SW2_MOTION.
static const int8_t ds2_map_pair[6] = {-1, -2, -3, 4, 5, 6};
static const int8_t ds2_map_pro[6] = {2, -1, -3, -5, 4, 6};
static const int8_t ds2_map_right_alone[6] = {2, -1, -3, -5, 4, 6};
static const int8_t ds2_map_left_alone[6] = {-2, 1, -3, 5, -4, 6};

static inline const int8_t *ds2_pad_map(const ds2_pad_state *p, const ds2_dev *dev) {
    if (p->pro.have) return ds2_map_pro;
    if (p->left.have && p->right.have) return ds2_map_pair;
    return dev == &p->left ? ds2_map_left_alone : ds2_map_right_alone;
}

// The device whose motion sensor the pad uses: the full pad member; of two halves the right one; of one half, that one. NULL when the pad is empty.
static inline ds2_dev *ds2_pad_motion_dev(ds2_pad_state *p) {
    if (p->pro.have) return &p->pro;
    if (p->left.have && p->right.have) return &p->right;   // a pair's console-style sensor is the right half's
    return p->left.have ? &p->left : p->right.have ? &p->right : NULL;
}

// bias: the gyro offset in counts (ds2_rest.bias), NULL for none
static inline void ds2_motion(const int16_t raw[6], const int8_t map[6], const float *bias, float acc[3], float rot[3]) {
    for (int i = 0; i < 6; i++) {
        int c = map[i] < 0 ? -map[i] : map[i];
        float s = map[i] < 0 ? -1.f : 1.f;
        if (c < 1 || c > 6) { c = 1; s = 0; }
        float v = s * ((float)raw[c - 1] - (c > 3 && bias ? bias[c - 4] : 0.f));
        if (i < 3) acc[i] = v * DS2_ACCEL_G; else rot[i - 3] = v * DS2_GYRO_RAD;
    }
}
