// Identical pads (pad_hid_mac.h) with fake IOKit entries and fake GameController controllers: only an entry that sends reports counts as a pad of its model, so one
// Pro Controller with a second, silent entry stays with GameController (GitHub issue #2237), while two that both stream are read as twins (GitHub issue #1218). A pad with
// Nintendo's report descriptor that GameController does not list (a PowerA NANO, PR #989) is asked for its device info, gets the handshake once it answers, is read from its
// report 0x30, and falls back to its report 0x3F when it does not answer or leaves full mode. Virtual devices must actually appear as usable GameController controllers,
// regardless of their vendor, or be read as HID after the wait (Steam Link, issue #2465).
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
@property NSNumber *virtualDevice; @property BOOL synthetic;
@property IOHIDValueCallback valueCallback; @property void *valueContext;
@property NSString *name; @property NSArray *elements;
@end
@implementation TestDev
@end
@interface TestElem : NSObject
@property uint32_t page, usage, report, reportSize; @property int output;
@property CFIndex min, max; @property BOOL relative; @property TestElem *parent;
@end
@implementation TestElem
@end
static TestElem *elem(int output, uint32_t report, uint32_t page, uint32_t usage) {
    TestElem *e = [TestElem new]; e.output = output; e.report = report; e.page = page; e.usage = usage; e.max = 1; return e;
}
@interface TestValue : NSObject
@property TestElem *element; @property CFIndex value;
@end
@implementation TestValue
@end
@interface TestGC : NSObject
@property NSString *productCategory, *vendorName; @property id extendedGamepad;
@end
@implementation TestGC
static NSMutableArray *g_gc; static BOOL g_supports = YES;
+ (NSArray *)controllers { return g_gc; }
+ (BOOL)supportsHIDDevice:(IOHIDDeviceRef)d { (void)d; return g_supports; }
@end

