// Exercise the real IOKit callbacks with fake devices and per-device report buffers; no hardware or manager is opened.
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../dm_input.h"

@interface TestPro : NSObject
@property BOOL synthetic;
@property NSNumber *vendor, *product;   // a Pro Controller unless set
@property uint8_t *buffer;
@property IOHIDReportCallback callback;
@property void *context;
@end
@implementation TestPro
@end
static CFTypeRef test_property(IOHIDDeviceRef device, CFStringRef key) {
    TestPro *d = (__bridge id)device;
    if (CFEqual(key, CFSTR(kIOHIDVendorIDKey))) return (__bridge CFTypeRef)(d.vendor ?: @0x057E);
    if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return (__bridge CFTypeRef)(d.product ?: @0x2009);
    return CFEqual(key, CFSTR(kIOHIDGCSyntheticDeviceKey)) && d.synthetic ? kCFBooleanTrue : NULL;
}
static void test_callback(IOHIDDeviceRef device, uint8_t *buffer, CFIndex len, IOHIDReportCallback callback, void *ctx) {
    TestPro *d = (__bridge id)device; assert(len >= 49);
    d.buffer = buffer; d.callback = callback; d.context = ctx;
}
static int sent, player, cleared = -1, ds4_sent;
static uint64_t tick = 1000000000, sample_time;
uint64_t now_ns(void) { return tick; }
void dm_motion_set_hid(int p, const float a[3], const float w[3], uint64_t ns) {
    sent++; player = p; sample_time = ns; assert(a[2] == -1 && (w[2] == 0 || w[2] > 0));
    if (w[2] > 0) ds4_sent++;   // the DualShock 4 reports below turn about z, the Pro's do not
}
void dm_motion_clear_hid(int p) { cleared = p; }
#define LOG(...) ((void)0)
#define IOHIDDeviceGetProperty test_property
#define IOHIDDeviceRegisterInputReportCallback test_callback
#include "../motion_pro_mac.h"
#undef IOHIDDeviceGetProperty
#undef IOHIDDeviceRegisterInputReportCallback

