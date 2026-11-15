// Identical pads (pad_hid_mac.h) with fake IOKit entries and fake GameController controllers: only an entry that sends reports counts as a pad of its model, so one
// Pro Controller with a second, silent entry stays with GameController (GitHub issue #2237), while two that both stream are read as twins (GitHub issue #1218)
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../dm_input.h"

@interface TestDev : NSObject
@property int vid, pid; @property long loc; @property uint64_t entry; @property NSString *transport;
@property uint8_t *buffer; @property IOHIDReportCallback callback; @property void *context;
@end
@implementation TestDev
@end
@interface TestGC : NSObject
@property NSString *productCategory, *vendorName; @property id extendedGamepad;
@end
@implementation TestGC
static NSMutableArray *g_gc;
+ (NSArray *)controllers { return g_gc; }
+ (BOOL)supportsHIDDevice:(IOHIDDeviceRef)d { (void)d; return YES; }
@end

static CFTypeRef test_property(IOHIDDeviceRef device, CFStringRef key) {
    TestDev *d = (__bridge id)device;
    if (CFEqual(key, CFSTR(kIOHIDProductKey))) return (__bridge CFTypeRef)@"Pro Controller";
    if (CFEqual(key, CFSTR(kIOHIDVendorIDKey))) return (__bridge CFTypeRef)@(d.vid);
    if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return (__bridge CFTypeRef)@(d.pid);
    if (CFEqual(key, CFSTR(kIOHIDLocationIDKey))) return (__bridge CFTypeRef)@(d.loc);
    if (CFEqual(key, CFSTR(kIOHIDTransportKey))) return (__bridge CFTypeRef)d.transport;
    return NULL;
}
static void test_report_cb(IOHIDDeviceRef device, uint8_t *buffer, CFIndex len, IOHIDReportCallback callback, void *ctx) {
    TestDev *d = (__bridge id)device; assert(len >= 49);
    d.buffer = buffer; d.callback = callback; d.context = ctx;
}
static void test_value_cb(IOHIDDeviceRef device, IOHIDValueCallback callback, void *ctx) { (void)device; (void)callback; (void)ctx; }
static CFArrayRef test_elements(IOHIDDeviceRef device, CFDictionaryRef match, IOOptionBits o) { (void)device; (void)match; (void)o; return CFBridgingRetain(@[]); }
static io_service_t test_service(IOHIDDeviceRef device) { return (io_service_t)((__bridge TestDev *)device).entry; }
static kern_return_t test_entry_id(io_registry_entry_t e, uint64_t *id) { *id = e; return KERN_SUCCESS; }
static IOReturn test_set_report(IOHIDDeviceRef d, IOHIDReportType t, CFIndex id, const uint8_t *r, CFIndex n) { (void)d; (void)t; (void)id; (void)r; (void)n; return kIOReturnSuccess; }

static uint64_t tick = 1000000000;
uint64_t now_ns(void) { return tick; }
#define LOG(...) (printf(__VA_ARGS__), putchar('\n'))
#define IOHIDDeviceGetProperty test_property
#define IOHIDDeviceRegisterInputReportCallback test_report_cb
#define IOHIDDeviceRegisterInputValueCallback test_value_cb
#define IOHIDDeviceCopyMatchingElements test_elements
#define IOHIDDeviceGetService test_service
#define IORegistryEntryGetRegistryEntryID test_entry_id
#define IOHIDDeviceSetReport test_set_report
#define GCController TestGC
#include "../pad_hid_mac.h"

static int attached, changes;
static void hidpad_attach(hid_pad *d) { (void)d; attached++; }
static void hidpad_detach(hid_pad *d) { (void)d; attached--; }
static void hidpad_read(hid_pad *d) { (void)d; }
static void hidpad_motion(hid_pad *d, const pro_motion_sample *s, int n) { (void)d; (void)s; (void)n; }
static void hidpad_models_changed(void) { changes++; hidpad_twins_eval(nil); hidpad_twins_release(); hidpad_twins_take(nil); }   // front_mac.m's pads_changed, without its slots