static CFTypeRef test_property(IOHIDDeviceRef device, CFStringRef key) {
    TestDev *d = (__bridge id)device;
    if (CFEqual(key, CFSTR(kIOHIDVirtualHIDevice))) return (__bridge CFTypeRef)d.virtualDevice;
    if (CFEqual(key, CFSTR(kIOHIDGCSyntheticDeviceKey))) return d.synthetic ? kCFBooleanTrue : NULL;
    if (CFEqual(key, CFSTR(kIOHIDProductKey))) return (__bridge CFTypeRef)(d.name ?: @"Pro Controller");
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
static void test_value_cb(IOHIDDeviceRef device, IOHIDValueCallback callback, void *ctx) { TestDev *d = (__bridge id)device; d.valueCallback = callback; d.valueContext = ctx; }
static IOHIDElementRef test_val_element(IOHIDValueRef v) { return (__bridge IOHIDElementRef)((__bridge TestValue *)v).element; }
static CFIndex test_val_int(IOHIDValueRef v) { return ((__bridge TestValue *)v).value; }
static CFIndex test_el_min(IOHIDElementRef e) { return ((__bridge TestElem *)e).min; }
static CFIndex test_el_max(IOHIDElementRef e) { return ((__bridge TestElem *)e).max; }
static CFArrayRef test_elements(IOHIDDeviceRef device, CFDictionaryRef match, IOOptionBits o) { (void)match; (void)o; return CFBridgingRetain(((__bridge TestDev *)device).elements ?: @[]); }
static uint32_t test_el_page(IOHIDElementRef e) { return ((__bridge TestElem *)e).page; }
static uint32_t test_el_usage(IOHIDElementRef e) { return ((__bridge TestElem *)e).usage; }
static uint32_t test_el_report(IOHIDElementRef e) { return ((__bridge TestElem *)e).report; }
static uint32_t test_el_report_size(IOHIDElementRef e) { return ((__bridge TestElem *)e).reportSize; }
static Boolean test_el_relative(IOHIDElementRef e) { return ((__bridge TestElem *)e).relative ? true : false; }
static IOHIDElementRef test_el_parent(IOHIDElementRef e) { return (__bridge IOHIDElementRef)((__bridge TestElem *)e).parent; }
static IOHIDElementType test_el_type(IOHIDElementRef e) { return ((__bridge TestElem *)e).output ? kIOHIDElementTypeOutput : kIOHIDElementTypeInput_Button; }
static io_service_t test_service(IOHIDDeviceRef device) { return (io_service_t)((__bridge TestDev *)device).entry; }
static kern_return_t test_entry_id(io_registry_entry_t e, uint64_t *id) { *id = e; return KERN_SUCCESS; }
static uint8_t outs[64][3]; static int nouts;   // each output report: its id, subcommand and first argument byte
static IOReturn test_set_report(IOHIDDeviceRef d, IOHIDReportType t, CFIndex id, const uint8_t *r, CFIndex n) {
    (void)d; assert(t == kIOHIDReportTypeOutput && id == r[0] && n >= 12 && nouts < 64);
    outs[nouts][0] = r[0]; outs[nouts][1] = r[10]; outs[nouts][2] = r[11]; nouts++; return kIOReturnSuccess;
}

static uint64_t tick = 1000000000;
uint64_t now_ns(void) { return tick; }
// The device and clock are synthetic; drive its one-shot timers by deadline too.
// Real dispatch timers can run late under CI load, so seven handshake steps did
// not reliably finish in the test's 100 ms wall-clock window.
static dispatch_time_t timer_now = 1, timer_due[128];
static __strong dispatch_block_t timer_block[128]; static int ntimers;
static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta) {
    assert(when == DISPATCH_TIME_NOW && delta >= 0); return timer_now + (uint64_t)delta;
}
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block) {
    assert(queue == dispatch_get_main_queue() && ntimers < 128);
    timer_due[ntimers] = when; timer_block[ntimers++] = [block copy];
}
static void advance_timers(uint64_t ns) {
    dispatch_time_t end = timer_now + ns;
    for (;;) {
        int first = -1;
        for (int i = 0; i < ntimers; i++) if (timer_due[i] <= end && (first < 0 || timer_due[i] < timer_due[first])) first = i;
        if (first < 0) break;
        tick += timer_due[first] - timer_now; timer_now = timer_due[first];
        dispatch_block_t block = timer_block[first];
        for (int i = first; i + 1 < ntimers; i++) { timer_due[i] = timer_due[i + 1]; timer_block[i] = timer_block[i + 1]; }
        timer_block[--ntimers] = nil;
        block();
    }
    tick += end - timer_now; timer_now = end;
}
#define LOG(...) (printf(__VA_ARGS__), putchar('\n'))
#define IOHIDDeviceGetProperty test_property
#define IOHIDDeviceRegisterInputReportCallback test_report_cb
#define IOHIDDeviceRegisterInputValueCallback test_value_cb
#define IOHIDValueGetElement test_val_element
#define IOHIDValueGetIntegerValue test_val_int
#define IOHIDElementGetLogicalMin test_el_min
#define IOHIDElementGetLogicalMax test_el_max
#define IOHIDDeviceCopyMatchingElements test_elements
#define IOHIDDeviceGetService test_service
#define IORegistryEntryGetRegistryEntryID test_entry_id
#define IOHIDDeviceSetReport test_set_report
#define IOHIDElementGetUsagePage test_el_page
#define IOHIDElementGetUsage test_el_usage
#define IOHIDElementGetReportID test_el_report
#define IOHIDElementGetReportSize test_el_report_size
#define IOHIDElementIsRelative test_el_relative
#define IOHIDElementGetParent test_el_parent
#define IOHIDElementGetType test_el_type
#define GCController TestGC
#define dispatch_time test_dispatch_time
#define dispatch_after test_dispatch_after
#define HIDPAD_NIN_STEP_NS 2000000ull
#define HIDPAD_NIN_WAIT_NS 200000000ull
#include "../pad_hid_mac.h"
#undef dispatch_time
#undef dispatch_after

static int attached, changes;
static void hidpad_attach(hid_pad *d) { (void)d; attached++; }
static void hidpad_detach(hid_pad *d) { (void)d; attached--; }
static int reads;
static uint64_t buttons;
static void hidpad_read(hid_pad *d) { reads++; buttons = pad_hid_buttons(&d->st); }
static int motion_samples;
static float motion_acc[3], motion_rot[3];
static uint64_t motion_ns;
static void hidpad_motion(hid_pad *d, const float acc[3], const float rot[3], uint64_t ns) {
    (void)d; assert(ns > motion_ns); motion_ns = ns; motion_samples++;
    memcpy(motion_acc, acc, sizeof motion_acc); memcpy(motion_rot, rot, sizeof motion_rot);
}
static int player = -1;
static int hidpad_player(const hid_pad *d) { (void)d; return player; }
static void hidpad_models_changed(void) { changes++; hidpad_twins_eval(nil); hidpad_twins_release(); hidpad_twins_take(nil); }   // front_mac.m's pads_changed, without its slots

