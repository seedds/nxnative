// Mac-only passive motion for pads GameController adopts without a gyro (motion_imu.h: the 8BitDo Ultimate 2 Wireless). All callbacks and routing run on
// the main run loop, alongside front_mac.m's player slots, as motion_pro_mac.h does for the Pro Controller: GameController keeps the buttons and sticks,
// and only the six-axis samples come from the device's own input reports, through the HID motion source (dm_motion_set_hid).
// Pairing: GameController gives a controller no HID identity, so a device drives a player only when exactly one such device and exactly one
// GameController controller of that name without motion are present (pro_motion_player). DM_MOTION=0 or DM_MOTION_HID=0 turns it off.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <mach/mach_time.h>
#include "motion_imu.h"
#include "motion_pro.h"
#include "dm_input.h"
#include <stdlib.h>
#include <string.h>

typedef struct imu_device { IOHIDDeviceRef hid; const imu_profile *pr; uint8_t report[128]; imu_motion motion; int short_logged; struct imu_device *next; } imu_device;
static IOHIDManagerRef g_imu_manager;
static imu_device *g_imu_devices, *g_imu_bound;
static int g_imu_player = -1, g_imu_active = 1, g_imu_controllers, g_imu_slot = -1;
static int g_imu_seen_devices = -1, g_imu_seen_controllers = -1;
static void (*g_imu_changed)(void);

static int imu_profile_named(const char *name) {   // a GameController controller's vendorName is the HID product string
    if (!name) return 0;
    for (int i = 0; i < IMU_PROFILES; i++) if (!strcmp(imu_profiles[i].name, name)) return 1;
    return 0;
}
static void imu_route(void) {
    int n = 0; for (imu_device *d = g_imu_devices; d; d = d->next) n++;
    int p = pro_motion_player(n, g_imu_controllers, g_imu_slot);
    imu_device *d = p >= 0 ? g_imu_devices : NULL;
    if (p < 0 && n && g_imu_controllers && (n != g_imu_seen_devices || g_imu_controllers != g_imu_seen_controllers))
        LOG("[hid] gyro from HID reports: %d devices / %d controllers without motion; cannot pair them uniquely, no motion from them", n, g_imu_controllers);
    g_imu_seen_devices = n; g_imu_seen_controllers = g_imu_controllers;
    if (d == g_imu_bound && p == g_imu_player) return;
    if (g_imu_player >= 0) dm_motion_clear_hid(g_imu_player);
    g_imu_bound = d; g_imu_player = p;
    if (d) { d->motion = (imu_motion){0}; LOG("[hid] gyro from HID reports: '%s' (%04x:%04x) drives player %d", d->pr->name, d->pr->vendor, d->pr->product, p + 1); }
}
static void dm_motion_imu_players(int controllers, int slot) { g_imu_controllers = controllers; g_imu_slot = slot; imu_route(); }
static void dm_motion_imu_active(int active) {
    g_imu_active = active;
    if (g_imu_bound) g_imu_bound->motion = (imu_motion){0};
    if (!active && g_imu_player >= 0) dm_motion_clear_hid(g_imu_player);
}
static uint64_t imu_ticks_ns(uint64_t ticks) {   // IOKit's report time is mach absolute time, now_ns's clock (hle_os.c)
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return ticks * tb.numer / tb.denom;
}
static void imu_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len, uint64_t stamp) {
    (void)sender;
    imu_device *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len < 0 || !g_imu_active || d != g_imu_bound) return;
    if (report_id == d->pr->report_id && (size_t)len < d->pr->length && !d->short_logged) {
        d->short_logged = 1;
        LOG("[hid] gyro from HID reports: '%s' sends %ld-byte reports without the sensor (firmware before 1.03?); no motion from it", d->pr->name, (long)len);
    }
    imu_motion_sample s;
    if (imu_motion_decode(d->pr, &d->motion, report_id, r, (size_t)len, imu_ticks_ns(stamp), now_ns(), &s)) dm_motion_set_hid(g_imu_player, s.acc, s.rot, s.ns);
}
static void imu_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (imu_device *d = g_imu_devices; d; d = d->next) if (d->hid == hid) return;
    CFNumberRef vn = IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey)), pn = IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
    int vid = 0, pid = 0; if (vn) CFNumberGetValue(vn, kCFNumberIntType, &vid); if (pn) CFNumberGetValue(pn, kCFNumberIntType, &pid);
    const imu_profile *pr = imu_profile_find(vid, pid); if (!pr) return;
    imu_device *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] gyro from HID reports: cannot allocate device state"); return; }
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->pr = pr; d->next = g_imu_devices; g_imu_devices = d;
    IOHIDDeviceRegisterInputReportWithTimeStampCallback(hid, d->report, sizeof d->report, imu_report, d);
    if (g_imu_changed) g_imu_changed(); else imu_route();
}
static void imu_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    imu_device **link = &g_imu_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    imu_device *d = *link; *link = d->next;
    if (d == g_imu_bound) g_imu_bound = NULL;
    // Unregister while its buffer / context still exist. The main run loop serializes removal with report callbacks.
    IOHIDDeviceRegisterInputReportWithTimeStampCallback(hid, d->report, sizeof d->report, NULL, NULL);
    if (g_imu_changed) g_imu_changed(); else imu_route();
    CFRelease(d->hid); free(d);
}
static void dm_motion_imu_start(void (*changed)(void)) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    e = getenv("DM_MOTION_HID"); if (e && !atoi(e)) return;
    g_imu_changed = changed;
    g_imu_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_imu_manager) return;
    NSMutableArray *match = [NSMutableArray array];
    for (int i = 0; i < IMU_PROFILES; i++) [match addObject:@{@kIOHIDVendorIDKey: @(imu_profiles[i].vendor), @kIOHIDProductIDKey: @(imu_profiles[i].product)}];
    IOHIDManagerSetDeviceMatchingMultiple(g_imu_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_imu_manager, imu_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_imu_manager, imu_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_imu_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_imu_manager, kIOHIDOptionsTypeNone);   // nonexclusive, no output / feature reports
    if (r != kIOReturnSuccess) {
        LOG("[RED] gyro from HID reports: cannot open HID (0x%x); no motion from these pads", r);
        IOHIDManagerUnscheduleFromRunLoop(g_imu_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_imu_manager); g_imu_manager = NULL;
    }
}