static IOHIDDeviceRef ref(TestDev *d) { return (__bridge IOHIDDeviceRef)d; }
static TestDev *pro(long loc, uint64_t entry, NSString *transport) { TestDev *d = [TestDev new]; d.vid = 0x057e; d.pid = 0x2009; d.loc = loc; d.entry = entry; d.transport = transport; return d; }
static void drain(void) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false); }   // the dispatch_async'd recounts
static void report(TestDev *d) {
    assert(d.callback && d.buffer); memset(d.buffer, 0, 49); d.buffer[0] = 0x30;
    d.callback(d.context, kIOReturnSuccess, ref(d), kIOHIDReportTypeInput, 0x30, d.buffer, 49);
    tick += 15000000; drain();
}
static int twins(void) { int n = 0; for (hid_pad *d = g_hidpads; d; d = d->next) n += d->twin; return n; }
static hid_pad *find(TestDev *t) { for (int pass = 0; pass < 2; pass++) for (hid_pad *d = pass ? g_hidpads : g_hidpads_gc; d; d = d->next) if (d->hid == ref(t)) return d; return NULL; }

int main(void) { @autoreleasepool {
    g_gc = [NSMutableArray new];
    TestGC *gc = [TestGC new]; gc.productCategory = @"Switch Pro Controller"; gc.vendorName = @"Pro Controller"; gc.extendedGamepad = @1;
    // issue #2237: one Pro Controller over Bluetooth, which GameController lists, and a second entry of its model at location 0 that sends nothing
    [g_gc addObject:gc];
    TestDev *bt = pro(0x69aabbcc, 11, @"Bluetooth"), *ghost = pro(0, 12, @"Bluetooth");
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(bt)); hidpad_found(NULL, kIOReturnSuccess, NULL, ref(ghost));
    assert(find(bt) && find(ghost) && bt.callback && ghost.callback);   // both listened to
    report(bt); report(bt);
    g_twins_due = 1; hidpad_models_changed(); g_twins_due = 0;           // past the twin wait
    assert(twins() == 0 && attached == 0 && !hidpad_gc_unread(gc));     // GameController keeps reading the one pad
    assert(find(ghost)->shown == 1);                                    // named once as not counted
    // a pad that stops streaming without being removed is not counted at the next change: the old entry of a reconnected pad
    TestDev *bt2 = pro(0x69aabbcc, 13, @"Bluetooth");
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(bt2));
    tick += PRO_SILENT_NS; report(bt2);
    g_twins_due = 1; hidpad_models_changed(); g_twins_due = 0;
    assert(twins() == 0 && !hidpad_gc_unread(gc));
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(bt)); hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(ghost)); hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(bt2));
    assert(!g_hidpads && !g_hidpads_gc);
    // issue #1218: two Pro Controllers over USB that GameController lists as one; both stream, so both are read here, and the second only once it sends
    TestDev *u1 = pro(0x01100000, 21, @"USB"), *u2 = pro(0x01200000, 22, @"USB");
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(u1)); hidpad_found(NULL, kIOReturnSuccess, NULL, ref(u2));
    report(u1);
    g_twins_due = 1; hidpad_models_changed(); g_twins_due = 0;
    assert(twins() == 0);                                               // u2 has sent nothing yet
    report(u2);
    g_twins_due = 1; hidpad_models_changed(); g_twins_due = 0;
    assert(twins() == 2 && attached == 2 && hidpad_gc_unread(gc) && g_hidpads->hid == ref(u2));   // by USB location: u1 taken first, so u2 is at the head
    report(u1); report(u2);
    // one falls silent (no removal): at the next change it is not counted, so both go back to GameController
    tick += PRO_SILENT_NS; report(u2);
    hidpad_models_changed();
    assert(twins() == 0 && attached == 0 && !hidpad_gc_unread(gc));
    report(u1); g_twins_due = 1; hidpad_models_changed(); g_twins_due = 0;   // it streams again: read here again
    assert(twins() == 2 && attached == 2);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(u1));
    assert(twins() == 0 && attached == 0);                              // one left: back to GameController
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(u2));
    assert(!g_hidpads && !g_hidpads_gc);
    printf("t_pad_hid_mac: passed (%d recounts)\n", changes);
} }