static IOHIDDeviceRef ref(TestDev *d) { return (__bridge IOHIDDeviceRef)d; }
static TestDev *pro(long loc, uint64_t entry, NSString *transport) { TestDev *d = [TestDev new]; d.vid = 0x057e; d.pid = 0x2009; d.loc = loc; d.entry = entry; d.transport = transport; return d; }
static void drain(void) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false); }   // the dispatch_async'd recounts
static void report(TestDev *d) {
    assert(d.callback && d.buffer); memset(d.buffer, 0, 49); d.buffer[0] = 0x30;
    d.callback(d.context, kIOReturnSuccess, ref(d), kIOHIDReportTypeInput, 0x30, d.buffer, 49);
    tick += 15000000; drain();
}
static void push_report(TestDev *d, const uint8_t *r, size_t n) {
    assert(d.callback && d.buffer); memcpy(d.buffer, r, n);
    d.callback(d.context, kIOReturnSuccess, ref(d), kIOHIDReportTypeInput, r[0], d.buffer, (CFIndex)n);
    tick += 15000000; drain();
}
static void wait_ms(int ms) { advance_timers((uint64_t)ms * NSEC_PER_MSEC); drain(); }
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)((uint16_t)v >> 8); }
static int twins(void) { int n = 0; for (hid_pad *d = g_hidpads; d; d = d->next) n += d->twin; return n; }
static hid_pad *find(TestDev *t) { for (int pass = 0; pass < 2; pass++) for (hid_pad *d = pass ? g_hidpads : g_hidpads_gc; d; d = d->next) if (d->hid == ref(t)) return d; return NULL; }

static TestDev *virtual_pad(NSString *name, int vid, uint64_t entry) {
    TestDev *d = [TestDev new]; d.name = name; d.vid = vid; d.pid = 0x028e; d.virtualDevice = @YES; d.entry = entry;
    d.elements = @[elem(0, 1, kHIDPage_Button, 1)]; return d;
}
static TestGC *controller(NSString *name, int usable) {
    TestGC *c = [TestGC new]; c.vendorName = name; c.extendedGamepad = usable ? @1 : nil; [g_gc addObject:c]; return c;
}
static void push_value(TestDev *d, TestElem *element, CFIndex value) {
    assert(d.valueCallback);
    TestValue *v = [TestValue new]; v.element = element; v.value = value;
    int before = reads; d.valueCallback(d.valueContext, kIOReturnSuccess, ref(d), (__bridge IOHIDValueRef)v);
    assert(reads == before + 1);
}
static void button(TestDev *d, int down) {
    push_value(d, d.elements[0], down);
    assert(buttons == (down ? DM_BTN_B : 0));
}
static void gc_fallback(void) {
    // No GameController delivery: all virtual vendors use the existing wait, even the trusted physical-controller vendors. No Steam name or product gate.
    NSArray *virtuals = @[virtual_pad(@"GamePad-1", 0x045e, 100), virtual_pad(@"Virtual Sony", 0x054c, 101),
        virtual_pad(@"Virtual Nintendo", 0x057e, 102), virtual_pad(@"Other virtual pad", 0x1234, 103)];
    for (TestDev *d in virtuals) hidpad_found(NULL, kIOReturnSuccess, NULL, ref(d));
    // Missing and false virtual flags both retain physical-controller trust, including a HID name that differs from GameController's.
    NSArray *physical = @[pro(0x41, 104, @"Bluetooth"), virtual_pad(@"Wireless Controller", 0x054c, 105), virtual_pad(@"Xbox pad", 0x045e, 106)];
    for (TestDev *d in physical) { d.virtualDevice = d.vid == 0x054c ? @NO : nil; hidpad_found(NULL, kIOReturnSuccess, NULL, ref(d)); assert(find(d) && !d.valueCallback); }
    // Apple's synthetic devices stay excluded; a device removed during the wait must never acquire a reader.
    TestDev *synthetic = virtual_pad(@"Synthetic pad", 0x045e, 107); synthetic.synthetic = YES;
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(synthetic)); assert(!find(synthetic));
    TestDev *removed = virtual_pad(@"Removed pad", 0x045e, 108);
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(removed)); hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(removed));
    assert(attached == 0);
    wait_ms(2200);
    assert(attached == 4 && !g_hidpads_waiting && !removed.valueCallback && !synthetic.valueCallback);
    for (TestDev *d in virtuals) {
        assert(find(d) && !find(d)->twin); button(d, 1); button(d, 0);
        hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(d)); assert(!d.valueCallback);
    }
    for (TestDev *d in physical) { assert(!d.valueCallback); hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(d)); }
    assert(attached == 0 && !g_hidpads && !g_hidpads_gc);

    // An already usable controller and one published during the wait stay with GameController. An unusable matching controller or an unrelated usable one cannot vouch.
    TestDev *listed = virtual_pad(@"Listed pad", 0x045e, 109), *before = virtual_pad(@"Before timeout", 0x045e, 110);
    TestDev *unusable = virtual_pad(@"Unusable pad", 0x045e, 111), *after = virtual_pad(@"After timeout", 0x045e, 112);
    controller(@"Listed pad", 1); TestGC *no_profile = controller(@"Unusable pad", 0);
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(listed)); assert(find(listed) && !g_hidpads_waiting);
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(before));
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(unusable)); hidpad_found(NULL, kIOReturnSuccess, NULL, ref(after));
    TestGC *early = controller(@"Before timeout", 1); hidpad_gc_connected(early);
    wait_ms(2200);
    assert(attached == 2 && !g_hidpads_waiting && !listed.valueCallback && !before.valueCallback);
    button(unusable, 1); button(after, 1);
    // A late notification without the profile front_mac.m reads must not remove the working HID reader.
    hidpad_gc_connected(no_profile); assert(attached == 2 && unusable.valueCallback); button(unusable, 0);
    no_profile.extendedGamepad = @1; hidpad_gc_connected(no_profile);
    assert(attached == 1 && !unusable.valueCallback && find(unusable));
    TestGC *late = controller(@"After timeout", 1); hidpad_gc_connected(late);
    assert(attached == 0 && !after.valueCallback && find(after));
    for (TestDev *d in @[listed, before, unusable, after]) hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(d));
    [g_gc removeAllObjects];
    assert(!g_hidpads && !g_hidpads_gc && !g_hidpads_waiting);
    puts("t_pad_hid_mac: virtual fallback and GameController handoff passed");
}

