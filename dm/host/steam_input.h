/*
 * Triton protocol adapted from SDL_hidapi_steam_triton.c and Valve's controller
 * structs in SDL e4ed62ee3f7d7fda8bf8e0f09e42b14a7ac11984. This is an altered,
 * standalone implementation, not the SDL driver. See docs/mac-controllers.md.
 *
 * Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
 * Copyright (C) Valve Corporation
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from the
 * use of this software. Permission is granted to anyone to use this software
 * for any purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim
 *    that you wrote the original software. If you use this software in a product,
 *    an acknowledgment in the product documentation would be appreciated but is
 *    not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "pad_map.h"
typedef struct { int down; int32_t x, y; float pressure; } steam_pad;
typedef struct {
    uint32_t buttons; int32_t lx, ly, rx, ry;
    steam_pad pad[2];
    float acc[3], gyro[3]; // Switch coordinates: acceleration in G, gyro in revolutions/s
    uint32_t tick; unsigned tick_us;
} steam_state;
static inline int steam_device(int vid, int pid, int interface) {
    return vid == 0x28de && (pid == 0x1302 || pid == 0x1303 ||
        ((pid == 0x1304 || pid == 0x1305) && interface >= 2 && interface <= 5));
}
static inline unsigned steam_u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static inline int steam_i16(const unsigned char *p) { unsigned n = steam_u16(p); return n >= 32768 ? (int)n - 65536 : (int)n; }
static inline int32_t steam_axis(const unsigned char *p) {
    int n = steam_u16(p); if (n >= 32768) n -= 65536; return n == -32768 ? -32767 : n;
}
// Raw HID sticks drift slightly at rest. Games can treat any nonzero stick as
// intentional input and suppress gyro aiming (TotK). Keep the centre exactly
// zero, rescale the remaining travel, and retain full per-axis diagonals.
static inline float steam_deadzone_value(void) {
    const char *e = getenv("DM_STEAM_DEADZONE"); if (!e) return 0.10f;
    char *end; float v = strtof(e, &end);
    return end != e && !*end && v >= 0 && v < 0.9f ? v : 0.10f;
}
static inline void steam_deadzone(int32_t *x, int32_t *y, float dz) {
    float fx = *x / 32767.0f, fy = *y / 32767.0f, radius = hypotf(fx, fy);
    if (radius <= dz) { *x = *y = 0; return; }
    if (radius >= 1) return; // square-range diagonals are already beyond full radial travel; do not rotate them by clipping
    float scale = (radius - dz) / ((1 - dz) * radius);
    *x = (int32_t)lrintf(fmaxf(-1, fminf(1, fx * scale)) * 32767);
    *y = (int32_t)lrintf(fmaxf(-1, fminf(1, fy * scale)) * 32767);
}
// Return 1 for a complete state, -1 disconnected, 2 connected, 0 ignored.
// USB (0x42), BLE (0x45), and timestamped (0x47) share their first 18 bytes.
// Require the full 46-byte state, including touch/IMU, to reject truncated input.
static inline int steam_decode(const unsigned char *p, size_t n, steam_state *s) {
    if (n < 2) return 0;
    if (p[0] == 0x46 || p[0] == 0x79) return p[1] == 1 ? -1 : p[1] == 2 ? 2 : 0;
    if (n < 46 || (p[0] != 0x42 && p[0] != 0x45 && p[0] != 0x47)) return 0;
    uint32_t b = (uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
    // Wire bit for each physical input (triggers also use their analog values).
    static const int wire[DM_PAD_BUTTONS] = {1, 0, 3, 2, 15, 5, 19, 9, 27, 23, 6, 14, 12, 13, 11, 10, 17, 7, 18, 8, 26, 22, 16, 4};
    s->buttons = 0;
    for (int i = 0; i < DM_PAD_BUTTONS; i++) if (b & (1u << wire[i])) s->buttons |= 1u << i;
    if (steam_u16(p + 6) >= 16384) s->buttons |= DM_BTN_ZL;
    if (steam_u16(p + 8) >= 16384) s->buttons |= DM_BTN_ZR;
    s->lx = steam_axis(p + 10); s->ly = steam_axis(p + 12);
    s->rx = steam_axis(p + 14); s->ry = steam_axis(p + 16);
    float dz = steam_deadzone_value();
    steam_deadzone(&s->lx, &s->ly, dz); steam_deadzone(&s->rx, &s->ry, dz);
    int pad = p[0] == 0x47 ? 20 : 18; // timestamped reports insert a trackpad timestamp
    for (int i = 0; i < 2; i++, pad += 6) {
        s->pad[i] = (steam_pad){!!(b & (1u << (i ? 21 : 25))), steam_axis(p + pad), steam_axis(p + pad + 2), steam_u16(p + pad + 4) / 32768.0f};
    }
    s->tick_us = p[0] == 0x47 ? 32 : 1;
    s->tick = p[0] == 0x47 ? steam_u16(p + 32) : steam_u16(p + 30) | (uint32_t)steam_u16(p + 32) << 16;
    // SDL maps Triton (x,y,z) to (x,z,-y); Switch's (x,-z,y) basis undoes
    // that permutation. Gravity uses the opposite sign to SDL specific force.
    for (int i = 0; i < 3; i++) {
        s->acc[i] = -steam_i16(p + 34 + 2*i) * (2.0f / 32768);
        s->gyro[i] = steam_i16(p + 40 + 2*i) * (2000.0f / (32768 * 360.0f));
    }
    return 1;
}
static inline void steam_rumble_report(unsigned char out[10], uint16_t left, uint16_t right) {
    memset(out, 0, 10); out[0] = 0x80;
    out[4] = left; out[5] = left >> 8; out[7] = right; out[8] = right >> 8;
}
static inline void steam_imu_report(unsigned char out[64], int enabled) {
    memset(out, 0, 64); out[0] = 1; out[1] = 0x87; out[2] = 3; out[3] = 48; out[4] = enabled ? 0x18 : 0;
}