static IOHIDDeviceRef hid(TestPro *d) { return (__bridge IOHIDDeviceRef)d; }
static void found(TestPro *d) { pro_found(NULL, kIOReturnSuccess, NULL, hid(d)); }
static void removed(TestPro *d) { pro_removed(NULL, kIOReturnSuccess, NULL, hid(d)); }
static int gc[PRO_MODELS], slot[PRO_MODELS] = {-1, -1};   // GameController's controllers of each model, as front_mac.m counts them
static void players(int pros, int pro_slot) { gc[PRO_MODEL_PRO] = pros; slot[PRO_MODEL_PRO] = pro_slot; dm_motion_pro_players(gc, slot); }
static void changed(void) { dm_motion_pro_players(gc, slot); }
static int mock_gc_nz[2], stick_notifies, stick_notify_player = -1;
static int mock_gc_sticks(int p, int nz[2]) {
    if (gc[PRO_MODEL_PRO] != 1 || slot[PRO_MODEL_PRO] != p) return 0;
    nz[0] = mock_gc_nz[0]; nz[1] = mock_gc_nz[1];
    return 1;
}
static void on_stick_changed(int p) { stick_notifies++; stick_notify_player = p; }
#define g_pro_player (g_pro_pair[PRO_MODEL_PRO].player)
#define g_pro_bound (g_pro_pair[PRO_MODEL_PRO].bound)
static void report_sticks(TestPro *d, int lx, int ly, int rx, int ry) {
    assert(d.callback && d.buffer); memset(d.buffer, 0, 49); d.buffer[0] = 0x30; d.buffer[1] = (uint8_t)(tick / 15000000);
    const int axes[4] = {lx, ly, rx, ry};
    for (int k = 0; k < 2; k++) { int x = axes[2 * k], y = axes[2 * k + 1]; d.buffer[6 + 3 * k] = x & 255; d.buffer[7 + 3 * k] = (x >> 8) | ((y & 15) << 4); d.buffer[8 + 3 * k] = y >> 4; }
    for (int k = 0; k < 3; k++) d.buffer[18 + 12 * k] = 0x10;   // +4096 z specific force
    d.callback(d.context, kIOReturnSuccess, hid(d), kIOHIDReportTypeInput, 0x30, d.buffer, 49);
    tick += 15000000;
}
static void report(TestPro *d) { report_sticks(d, 2048, 2048, 2048, 2048); }
static void ds4_report(TestPro *d) {   // lying face up, turning about its own y (Switch z)
    assert(d.callback && d.buffer); memset(d.buffer, 0, 78); d.buffer[0] = 0x11; d.buffer[18] = 0x10; d.buffer[24] = 0x20;
    d.callback(d.context, kIOReturnSuccess, hid(d), kIOHIDReportTypeInput, 0x11, d.buffer, 78);
    d.callback(d.context, kIOReturnSuccess, hid(d), kIOHIDReportTypeInput, 0x01, d.buffer, 78);   // USB's report: not read
    tick += 4000000;
}
int main(void) { @autoreleasepool {
    g_pro_changed = changed;
    TestPro *a = [TestPro new], *b = [TestPro new], *synthetic = [TestPro new]; synthetic.synthetic = YES;
    found(synthetic); assert(!g_pro_devices && !synthetic.callback);
    pro_found(NULL, kIOReturnError, NULL, hid(a)); assert(!g_pro_devices);
    players(1, 2); found(a); found(a);
    assert(g_pro_devices && !g_pro_devices->next && g_pro_player == -1);   // no report 0x30 yet: not paired
    report(a); assert(sent == 3 && player == 2 && g_pro_player == 2 && sample_time == tick - 15000000);   // its first report pairs it
    a.callback(a.context, kIOReturnError, hid(a), kIOHIDReportTypeInput, 0x30, a.buffer, 49); assert(sent == 3);
    dm_motion_pro_active(0); assert(cleared == 2); report(a); assert(sent == 3);
    dm_motion_pro_active(1); report(a); assert(sent == 6);
    players(2, 0); assert(g_pro_player == -1 && cleared == 2); report(a); assert(sent == 6);
    players(1, 2); found(b);
    assert(a.buffer != b.buffer && a.context != b.context && g_pro_player == 2);   // b sends nothing (an entry with no pad behind it): a stays paired
    report(a); assert(sent == 9);
    report(b); assert(g_pro_player == -1); report(a); assert(sent == 9);   // both send: which one GameController reads is not known
    removed(b); assert(!b.callback && g_pro_player == 2);
    report(a); assert(sent == 12 && player == 2);
    players(1, -1); report(a); assert(sent == 12 && cleared == 2);   // no free slot
    players(1, 4); report(a); assert(sent == 15 && player == 4);
    removed(a); assert(!a.callback && !g_pro_devices && !g_pro_bound && cleared == 4);
    players(1, 0); found(b); report(b); assert(sent == 18 && player == 0);   // reconnect / merged mode
    TestPro *c = [TestPro new]; found(c); assert(g_pro_bound && g_pro_bound->hid == hid(b));
    tick += PRO_SILENT_NS; report(c);   // b fell silent without being removed (issue #2237's leftover entry), c streams: c is paired
    assert(g_pro_bound && g_pro_bound->hid == hid(c) && sent == 21 && player == 0);
    // A DualShock 4 beside the Pro: each model is paired on its own (the PR's shared count left both unpaired)
    TestPro *ds = [TestPro new]; ds.vendor = @0x054C; ds.product = @0x09CC; found(ds);
    gc[PRO_MODEL_DS4] = 1; slot[PRO_MODEL_DS4] = 1; changed();
    assert(g_pro_bound && g_pro_bound->hid == hid(c) && !g_pro_pair[PRO_MODEL_DS4].bound);   // no report 0x11 yet
    ds4_report(ds); assert(g_pro_pair[PRO_MODEL_DS4].bound && g_pro_pair[PRO_MODEL_DS4].player == 1 && ds4_sent == 1 && player == 1);
    report(c); assert(g_pro_bound && g_pro_bound->hid == hid(c) && player == 0 && sent == 25);
    TestPro *other = [TestPro new]; other.vendor = @0x054C; other.product = @0x0CE6; found(other); assert(g_pro_devices->hid != hid(other));   // a DualSense: not this reader's
    gc[PRO_MODEL_DS4] = 2; changed(); assert(!g_pro_pair[PRO_MODEL_DS4].bound && cleared == 1 && g_pro_bound && g_pro_bound->hid == hid(c));   // two DualShock 4s: only that model gives up
    ds4_report(ds); assert(ds4_sent == 1);
    gc[PRO_MODEL_DS4] = 1; slot[PRO_MODEL_DS4] = 0; changed();   // DM_PAD_MERGE: both on player 1, the Pro (first in the table) keeps it
    assert(!g_pro_pair[PRO_MODEL_DS4].bound && g_pro_bound && g_pro_bound->hid == hid(c));
    // The Pro falls silent without a removal callback; the DS4 keeps streaming and takes player 1 after the Pro expires.
    int ds4_before = ds4_sent;
    for (int i = 0; i < 300; i++) ds4_report(ds);   // 1.2 seconds: the DS4 never has a gap, the Pro sends nothing
    assert(!g_pro_bound && g_pro_player == -1 && cleared == 0);
    assert(g_pro_pair[PRO_MODEL_DS4].bound && g_pro_pair[PRO_MODEL_DS4].player == 0);
    assert(ds4_sent > ds4_before && player == 0 && sample_time == tick - 4000000);
    ds4_before = ds4_sent;
    report(c);   // the preferred model resumes: its first report takes the player back
    assert(g_pro_bound && g_pro_bound->hid == hid(c) && !g_pro_pair[PRO_MODEL_DS4].bound && player == 0);
    ds4_report(ds); assert(ds4_sent == ds4_before);   // the DS4 no longer feeds the shared player
    removed(ds); gc[PRO_MODEL_DS4] = 0; slot[PRO_MODEL_DS4] = -1; changed();
    removed(c); removed(b); removed(synthetic); assert(!g_pro_devices);

    // Automatic per-stick passive HID fallback (#2655, #382): latches only after sustained deflection while GameController (mapped and unmapped) stays zero
    g_pro_gc_sticks = mock_gc_sticks; g_pro_stick_changed = on_stick_changed;
    g_pro_sticks = 1; g_pro_gyro = 0;   // turning off motion must not disable stick fallback
    int motion_before = sent;
    players(1, 2); found(a);
    // BETOP T6 pattern: right stick is live in GameController (nz[1]=1), left stick stays exact zero (nz[0]=0)
    mock_gc_nz[0] = 0; mock_gc_nz[1] = 1; stick_notifies = 0;
    report(a);
    int32_t st[4] = {123, 456, 789, -321};
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 0 && st[0] == 123 && st[1] == 456 && st[2] == 789 && st[3] == -321);
    assert(sent == motion_before);   // gyro stays disabled independently
    // Brief flick (< PRO_STICK_HOLD_NS) then back to center must not latch
    for (int i = 0; i < 5; i++) report_sticks(a, 4091, 2048, 4091, 2048);   // 75 ms < 150 ms
    report(a);
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 0 && stick_notifies == 0);
    // Sustained left+right deflection >= PRO_STICK_HOLD_NS: left latches, right stays on GameController because nz[1] was live
    for (int i = 0; i < 11; i++) report_sticks(a, 4091, 4, 4, 4091);   // 165 ms >= 150 ms
    assert(stick_notifies == 1 && stick_notify_player == 2);
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 1 && st[0] == DM_STICK_MAX && st[1] == -DM_STICK_MAX && st[2] == 789 && st[3] == -321);
    assert(dm_pro_sticks(0, mock_gc_nz, st) == 0);   // never leak another player's input
    // Repeated identical packets do not re-notify (protects DM_PAD_MERGE last-moved-wins)
    report_sticks(a, 4091, 4, 4, 4091);
    assert(stick_notifies == 1);
    // Return to center notifies once, then idle packets stay silent
    report(a); assert(stick_notifies == 2);
    report(a); report(a); assert(stick_notifies == 2);
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 1 && st[0] == 0 && st[1] == 0 && st[2] == 789 && st[3] == -321);
    // Focus loss clears active override without losing the latch; resume re-enables immediately
    report_sticks(a, 4091, 2048, 2048, 2048); assert(stick_notifies == 3);
    dm_motion_pro_active(0);
    st[0] = 17; assert(dm_pro_sticks(2, mock_gc_nz, st) == 0 && st[0] == 17);
    dm_motion_pro_active(1);
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 1 && st[0] == DM_STICK_MAX);
    report_sticks(a, 4091, 2048, 2048, 2048); assert(stick_notifies == 4);   // re-sent after focus return
    // Staleness expiry releases a tilted latched stick; malformed short report cannot refresh it
    tick += PRO_MOTION_STALE_NS;
    uint64_t heard = g_pro_bound->heard;
    a.callback(a.context, kIOReturnSuccess, hid(a), kIOHIDReportTypeInput, 0x30, a.buffer, 12);
    assert(g_pro_bound->heard == heard);
    pro_stick_expire();
    assert(stick_notifies == 5 && stick_notify_player == 2);
    st[0] = 19; assert(dm_pro_sticks(2, mock_gc_nz, st) == 0 && st[0] == 19);
    // Fresh report resumes latched left stick
    report_sticks(a, 4091, 2048, 2048, 2048); assert(stick_notifies == 6 && dm_pro_sticks(2, mock_gc_nz, st) == 1);
    // Ambiguity (second streaming Pro) unbinds and notifies old player so tilted stick is released
    found(b); report(b);
    assert(stick_notifies == 7 && stick_notify_player == 2 && dm_pro_sticks(2, mock_gc_nz, st) == 0);
    removed(b);
    // Both sticks dead in GameController (PR #382): both latch independently
    mock_gc_nz[0] = 0; mock_gc_nz[1] = 0; stick_notifies = 0;
    report(a);
    for (int i = 0; i < 11; i++) report_sticks(a, 4091, 4, 4, 4091);
    assert(stick_notifies == 1 && dm_pro_sticks(2, mock_gc_nz, st) == 3);
    assert(st[0] == DM_STICK_MAX && st[1] == -DM_STICK_MAX && st[2] == -DM_STICK_MAX && st[3] == DM_STICK_MAX);
    // Late GameController activity returns that stick to GameController and keeps it there
    mock_gc_nz[0] = 1;
    report_sticks(a, 4091, 4, 4, 4091);
    assert(stick_notifies == 2);
    st[0] = 55; st[1] = -66;
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 2 && st[0] == 55 && st[1] == -66 && st[2] == -DM_STICK_MAX && st[3] == DM_STICK_MAX);
    mock_gc_nz[0] = 0;
    for (int i = 0; i < 12; i++) report_sticks(a, 4091, 4, 4, 4091);
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 2);   // once GC was live, left stick never latches back
    // Opt-out (DM_PRO_STICKS=0): disables stick override while keeping motion independent
    g_pro_sticks = 0; st[2] = 77;
    assert(dm_pro_sticks(2, mock_gc_nz, st) == 0 && st[2] == 77);
    removed(a); assert(!g_pro_devices);
    puts("t_motion_pro_mac: passed");
} }