static void hid_layouts(void) {
    TestElem *x = elem(0, 1, 1, 0x30), *y = elem(0, 1, 1, 0x31);
    TestElem *z = elem(0, 1, 1, 0x32), *rz = elem(0, 1, 1, 0x35);
    TestElem *rx = elem(0, 1, 1, 0x33), *ry = elem(0, 1, 1, 0x34);
    TestElem *face = elem(0, 1, 9, 3), *simple = elem(0, 0x3F, 9, 3);
    TestElem *brake = elem(0, 1, 2, 0xC5), *accel = elem(0, 1, 2, 0xC4);
    TestDev *steam = virtual_pad(@"Remote controller", 0x045e, 113);
    TestDev *other = virtual_pad(@"Xbox ID with dedicated triggers", 0x045e, 114);
    TestDev *nin = virtual_pad(@"Xbox ID with Nintendo report", 0x045e, 115);
    steam.elements = @[x, y, z, rz, rx, ry, face];
    other.elements = @[x, y, rx, ry, brake, accel, face];
    nin.elements = @[x, y, z, rz, rx, ry, simple];   // report 0x3F discovered after the axes
    for (TestDev *d in @[steam, other, nin]) hidpad_found(NULL, kIOReturnSuccess, NULL, ref(d));
    wait_ms(2200);

    int32_t s[4];
    push_value(steam, face, 1); assert(buttons == DM_BTN_Y);
    push_value(steam, face, 0); assert(buttons == 0);
    push_value(steam, rx, 1);
    push_value(steam, z, 1); assert(buttons == DM_BTN_ZL);
    push_value(steam, rz, 1); assert(buttons == (DM_BTN_ZL | DM_BTN_ZR));
    pad_hid_sticks(&find(steam)->st, s); assert(s[2] == DM_STICK_MAX && s[3] == 0);
    push_value(steam, z, 0); assert(buttons == DM_BTN_ZR);
    push_value(steam, rz, 0); assert(buttons == 0);

    push_value(other, face, 1); assert(buttons == 0);   // the default map has no usage 3
    push_value(other, brake, 1); assert(buttons == DM_BTN_ZL);
    push_value(other, accel, 1); assert(buttons == (DM_BTN_ZL | DM_BTN_ZR));
    push_value(other, brake, 0); push_value(other, accel, 0); assert(buttons == 0);

    push_value(nin, simple, 1); assert(buttons == DM_BTN_Y);
    push_value(nin, z, 1); push_value(nin, rz, 1); assert(buttons == DM_BTN_Y);
    pad_hid_sticks(&find(nin)->st, s); assert(s[2] == DM_STICK_MAX && s[3] == -DM_STICK_MAX);
    for (TestDev *d in @[steam, other, nin]) hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(d));
    assert(!attached && !g_hidpads && !g_hidpads_gc && !g_hidpads_waiting);

    // The PS4-mode Fighting Stick Alpha (0f0d:011c) also declares button usages 1–14, in input report 1. See PR #3061 and docs/reference/services.md.
    NSMutableArray *taiko_buttons = [NSMutableArray array], *ps4_buttons = [NSMutableArray array];
    for (uint32_t u = 1; u <= 14; u++) {
        [taiko_buttons addObject:elem(0, 0, kHIDPage_Button, u)];
        [ps4_buttons addObject:elem(0, 1, kHIDPage_Button, u)];
    }
    TestDev *drum = virtual_pad(@"Taiko Controller", 0x0f0d, 116), *stick = virtual_pad(@"Fighting Stick Alpha (PS4 mode)", 0x0f0d, 117);
    drum.pid = 0x00f0; stick.pid = 0x011c; drum.virtualDevice = stick.virtualDevice = nil;
    drum.elements = [@[elem(0, 0, 1, 0x30), elem(0, 0, 1, 0x31), elem(0, 0, 1, 0x32), elem(0, 0, 1, 0x35), elem(0, 0, 1, 0x39)]
        arrayByAddingObjectsFromArray:taiko_buttons];
    stick.elements = [@[x, y, z, rz, elem(0, 1, 1, 0x39), rx, ry] arrayByAddingObjectsFromArray:ps4_buttons];
    for (TestDev *d in @[drum, stick]) hidpad_found(NULL, kIOReturnSuccess, NULL, ref(d));
    wait_ms(2200);
    push_value(drum, taiko_buttons[2], 1); assert(buttons == DM_BTN_A);
    push_value(drum, taiko_buttons[2], 0); assert(buttons == 0);
    push_value(drum, taiko_buttons[9], 1); assert(buttons == DM_BTN_PLUS);
    push_value(drum, taiko_buttons[9], 0); assert(buttons == 0);
    push_value(stick, ps4_buttons[2], 1); assert(buttons == 0);   // the default map has no usage 3
    push_value(stick, ps4_buttons[0], 1); assert(buttons == DM_BTN_B);
    push_value(stick, ps4_buttons[0], 0); assert(buttons == 0);
    for (TestDev *d in @[drum, stick]) hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(d));
    assert(!attached && !g_hidpads && !g_hidpads_gc && !g_hidpads_waiting);
    puts("t_pad_hid_mac: Xbox axis matching, Nintendo report precedence and Taiko Drum layout passed");
}

