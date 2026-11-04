// Mac-only passive Joy-Con rail buttons. Include in one Objective-C translation unit.
// All APIs and callbacks run on the main thread/run loop. No automatic startup,
// exclusive access, output/feature reports, or GameController player assignment.
#pragma once
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "joycon_rails.h"
#include <stdlib.h>
#include <assert.h>

typedef struct joycon_rails_device {
    IOHIDDeviceRef hid;
    uint32_t product;
    uint8_t report[512], bits;
    BOOL valid;
    struct joycon_rails_device *next;
} joycon_rails_device;
static IOHIDManagerRef g_joycon_rails_manager;
static joycon_rails_device *g_joycon_rails_devices;
static void (*g_joycon_rails_changed)(void);

// Unavailable/ambiguous sides always return zero bits, never a guessed device.
static BOOL dm_joycon_rails_state(int side, uint8_t *bits) {
    assert([NSThread isMainThread]);
    if (bits) *bits = 0;
    if (side != 0 && side != 1) return NO;
    joycon_rails_device *only = NULL;
    for (joycon_rails_device *d = g_joycon_rails_devices; d; d = d->next) {
        if (d->product != (side == 0 ? 0x2006u : 0x2007u)) continue;
        if (only) return NO;
        only = d;
    }
    if (!only || !only->valid) return NO;
    if (bits) *bits = only->bits;
    return YES;
}
static void joycon_rails_report(void *ctx, IOReturn result, void *sender,
                               IOHIDReportType type, uint32_t report_id, uint8_t *bytes, CFIndex len) {
    (void)sender;
    joycon_rails_device *d = ctx;
    // Other report families do not replace the last standard button report.
    if (result == kIOReturnSuccess && (type != kIOHIDReportTypeInput || report_id != 0x30)) return;
    uint8_t bits = 0;
    BOOL valid = result == kIOReturnSuccess && len >= 0 &&
        dm_joycon_rails_decode(d->product, report_id, bytes, (size_t)len, &bits);
    BOOL changed = d->valid != valid || d->bits != bits || result != kIOReturnSuccess;
    d->valid = valid; d->bits = bits;
    // Notify last: the client may stop the reader, freeing d.
    if (changed && g_joycon_rails_changed) g_joycon_rails_changed();
}
static void joycon_rails_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess ||
        IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    CFTypeRef vendor = IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey));
    CFTypeRef product = IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
    int v = 0, p = 0;
    if (!vendor || CFGetTypeID(vendor) != CFNumberGetTypeID() ||
        !product || CFGetTypeID(product) != CFNumberGetTypeID() ||
        !CFNumberGetValue(vendor, kCFNumberIntType, &v) ||
        !CFNumberGetValue(product, kCFNumberIntType, &p) ||
        v != 0x057e || (p != 0x2006 && p != 0x2007)) return;
    for (joycon_rails_device *d = g_joycon_rails_devices; d; d = d->next) if (d->hid == hid) return;
    joycon_rails_device *d = calloc(1, sizeof *d);
    if (!d) return;
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->product = p;
    d->next = g_joycon_rails_devices; g_joycon_rails_devices = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, joycon_rails_report, d);
    if (g_joycon_rails_changed) g_joycon_rails_changed();
}
static void joycon_rails_free(joycon_rails_device *d) {
    // Unregister before releasing the retained HID device, buffer, or callback context.
    IOHIDDeviceRegisterInputReportCallback(d->hid, d->report, sizeof d->report, NULL, NULL);
    CFRelease(d->hid); free(d);
}
static void joycon_rails_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    joycon_rails_device **link = &g_joycon_rails_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    joycon_rails_device *d = *link; *link = d->next;
    joycon_rails_free(d);
    if (g_joycon_rails_changed) g_joycon_rails_changed();
}
static void dm_joycon_rails_stop(void) {
    assert([NSThread isMainThread]);
    void (*changed)(void) = g_joycon_rails_changed;
    g_joycon_rails_changed = NULL;
    if (g_joycon_rails_manager) {
        IOHIDManagerUnscheduleFromRunLoop(g_joycon_rails_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        IOHIDManagerRegisterDeviceMatchingCallback(g_joycon_rails_manager, NULL, NULL);
        IOHIDManagerRegisterDeviceRemovalCallback(g_joycon_rails_manager, NULL, NULL);
    }
    while (g_joycon_rails_devices) {
        joycon_rails_device *d = g_joycon_rails_devices;
        g_joycon_rails_devices = d->next; joycon_rails_free(d);
    }
    if (g_joycon_rails_manager) {
        IOHIDManagerClose(g_joycon_rails_manager, kIOHIDOptionsTypeNone);
        CFRelease(g_joycon_rails_manager); g_joycon_rails_manager = NULL;
    }
    if (changed) changed();
}
static void dm_joycon_rails_start(void (*changed)(void)) {
    assert([NSThread isMainThread]);
    // Repeated start is idempotent; state can be queried immediately by the new client.
    if (g_joycon_rails_manager) { g_joycon_rails_changed = changed; if (changed) changed(); return; }
    g_joycon_rails_changed = changed;
    g_joycon_rails_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_joycon_rails_manager) { dm_joycon_rails_stop(); return; }
    IOHIDManagerSetDeviceMatchingMultiple(g_joycon_rails_manager, (__bridge CFArrayRef)@[
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @0x2006},
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @0x2007}]);
    IOHIDManagerRegisterDeviceMatchingCallback(g_joycon_rails_manager, joycon_rails_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_joycon_rails_manager, joycon_rails_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_joycon_rails_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    if (IOHIDManagerOpen(g_joycon_rails_manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        dm_joycon_rails_stop(); return;
    }
    if (g_joycon_rails_changed) g_joycon_rails_changed();
}
