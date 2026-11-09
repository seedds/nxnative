// 8BitDo Ultimate 2 Wireless over Bluetooth (2dc8:6012): GameController lists it with buttons only, no GCMotion and no haptics. Firmware 1.03 and later sends 34-byte
// input reports 0x01 whose bytes 15..26 are six little-endian int16: accelerometer x, y, z (4096 counts per G) and gyroscope x, y, z (2000 deg/s full scale), and takes a
// 5-byte output report 0x05 (strong motor, weak motor, two trigger motors this model lacks); firmware 1.02 sends 12-byte reports with neither (SDL's hidapi 8BitDo driver).
// Raw axes as the Pro Controller's (motion_pro.h): x toward the shoulders, y left, z out of the face, acceleration as specific force.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

enum { ABITDO_VID = 0x2DC8, ABITDO_U2W_BT = 0x6012, ABITDO_U2W_REPORT = 34 };
#define ABITDO_U2W_NAME "8BitDo Ultimate 2 Wireless"   // GameController's vendorName for it

// One report into the Switch frame (G, rad/s: x right, y forward, gravity (0, 0, -1) face up); 0 for anything but a sensor report
static inline int abitdo_motion_decode(const uint8_t *r, size_t len, float acc[3], float rot[3]) {
    if (len < ABITDO_U2W_REPORT || r[0] != 0x01) return 0;
    int16_t v[6];
    for (int i = 0; i < 6; i++) v[i] = (int16_t)((uint16_t)r[15 + 2 * i] | (uint16_t)r[16 + 2 * i] << 8);
    const float g = 1.f / 4096.f, rad = (float)(2000.0 * M_PI / 180.0 / 32767.0);
    acc[0] = v[1] * g; acc[1] = -v[0] * g; acc[2] = -v[2] * g;
    rot[0] = -v[4] * rad; rot[1] = v[3] * rad; rot[2] = v[5] * rad;
    return 1;
}