static void push_ignored_value(TestDev *d, TestElem *element, CFIndex value) {
    assert(d.valueCallback);
    TestValue *v = [TestValue new]; v.element = element; v.value = value;
    int before = reads; d.valueCallback(d.valueContext, kIOReturnSuccess, ref(d), (__bridge IOHIDValueRef)v);
    assert(reads == before);
}
static void hid_composite_and_wrapped(void) {
    // Composite gamepad (modeled on SHIELD 2015, tested with an unrelated VID:PID to prove no device gate):
    // report 1 GamePad collection (with sticks under a standard Physical Pointer collection), wrapped -1..0 16-bit axes,
    // Consumer AC Back inside GamePad, sibling Mouse collection (relative X/Y + mouse click), and sibling standalone Consumer Control collection.
    TestElem *col_pad = elem(0, 0, kHIDPage_GenericDesktop, kHIDUsage_GD_GamePad);
    TestElem *col_ptr = elem(0, 0, kHIDPage_GenericDesktop, kHIDUsage_GD_Pointer); col_ptr.parent = col_pad;
    TestElem *col_mouse = elem(0, 0, kHIDPage_GenericDesktop, kHIDUsage_GD_Mouse);
    TestElem *col_csmr = elem(0, 0, kHIDPage_Consumer, kHIDUsage_Csmr_ConsumerControl);
    TestElem *lx = elem(0, 1, 1, 0x30), *ly = elem(0, 1, 1, 0x31), *rx = elem(0, 1, 1, 0x32), *ry = elem(0, 1, 1, 0x35);
    TestElem *lt = elem(0, 1, 2, 0xC5), *rt = elem(0, 1, 2, 0xC4);
    lx.parent = ly.parent = col_ptr; rx.parent = ry.parent = lt.parent = rt.parent = col_pad;
    for (TestElem *a in @[lx, ly, rx, ry, lt, rt]) { a.min = -1; a.max = 0; a.reportSize = 16; }
    TestElem *start = elem(0, 1, kHIDPage_Button, 12); start.parent = col_pad;
    TestElem *ac_back = elem(0, 1, kHIDPage_Consumer, kHIDUsage_Csmr_ACBack); ac_back.parent = col_pad;
    TestElem *ac_home = elem(0, 1, kHIDPage_Consumer, 0x223); ac_home.parent = col_pad;
    TestElem *rel_in_pad = elem(0, 1, 1, 0x30); rel_in_pad.parent = col_pad; rel_in_pad.relative = YES; rel_in_pad.min = -127; rel_in_pad.max = 127;
    TestElem *mx = elem(0, 2, 1, 0x30), *my = elem(0, 2, 1, 0x31), *mbtn = elem(0, 2, kHIDPage_Button, 1);
    mx.parent = my.parent = mbtn.parent = col_mouse; mx.relative = my.relative = YES; mx.min = my.min = -127; mx.max = my.max = 127;
    TestElem *media_back = elem(0, 3, kHIDPage_Consumer, kHIDUsage_Csmr_ACBack); media_back.parent = col_csmr;

    TestDev *pad = [TestDev new]; pad.name = @"Composite HID Pad"; pad.vid = 0x1234; pad.pid = 0x5678; pad.entry = 120;
    pad.elements = @[lx, ly, rx, ry, lt, rt, start, ac_back, ac_home, rel_in_pad, mx, my, mbtn, media_back];
    BOOL saved_supports = g_supports; g_supports = NO;
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(pad));
    hid_pad *hp = find(pad);
    assert(hp && hp->st.has[PH_X] && hp->st.has[PH_Y] && hp->st.has[PH_Z] && hp->st.has[PH_RZ] && hp->st.has[PH_BRAKE] && hp->st.has[PH_ACCEL]);

    int32_t s[4];
    push_value(pad, lx, 0x7fff); push_value(pad, ly, -32768); push_value(pad, rx, 0x7fff); push_value(pad, ry, 0x7fff);
    pad_hid_sticks(&hp->st, s); assert(s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0);
    push_value(pad, lx, -1); push_value(pad, ly, 0);
    pad_hid_sticks(&hp->st, s); assert(s[0] == DM_STICK_MAX && s[1] == DM_STICK_MAX);
    // Relative mouse X/Y, mouse click, relative axis inside GamePad, AC Home and standalone Consumer Control AC Back are all ignored.
    push_ignored_value(pad, mx, -127); push_ignored_value(pad, my, 127); push_ignored_value(pad, mbtn, 1);
    push_ignored_value(pad, rel_in_pad, -127); push_ignored_value(pad, ac_home, 1); push_ignored_value(pad, media_back, 1);
    pad_hid_sticks(&hp->st, s); assert(s[0] == DM_STICK_MAX && s[1] == DM_STICK_MAX && buttons == 0);
    // Measured trigger sweep on wrapped 16-bit Brake/Accel.
    static const long sweep[] = {0, 5305, 16397, 26314, 44624, 65535, 20445, 0};
    for (unsigned i = 0; i < sizeof sweep / sizeof *sweep; i++) {
        push_value(pad, lt, sweep[i]); assert(!!(buttons & DM_BTN_ZL) == (sweep[i] > 32767));
        push_value(pad, rt, sweep[i]); assert(!!(buttons & DM_BTN_ZR) == (sweep[i] > 32767));
    }
    push_value(pad, ac_back, 1); assert(buttons == DM_BTN_MINUS);
    push_value(pad, start, 1); assert(buttons == (DM_BTN_PLUS | DM_BTN_MINUS));
    push_value(pad, ac_back, 0); assert(buttons == DM_BTN_PLUS);
    push_value(pad, start, 0); assert(buttons == 0);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(pad));

    // A device whose only X/Y are relative (Mouse collection) does not mark left-stick axes present.
    TestDev *rel_only = [TestDev new]; rel_only.name = @"Relative-only axes"; rel_only.vid = 0x1234; rel_only.pid = 0x5679; rel_only.entry = 121;
    rel_only.elements = @[mx, my, rel_in_pad];
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(rel_only));
    hp = find(rel_only); assert(hp && !hp->st.has[PH_X] && !hp->st.has[PH_Y]);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(rel_only));
    g_supports = saved_supports;
    assert(!attached && !g_hidpads && !g_hidpads_gc && !g_hidpads_waiting);
    puts("t_pad_hid_mac: composite mouse filtering, wrapped unsigned axes and Consumer AC Back passed");
}

