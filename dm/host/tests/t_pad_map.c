#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "../pad_map.h"
#include "../steam_input.h"
static steam_state decode_sticks(unsigned char report, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    unsigned char packet[46] = {0}; packet[0] = report;
    int16_t axes[] = {lx, ly, rx, ry};
    for (int i = 0; i < 4; i++) { uint16_t v = (uint16_t)axes[i]; packet[10+2*i] = v; packet[11+2*i] = v >> 8; }
    packet[39] = 0x40; packet[41] = 0x40; // motion must survive centred sticks
    int pad = report == 0x47 ? 20 : 18; packet[pad] = 3; packet[pad+2] = 4;
    steam_state s; assert(steam_decode(packet, sizeof packet, &s) == 1);
    assert(s.acc[2] == -1 && s.gyro[0] > 0 && s.pad[0].x == 3 && s.pad[0].y == 4);
    return s;
}
static void t_deadzone(void) {
    unsetenv("DM_STEAM_DEADZONE");
    for (int i = 0; i < 3; i++) {
        unsigned char report = (unsigned char[]){0x42,0x45,0x47}[i];
        steam_state s = decode_sticks(report, 7, -9, -13, 11);
        assert(s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0); // rest must not suppress gyro aiming
        s = decode_sticks(report, 2000, 2000, 3000, -3000);
        assert(s.lx == 0 && s.ly == 0 && s.rx > 0 && s.ry == -s.rx); // radial, not per-axis
        s = decode_sticks(report, 3276, 0, 3278, 0);
        assert(s.lx == 0 && s.ly == 0 && s.rx == 1 && s.ry == 0); // continuous at the boundary
        s = decode_sticks(report, 16384, 0, 12000, -16000);
        assert(s.lx == 14564 && s.ly == 0 && s.rx == 11149 && s.ry == -14865);
        s = decode_sticks(report, 32767, 0, -32768, 0);
        assert(s.lx == 32767 && s.rx == -32767); // full travel is retained
        s = decode_sticks(report, 32767, -32768, -32768, 32767);
        assert(s.lx == 32767 && s.ly == -32767 && s.rx == -32767 && s.ry == 32767);
        s = decode_sticks(report, 32767, 16384, -16384, -32768);
        assert(s.lx == 32767 && s.ly == 16384 && s.rx == -16384 && s.ry == -32767); // preserve asymmetric outer diagonals
    }
    setenv("DM_STEAM_DEADZONE", "0.2", 1);
    steam_state s = decode_sticks(0x42, 6000, 0, 16384, 0);
    assert(s.lx == 0 && s.rx == 12288);
    setenv("DM_STEAM_DEADZONE", "0.8", 1);
    s = decode_sticks(0x42, 32767, 16384, -16384, -32768);
    assert(s.lx == 32767 && s.ly == 16384 && s.rx == -16384 && s.ry == -32767);
    setenv("DM_STEAM_DEADZONE", "0", 1);
    s = decode_sticks(0x42, 7, -9, -13, 11);
    assert(s.lx == 7 && s.ly == -9 && s.rx == -13 && s.ry == 11);
    const char *invalid[] = {"", "garbage", "0junk", "nan", "inf", "-0.1", "0.9", "1"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        setenv("DM_STEAM_DEADZONE", invalid[i], 1);
        s = decode_sticks(0x42, 7, -9, 16384, 0);
        assert(s.lx == 0 && s.ly == 0 && s.rx == 14564); // invalid settings retain the safe default
    }
    unsetenv("DM_STEAM_DEADZONE");
}
int main(void) {
    t_deadzone();
    int map[DM_PAD_BUTTONS]; pad_physical_default(map, 0);
    assert(pad_map_buttons(1u << 0, map) == DM_BTN_A); // east -> Switch A
    pad_physical_default(map, 1);
    assert(pad_map_buttons(1u << 1, map) == DM_BTN_A); // south -> label A
    map[16] = 63; assert(pad_map_buttons(1u << 16, map) == DM_BTN_SHAKE);
    map[1] = -1; assert(pad_map_buttons(1u << 1, map) == 0);
    map[1] = 100; assert(pad_map_buttons(1u << 1, map) == 0);
    unsigned char packet[64] = {0x42, 1}; steam_state s = {0};
    // South A, L4, right trackpad click. Signed axes and half-pulled triggers.
    packet[2] = 1; packet[4] = 0x42;
    packet[7] = 0x40; packet[9] = 0x40;
    packet[10] = 0xff; packet[11] = 0x7f;
    packet[12] = 0; packet[13] = 0x80;
    for (int i = 0; i < 3; i++) {
        packet[0] = (unsigned char[]){0x42, 0x45, 0x47}[i];
        assert(steam_decode(packet, 46, &s) == 1);
        assert(s.buttons == ((1u << 1) | (1u << 16) | (1u << 21) | DM_BTN_ZL | DM_BTN_ZR));
        assert(s.lx == 32767 && s.ly == -32767 && s.rx == 0 && s.ry == 0);
        for (int n = 0; n < 46; n++) assert(steam_decode(packet, n, &s) == 0);
    }
    packet[0] = 0x79; packet[1] = 1; assert(steam_decode(packet, 2, &s) == -1);
    packet[0] = 0x46; assert(steam_decode(packet, 2, &s) == -1);
    packet[1] = 2; assert(steam_decode(packet, 2, &s) == 2);
    packet[0] = 0x43; assert(steam_decode(packet, 64, &s) == 0);
    assert(steam_device(0x28de, 0x1302, -1)); assert(steam_device(0x28de, 0x1303, -1));
    assert(steam_device(0x28de, 0x1304, 2)); assert(steam_device(0x28de, 0x1305, 5));
    assert(!steam_device(0x28de, 0x1304, 1)); assert(!steam_device(0x28de, 0x1142, 2));
    assert(!steam_device(0x1234, 0x1302, 2));
    for (int version = 0; version < 3; version++) {
        unsigned char imu[46] = {0}; imu[0] = (unsigned char[]){0x42,0x45,0x47}[version];
        imu[5] = 0x02; imu[4] = 0x20; // both trackpads touched
        int pad = version == 2 ? 20 : 18;
        imu[pad+1] = 0x40; imu[pad+3] = 0xc0; imu[pad+5] = 0x40;
        imu[pad+7] = 0x80; imu[pad+9] = 0x7f;
        int tick = version == 2 ? 32 : 30; imu[tick] = 0xfe; imu[tick+1] = 0xff;
        imu[39] = 0x40; imu[41] = 0x40; // +1G Z, +1000 degrees/second X
        assert(steam_decode(imu, sizeof imu, &s) == 1);
        assert(s.pad[0].down && s.pad[1].down && s.pad[0].x == 16384 && s.pad[0].y == -16384);
        assert(s.pad[1].x == -32767 && fabsf(s.pad[0].pressure - 0.5f) < 1e-5f);
        assert(s.acc[2] == -1 && fabsf(s.gyro[0] - 1000.0f/360) < 1e-5f);
        assert(s.tick == 65534 && s.tick_us == (version == 2 ? 32 : 1));
    }
    unsigned char output[10], feature[64];
    steam_rumble_report(output, 0x1234, 0xabcd);
    assert(output[0] == 0x80 && output[4] == 0x34 && output[5] == 0x12 && output[7] == 0xcd && output[8] == 0xab);
    assert(output[1] == 0 && output[2] == 0 && output[3] == 0 && output[6] == 0 && output[9] == 0);
    steam_imu_report(feature, 1); assert(feature[0] == 1 && feature[1] == 0x87 && feature[3] == 48 && feature[4] == 0x18);
    steam_imu_report(feature, 0); assert(feature[4] == 0);
    puts("pad mapping and Steam Controller reports: OK");
}
