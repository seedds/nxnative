// Exercise the actual Joy-Con HID transport with fake devices, including its
// volatile sensor commands, ownership, ambiguity, focus changes and removal.
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <assert.h>
#include <string.h>
#include "../dm_input.h"
@interface TestJoy : NSObject
@property int product;
@property uint8_t *buffer;
@property IOHIDReportCallback callback;
@property void *context;
@property int enables, disables;
@end
@implementation TestJoy
@end
static uint64_t tick = 1000000000;
static int received[2], cleared[2], last_player;
uint64_t now_ns(void) { return tick; }
void dm_motion_set_joycon(int p, int side, const float a[3], const float w[3], uint64_t ns) {
    assert(side >= 0 && side < 2 && a[2] == -1 && ns <= tick);
    assert(w[0] == 0 && w[1] == 0 && w[2] == 0);
    last_player = p; received[side]++;
}
void dm_motion_clear_joycon(int p, int side) { (void)p; cleared[side]++; }
static CFTypeRef property(IOHIDDeviceRef hid, CFStringRef key) {
    TestJoy *d = (__bridge id)hid;
    if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return (__bridge CFTypeRef)@(d.product);
    return NULL;
}
static void callback(IOHIDDeviceRef hid, uint8_t *b, CFIndex len, IOHIDReportCallback cb, void *ctx) {
    TestJoy *d = (__bridge id)hid; assert(len >= 49); d.buffer = b; d.callback = cb; d.context = ctx;
}
static IOReturn output(IOHIDDeviceRef hid, IOHIDReportType type, CFIndex rid, const uint8_t *b, CFIndex len) {
    TestJoy *d = (__bridge id)hid;
    assert(type == kIOHIDReportTypeOutput && rid == 1 && b[0] == 1);
    if (b[10] == 0x10) {
        assert(len == 16 && !memcmp(b + 11, (uint8_t[]){0x20,0x60,0,0,24}, 5));
        uint8_t reply[49] = {0x21}; reply[13] = 0x90; reply[14] = 0x10; reply[15] = 0x20; reply[16] = 0x60; reply[19] = 24;
        for (int i = 0; i < 3; i++) { reply[20+6+2*i+1] = 0x40; reply[20+18+2*i] = 0x3b; reply[20+18+2*i+1] = 0x34; }
        d.callback(d.context, 0, hid, kIOHIDReportTypeInput, 0x21, reply, sizeof reply);
        return 0;
    }
    assert(len == 12 && b[10] == 0x40);
    assert(!memcmp(b + 2, (uint8_t[]){0,1,0x40,0x40,0,1,0x40,0x40}, 8));
    if (b[11]) d.enables++; else d.disables++;
    return kIOReturnSuccess;
}
#define LOG(...) ((void)0)
#define IOHIDDeviceGetProperty property
#define IOHIDDeviceRegisterInputReportCallback callback
#define IOHIDDeviceSetReport output
#include "../motion_joy_mac.h"
#undef IOHIDDeviceGetProperty
#undef IOHIDDeviceRegisterInputReportCallback
#undef IOHIDDeviceSetReport
static IOHIDDeviceRef hid(TestJoy *d) { return (__bridge IOHIDDeviceRef)d; }
static TestJoy *device(int pid) { TestJoy *d = [TestJoy new]; d.product = pid; joy_found(NULL, 0, NULL, hid(d)); return d; }
static void packet(TestJoy *d, int data) {
    memset(d.buffer, 0, 49); d.buffer[0] = 0x30; d.buffer[1] = (uint8_t)(tick / 15000000);
    if (data) for (int k = 0; k < 3; k++) {
        int16_t z = d.product == 0x2007 ? -4096 : 4096;
        d.buffer[17 + 12 * k] = (uint8_t)z; d.buffer[18 + 12 * k] = (uint16_t)z >> 8;
    }
    d.callback(d.context, 0, hid(d), kIOHIDReportTypeInput, 0x30, d.buffer, 49);
    tick += 15000000;
}
int main(void) { @autoreleasepool {
    // Factory offsets remove a real stationary bias without learning from motion.
    uint8_t factory[24] = {0};
    for (int i = 0; i < 3; i++) {
        factory[6+2*i+1] = 0x40; factory[18+2*i] = 0x3b; factory[18+2*i+1] = 0x34;
        int16_t off = i == 2 ? -39 : -28; factory[12+2*i] = (uint8_t)off; factory[13+2*i] = (uint16_t)off >> 8;
    }
    joy_calibration cal = {0}; assert(joy_calibration_parse(&cal, factory, sizeof factory));
    uint8_t raw[49] = {0x30}; raw[1] = 1;
    for (int k = 0; k < 3; k++) {
        raw[18+12*k] = 0xf0; // right sensor face-up specific force -4096
        for (int i = 0; i < 3; i++) { raw[19+12*k+2*i] = factory[12+2*i]; raw[20+12*k+2*i] = factory[13+2*i]; }
    }
    pro_motion motion = {0}; pro_motion_sample samples[3];
    assert(joy_motion_decode(&motion, 1, raw, sizeof raw, tick, samples, &cal) == 3);
    for (int k = 0; k < 3; k++) assert(samples[k].rot[0] == 0 && samples[k].rot[1] == 0 && samples[k].rot[2] == 0 && samples[k].acc[2] == -1);
    joy_calibration old = cal; memset(factory, 0, sizeof factory);
    assert(!joy_calibration_parse(&cal, factory, sizeof factory) && !memcmp(&cal, &old, sizeof cal));
    TestJoy *right = device(0x2007), *left = device(0x2006);
    dm_motion_joy_players(1, 2);
    packet(left, 0); packet(right, 0); packet(left, 0);
    assert(left.enables == 1 && right.enables == 1);   // no repeated enable commands while waiting
    packet(left, 1); packet(right, 1);
    assert(received[0] == 3 && received[1] == 3 && last_player == 2);
    dm_motion_joy_active(0);
    assert(left.disables == 1 && right.disables == 1);
    packet(left, 1); assert(received[0] == 3);
    dm_motion_joy_active(1); packet(left, 1);   // already-live sensor belongs to another client
    dm_motion_joy_active(0); assert(left.disables == 1);
    dm_motion_joy_active(1); packet(left, 0); assert(left.enables == 2);
    TestJoy *extra = device(0x2006);   // cannot identify which physical left belongs to the pair
    assert(g_joy_devices->player == -1 && left.disables == 2);
    joy_removed(NULL, 0, NULL, hid(extra));
    packet(left, 0); assert(left.enables == 3);
    joy_removed(NULL, 0, NULL, hid(left));
    assert(left.callback == NULL && g_joy_devices->player == -1);
    joy_removed(NULL, 0, NULL, hid(right));
    assert(!g_joy_devices && right.callback == NULL && cleared[0] && cleared[1]);
    puts("Joy-Con transport: routing, axes, samples, ownership, focus and removal passed");
    return 0;
} }
