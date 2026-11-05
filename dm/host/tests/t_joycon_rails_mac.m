// Production callbacks, fake physical devices; no HID manager is opened or scheduled.
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
@interface TestRails : NSObject
@property NSNumber *vendor;
@property NSNumber *product;
@property BOOL synthetic;
@property uint8_t *buffer;
@property IOHIDReportCallback callback;
@property void *context;
@end
@implementation TestRails
@end
static CFTypeRef test_property(IOHIDDeviceRef hid, CFStringRef key) {
    TestRails *d = (__bridge id)hid;
    if (CFEqual(key, CFSTR(kIOHIDGCSyntheticDeviceKey))) return d.synthetic ? kCFBooleanTrue : NULL;
    if (CFEqual(key, CFSTR(kIOHIDVendorIDKey))) return (__bridge CFTypeRef)d.vendor;
    if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return (__bridge CFTypeRef)d.product;
    return NULL;
}
static void test_register(IOHIDDeviceRef hid, uint8_t *buffer, CFIndex len, IOHIDReportCallback cb, void *ctx) {
    TestRails *d = (__bridge id)hid;
    assert(len >= 49 && buffer);
    if (!cb) assert(buffer == d.buffer && d.context);
    d.buffer = buffer; d.callback = cb; d.context = ctx;
}
static int schedules, opens, closes;
static IOReturn open_result;
static void test_schedule(IOHIDManagerRef m, CFRunLoopRef loop, CFStringRef mode) {
    assert(m && loop == CFRunLoopGetMain() && mode == kCFRunLoopCommonModes); schedules++;
}
static void test_unschedule(IOHIDManagerRef m, CFRunLoopRef loop, CFStringRef mode) {
    assert(m && loop == CFRunLoopGetMain() && mode == kCFRunLoopCommonModes); schedules--;
}
static IOReturn test_open(IOHIDManagerRef m, IOOptionBits options) {
    assert(m && options == kIOHIDOptionsTypeNone); opens++; return open_result;
}
static IOReturn test_close(IOHIDManagerRef m, IOOptionBits options) {
    assert(m && options == kIOHIDOptionsTypeNone); closes++; return kIOReturnSuccess;
}
#define IOHIDDeviceGetProperty test_property
#define IOHIDDeviceRegisterInputReportCallback test_register
#define IOHIDManagerScheduleWithRunLoop test_schedule
#define IOHIDManagerUnscheduleFromRunLoop test_unschedule
#define IOHIDManagerOpen test_open
#define IOHIDManagerClose test_close
#include "../joycon_rails_mac.h"
static int notifications;
static void changed(void) { notifications++; }
static TestRails *device(int product) {
    TestRails *d = [TestRails new]; d.vendor = @0x057e; d.product = @(product); return d;
}
static void found(TestRails *d) { joycon_rails_found(NULL, kIOReturnSuccess, NULL, (__bridge IOHIDDeviceRef)d); }
static void removed(TestRails *d) { joycon_rails_removed(NULL, kIOReturnSuccess, NULL, (__bridge IOHIDDeviceRef)d); }
static void report(TestRails *d, uint8_t right, uint8_t left) {
    assert(d.callback && d.buffer); memset(d.buffer, 0, 49);
    d.buffer[0] = 0x30; d.buffer[3] = right; d.buffer[5] = left;
    d.callback(d.context, kIOReturnSuccess, (__bridge void *)d, kIOHIDReportTypeInput, 0x30, d.buffer, 49);
}
static void state(int side, BOOL available, uint8_t expected) {
    uint8_t bits = 0xff;
    assert(dm_joycon_rails_state(side, &bits) == available && bits == expected);
}
int main(void) { @autoreleasepool {
    dm_joycon_rails_start(changed); assert(opens == 1 && schedules == 1 && notifications == 1);
    dm_joycon_rails_start(changed); assert(opens == 1);
    state(0, NO, 0); state(1, NO, 0); state(-1, NO, 0); state(2, NO, 0);
    TestRails *l = device(0x2006), *r = device(0x2007), *l2 = device(0x2006), *r2 = device(0x2007);
    TestRails *synthetic = device(0x2006); synthetic.synthetic = YES;
    TestRails *wrong = device(0x2009); found(synthetic); found(wrong); assert(!g_joycon_rails_devices);
    wrong.product = @0x2006; wrong.vendor = @1; found(wrong); assert(!g_joycon_rails_devices);
    found(l); found(l); state(0, NO, 0);
    int n = notifications; report(l, 0x30, 0); state(0, YES, 0); assert(notifications == n + 1);
    report(l, 0x30, 0); assert(notifications == n + 1);
    report(l, 0, 0x20); state(0, YES, 0x20); assert(notifications == n + 2);
    report(l, 0, 0); state(0, YES, 0); assert(notifications == n + 3);
    found(r); assert(l.buffer != r.buffer && l.context != r.context);
    report(r, 0x10, 0x20); state(1, YES, 0x10); state(0, YES, 0);
    found(l2); report(l2, 0, 0x30); report(l, 0, 0x20);
    state(0, NO, 0); state(1, YES, 0x10);
    removed(l2); assert(!l2.callback); state(0, YES, 0x20);
    found(r2); report(r2, 0x30, 0); state(1, NO, 0); state(0, YES, 0x20);
    removed(r2); state(1, YES, 0x10);
    n = notifications;
    l.callback(l.context, kIOReturnError, NULL, kIOHIDReportTypeInput, 0x30, l.buffer, 49);
    state(0, NO, 0); state(1, YES, 0x10); assert(notifications == n + 1);
    report(l, 0, 0x30); state(0, YES, 0x30);
    l.callback(l.context, kIOReturnSuccess, NULL, kIOHIDReportTypeInput, 0x21, l.buffer, 49);
    state(0, YES, 0x30);
    l.callback(l.context, kIOReturnSuccess, NULL, kIOHIDReportTypeInput, 0x30, l.buffer, 48);
    state(0, NO, 0);
    report(l, 0, 0x30); removed(l); state(0, NO, 0); assert(!l.callback);
    found(l); state(0, NO, 0); report(l, 0, 0); state(0, YES, 0);
    dm_joycon_rails_stop(); assert(!l.callback && !r.callback && schedules == 0 && closes == 1);
    state(0, NO, 0); state(1, NO, 0);
    n = notifications; dm_joycon_rails_stop(); assert(notifications == n);
    open_result = kIOReturnError;
    dm_joycon_rails_start(changed); assert(!g_joycon_rails_manager && schedules == 0 && closes == 2);
    open_result = kIOReturnSuccess;
    dm_joycon_rails_start(changed); dm_joycon_rails_stop();
    assert(!g_joycon_rails_devices && !g_joycon_rails_manager);
    puts("t_joycon_rails_mac: passed");
} }