int main(void) { @autoreleasepool {
    g_gc = [NSMutableArray new];
    gc_fallback();
    hid_layouts();
    hid_composite_and_wrapped();
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
    // PR #989: a Pro Controller clone of another vendor (a PowerA NANO: HID vendor and product 0), which GameController does not list
    g_supports = NO; drain(); nouts = 0;   // the twins' calibration reads above are not counted
    hid_pad twin = {.twin = 1, .nin = PAD_NIN_PROTOCOL, .lights = -1};
    hidpad_lights(&twin, 0); assert(nouts == 0);   // widening raw-pad LED support leaves the existing twin path alone
    NSArray *nin = @[elem(0, 0x3F, 9, 1), elem(0, 0x3F, 1, 0x30), elem(0, 0x21, 0xFF00, 1), elem(0, 0x30, 0xFF00, 1), elem(1, 0x01, 0xFF00, 1), elem(1, 0x10, 0xFF00, 1)];
    TestDev *nano = [TestDev new]; nano.name = @"Lic Pro Controller"; nano.loc = 0x31; nano.entry = 31; nano.transport = @"Bluetooth"; nano.elements = nin;
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(nano));
    hid_pad *n = find(nano);
    assert(n && attached == 1 && n->proto && (n->nin & PAD_NIN_RUMBLE) && n->st.layout == PAD_HID_NINTENDO && n->st.map[0] == 2 && n->st.map[2] == 4);   // report 0x3F's numbering until then
    assert(nouts == 1 && outs[0][0] == 0x01 && outs[0][1] == JC1_SUB_DEVICE_INFO && nano.callback);
    wait_ms(300); assert(nouts == 2 && outs[1][1] == JC1_SUB_DEVICE_INFO && n->tries == 2);   // no answer yet: asked again
    uint8_t r[64] = {0x21}; r[13] = 0x82; r[14] = JC1_SUB_DEVICE_INFO;
    push_report(nano, r, 49); assert(n->talks && hidpad_rumbles(n));
    wait_ms(100);
    static const uint8_t hs[8][2] = {{0x03, 0x30}, {0x40, 1}, {0x48, 1}, {0x10, 0x20}, {0x10, 0x3D}, {0x10, 0x10}, {0x30, 0xF0}};   // the handshake, then all four lights flashing
    assert(nouts == 2 + 7 && !n->steps);
    for (int i = 0; i < 7; i++) assert(outs[2 + i][0] == 0x01 && outs[2 + i][1] == hs[i][0] && outs[2 + i][2] == hs[i][1]);
    memset(r, 0, sizeof r); r[0] = 0x30; r[1] = 1; r[3] = 0x08; for (int k = 0; k < 2; k++) { r[6 + 3 * k] = 0x00; r[7 + 3 * k] = 0x08; r[8 + 3 * k] = 0x80; }   // A, both sticks at 2048
    push_report(nano, r, 49); assert(n->full && hidpad_full(n) && n->pro_m == DM_BTN_A && n->pro_s[0] == 0 && n->pro_s[1] == 0);
    // The requested IMU calibration reaches the decoder: factory rest is zero rotation, with each axis's own sensitivity.
    uint8_t cal[64] = {0x21}; cal[13] = 0x90; cal[14] = JC1_SUB_SPI_READ; cal[15] = 0x20; cal[16] = 0x60; cal[19] = JC1_IMU_CAL_LEN;
    const int origin[3] = {100, -50, 25};
    for (int a = 0; a < 3; a++) { put16(cal + 26 + 2 * a, a == 1 ? 8192 : 16384); put16(cal + 32 + 2 * a, origin[a]); put16(cal + 38 + 2 * a, origin[a] + 13371); }
    push_report(nano, cal, 43); assert(!n->unit.cal.have);   // a truncated calibration cannot replace the defaults
    push_report(nano, cal, 44); assert(n->unit.cal.have && n->unit.cal.gyro_origin[0] == 100);
    r[1] = 2;
    for (int k = 0; k < 3; k++) { put16(r + 17 + 12 * k, 4096); for (int a = 0; a < 3; a++) put16(r + 19 + 12 * k + 2 * a, origin[a]); }
    motion_samples = 0; motion_ns = 0;
    push_report(nano, r, 49);
    assert(motion_samples == 3 && motion_acc[0] == 0 && motion_acc[1] == 0 && motion_acc[2] == -1);
    for (int a = 0; a < 3; a++) assert(fabsf(motion_rot[a]) < 1e-6f);   // previously reported 7 deg/s for the first axis at factory rest
    r[1] = 3;
    for (int k = 0; k < 3; k++) { put16(r + 15 + 12 * k, 2048); put16(r + 19 + 12 * k, origin[0] + 100); }
    push_report(nano, r, 49);
    assert(motion_samples == 6 && fabsf(motion_acc[0] - 1) < 1e-6f);
    assert(fabsf(motion_rot[1] - 100 * n->unit.cal.gyro_scale[0] * (float)M_PI / 180) < 1e-6f);
    // A map survives the automatic switch to full mode; Home and Capture still address usages 13 and 14.
    pad_hid_init(&n->st, "A=1,B=2,X=13,Y=14", PAD_HID_NINTENDO); n->st.buttons = 1u << 1 | 1u << 12 | 1u << 13;
    uint64_t mapped = pad_hid_buttons(&n->st); assert(mapped == (DM_BTN_B | DM_BTN_X | DM_BTN_Y));
    r[4] = 0x30; push_report(nano, r, 49); assert(n->pro_m == mapped);
    pad_hid_init(&n->st, NULL, PAD_HID_NINTENDO);
    player = 0; hidpad_lights(n, 0); assert(nouts == 10 && outs[9][1] == JC1_SUB_LEDS && outs[9][2] == 0x01);   // player 1 once it joins
    hidpad_lights(n, 0); assert(nouts == 10);                                                                    // sent once
    player = -1;
    memset(r, 0, sizeof r); r[0] = PAD_HID_NIN_REPORT;
    push_report(nano, r, 12); assert(!n->full && nouts == 11 && outs[10][1] == JC1_SUB_DEVICE_INFO);                 // back in report 0x3F: read from it, asked again
    wait_ms(300); assert(nouts == 12 && outs[11][1] == JC1_SUB_DEVICE_INFO && n->tries == 2);                    // that ask unanswered: asked again
    memset(r, 0, sizeof r); r[0] = 0x21; r[13] = 0x82; r[14] = JC1_SUB_DEVICE_INFO;
    push_report(nano, r, 49); wait_ms(100); assert(nouts == 12 + 7 && !n->steps && n->answered == n->asks);          // answered: the handshake again, lights included
    n->lights = -1; push_report(nano, (uint8_t[12]){PAD_HID_NIN_REPORT}, 12); assert(nouts == 19);              // not in full mode: a 0x3F asks nothing more
    // its player's rumble (output report 0x10): a buzz goes out on each tick while it lasts, then three reports of silence, then nothing
    player = 0; dm_haptic_state hs0 = {0, 1, {{0.8f, 160, 0.5f, 320}, {0}}}; dm_hidpad_rumble(&hs0);
    int before = nouts; hidpad_rumble_tick(); assert(nouts == before + 1 && outs[before][0] == 0x10);
    hs0.motor[0] = (dm_vibration){0, 160, 0, 320}; dm_hidpad_rumble(&hs0);
    for (int i = 0; i < 5; i++) hidpad_rumble_tick();
    assert(nouts == before + 5);   // the buzz again (the value set when the last report went: held for one report), three of silence, then none
    player = -1;
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(nano)); assert(attached == 0 && !g_hidpads);
    int base = nouts;
    wait_ms(300); assert(nouts == base);                                                                            // gone: nothing more asked
    // buttons in report 0x3F but no subcommand reports: that report's numbering, no output
    TestDev *simple = [TestDev new]; simple.name = @"Pad"; simple.entry = 32; simple.elements = @[elem(0, 0x3F, 9, 1)];
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(simple)); n = find(simple);
    assert(n && !n->proto && n->st.layout == PAD_HID_NINTENDO && n->st.map[2] == 4 && nouts == base);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(simple));
    // an 8BitDo (buttons in report 1): its own numbering, untouched
    TestDev *ebd = [TestDev new]; ebd.vid = PAD_HID_DEFAULT_VENDOR; ebd.name = @"8BitDo"; ebd.entry = 33; ebd.elements = @[elem(0, 1, 9, 1), elem(1, 1, 0xFF00, 1)];
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(ebd)); n = find(ebd);
    assert(n && !(n->nin & PAD_NIN_SIMPLE) && !n->proto && n->st.layout == PAD_HID_DEFAULT && n->st.map[2] == 5 && nouts == base);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(ebd));
    // Lights are independent of full input mode: the subcommand channel alone shows its player, with no device-info handshake.
    TestDev *led = [TestDev new]; led.name = @"LED pad"; led.entry = 35;
    led.elements = @[elem(0, 0x3F, 9, 1), elem(0, 0x21, 0xFF00, 1), elem(1, 0x01, 0xFF00, 1)];
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(led)); n = find(led);
    assert(n && !n->proto && nouts == base + 1 && outs[base][1] == JC1_SUB_LEDS && outs[base][2] == 0xF0);
    hidpad_lights(n, 0); hidpad_lights(n, 0);
    assert(nouts == base + 2 && outs[base + 1][1] == JC1_SUB_LEDS && outs[base + 1][2] == 1);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(led)); base = nouts;
    // the descriptor, but no answer: asked HIDPAD_NIN_TRIES times, then read from report 0x3F
    TestDev *mute = [TestDev new]; mute.name = @"Mute"; mute.entry = 34; mute.elements = nin;
    hidpad_found(NULL, kIOReturnSuccess, NULL, ref(mute)); n = find(mute);
    wait_ms(700); assert(n->tries == HIDPAD_NIN_TRIES && !n->talks && nouts == base + HIDPAD_NIN_TRIES + 1 && !hidpad_rumbles(n));
    assert(outs[nouts - 1][1] == JC1_SUB_LEDS && outs[nouts - 1][2] == 0xF0);   // the unanswered device info does not suppress lights
    hidpad_lights(n, 0); hidpad_lights(n, 0);
    assert(nouts == base + HIDPAD_NIN_TRIES + 2 && outs[nouts - 1][1] == JC1_SUB_LEDS && outs[nouts - 1][2] == 1);
    hidpad_removed(NULL, kIOReturnSuccess, NULL, ref(mute));
    assert(!g_hidpads && !g_hidpads_gc);
    printf("t_pad_hid_mac: passed (%d recounts)\n", changes);
} }
