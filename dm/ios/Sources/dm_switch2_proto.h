// Nintendo Switch 2 controller BLE protocol: identification, GATT identifiers, command framing, input-report decoding and HD rumble packing.
// Pure C with no platform headers, so tests/t_switch2.c checks it on any machine and DmSwitch2BLE.m is only the CoreBluetooth transport.
//
// Protocol credit: the GATT identifiers, framing, report/calibration layout and rumble packing are a port of "Finally the Controller Works"
// by Peter Sharma (github.com/Peterksharma/switch2mac), which publishes its protocol notes and Swift CoreBluetooth implementation. It in turn
// credits ndeadly/switch2_controller_research, trevlars/switch2-controllers-linux (MIT), Nadeflore/switch2-controllers and the wider Switch 2
// reverse-engineering community.
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
#include "dm_input.h"

// BLE advertising: Nintendo's manufacturer company id, USB-style vendor id and the controller model ids.
#define DS2_COMPANY_ID 0x0553
#define DS2_VENDOR_ID  0x057E
#define DS2_PID_PRO_CONTROLLER_2 0x2069

// GATT characteristics of the proprietary service (all writes are Write Without Response).
#define DS2_UUID_INPUT    "AB7DE9BE-89FE-49AD-828F-118F09DF7FD2"
#define DS2_UUID_CMD_WR   "649D4AC9-8EB7-4E6C-AF44-1EA54FE5F005"
#define DS2_UUID_CMD_RSP  "C765A961-D9D8-4D36-A20A-5315B111836A"
#define DS2_UUID_RUMBLE   "CC483F51-9258-427D-A939-630C31F72B05"

enum { DS2_CMD_MEMORY = 0x02, DS2_CMD_LEDS = 0x09, DS2_CMD_FEATURE = 0x0C, DS2_CMD_PAIR = 0x15 };
enum { DS2_SUB_MEMORY_READ = 0x04, DS2_SUB_LEDS_SET = 0x07, DS2_SUB_FEATURE_SET = 0x02, DS2_SUB_FEATURE_ENABLE = 0x04 };

// Feature mask the console sets on a Pro Controller 2: buttons, sticks, motion, battery current, magnetometer. 0xFF makes Joy-Con 2 report
// phantom ZL/ZR, so the mask is explicit.
#define DS2_FEATURES_PRO (0x01 | 0x02 | 0x04 | 0x20 | 0x80)

enum {
    DS2_ADDR_INFO = 0x00013000u,
    DS2_ADDR_FACTORY_STICK1 = 0x000130A8u,
    DS2_ADDR_FACTORY_STICK2 = 0x000130E8u,
    DS2_ADDR_USER_STICK1 = 0x001FC042u,
    DS2_ADDR_USER_STICK2 = 0x001FC062u,
};

// Raw button bits, 32-bit little-endian at report offset 4.
enum {
    DS2_BTN_Y = 0x00000001u, DS2_BTN_X = 0x00000002u, DS2_BTN_B = 0x00000004u, DS2_BTN_A = 0x00000008u,
    DS2_BTN_R = 0x00000040u, DS2_BTN_ZR = 0x00000080u, DS2_BTN_MINUS = 0x00000100u, DS2_BTN_PLUS = 0x00000200u,
    DS2_BTN_RSTICK = 0x00000400u, DS2_BTN_LSTICK = 0x00000800u, DS2_BTN_HOME = 0x00001000u,
    DS2_BTN_CAPTURE = 0x00002000u, DS2_BTN_C = 0x00004000u,
    DS2_BTN_DOWN = 0x00010000u, DS2_BTN_UP = 0x00020000u, DS2_BTN_RIGHT = 0x00040000u, DS2_BTN_LEFT = 0x00080000u,
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
    return 1;
}

// Switch 2 raw buttons to nn::hid NpadButton bits (dm_input.h). Home, Capture, C, GL, GR and the rail SL/SR have no NpadButton here; iOS also
// reserves Home and Capture as system gestures, so they are deliberately not forwarded.
static inline uint64_t ds2_buttons(uint32_t b) {
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
    return m;
}

// Stick calibration block (0x0B bytes): centre, max and min each a packed 12-bit pair. A user slot of 0xFFFFFF is unset; the factory slot is used then.
typedef struct { float cx, cy, maxx, maxy, minx, miny; int valid; } ds2_cal;

static inline void ds2_cal_default(ds2_cal *c) { c->cx = c->cy = 2048; c->maxx = c->maxy = c->minx = c->miny = 2048; c->valid = 0; }

static inline int ds2_cal_parse(const uint8_t *d, size_t n, ds2_cal *c) {
    if (!d || !c || n < 9) return 0;
    uint32_t p = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16;
    uint32_t q = (uint32_t)d[3] | (uint32_t)d[4] << 8 | (uint32_t)d[5] << 16;
    uint32_t r = (uint32_t)d[6] | (uint32_t)d[7] << 8 | (uint32_t)d[8] << 16;
    c->cx = (float)(p & 0xFFF); c->cy = (float)(p >> 12);
    c->maxx = (float)(q & 0xFFF); c->maxy = (float)(q >> 12);
    c->minx = (float)(r & 0xFFF); c->miny = (float)(r >> 12);
    c->valid = 1;
    return 1;
}

static inline int ds2_cal_blank(const uint8_t *d, size_t n) { return !d || n < 3 || (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF); }

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

// Motor packet: 0x00, then two blocks of 0x50|seq followed by three identical samples (Pro Controller 2 drives both grips).
static inline size_t ds2_motor_packet(uint8_t out[40], const ds2_vibration *v, uint8_t seq) {
    uint8_t sample[5]; ds2_vibration_pack(v, sample);
    size_t o = 0;
    out[o++] = 0x00;
    for (int block = 0; block < 2; block++) {
        out[o++] = (uint8_t)(0x50 | (seq & 0x0F));
        for (int i = 0; i < 3; i++) { memcpy(out + o, sample, 5); o += 5; }
    }
    return o;
}

// Advertising payload AFTER the two company-id bytes. Vendor id at 3, product id at 5, reconnect host (the bonded host's Bluetooth address,
// little-endian) at 10; a zero host is pairing mode (Sync held, or no host bonded).
typedef struct { uint16_t vid, pid; uint64_t host; } ds2_advert;

static inline int ds2_advert_parse(const uint8_t *d, size_t n, ds2_advert *a) {
    if (!d || !a || n < 16) return 0;
    a->vid = ds2_u16(d + 3); a->pid = ds2_u16(d + 5);
    a->host = 0;
    for (int i = 0; i < 6; i++) a->host |= (uint64_t)d[10 + i] << (8 * i);
    return a->vid == DS2_VENDOR_ID;
}

// Report motion is raw counts. Nominal scales of the Switch family IMU reports (1/4096 G, 70 mdps/count); the Switch 2 axis order is not
// verified against hardware yet, so DmSwitch2BLE publishes motion only with DM_SW2_MOTION=1 and documents it as inferred.
#define DS2_ACCEL_G_PER_COUNT (1.f / 4096.f)
#define DS2_GYRO_RAD_PER_COUNT (0.070f * (float)3.14159265358979323846f / 180.f)
