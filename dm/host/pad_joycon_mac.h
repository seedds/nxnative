// Mac: split a Bluetooth Joy-Con pair into two sideways controllers (pad_joycon.h, LNPANDY/NX2Apple#740).
// When DM_JOYCON_SPLIT=1 is set, front_mac.m skips GameController's combined Joy-Con entry and reads Joy-Con (L) (0x057e:0x2006)
// and Joy-Con (R) (0x057e:0x2007) directly with IOHIDManager so each half gets its own player slot.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "pad_joycon.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct joycon_pad {
    IOHIDDeviceRef hid;
    joycon_state st;
    pro_motion motion;
    char name[64];
    uint8_t rep[128];
    struct joycon_pad *next;
} joycon_pad;

static IOHIDManagerRef g_joycon_manager;
static joycon_pad *g_joycons;
static int g_joycon_split;
static void joycon_attach(joycon_pad *d);
static void joycon_detach(joycon_pad *d);
static void joycon_read(joycon_pad *d);
static void joycon_read_motion(joycon_pad *d, const uint8_t *rep, size_t len);
static void joycon_home_changed(void);

static int joycon_is_gc_controller(GCController *c) {
    if (!c) return 0;
    NSString *cat = c.productCategory ?: @"";
    NSString *ven = c.vendorName ?: @"";
    return [cat rangeOfString:@"Joy-Con" options:NSCaseInsensitiveSearch].location != NSNotFound
        || [cat rangeOfString:@"JoyCon" options:NSCaseInsensitiveSearch].location != NSNotFound
        || [ven rangeOfString:@"Joy-Con" options:NSCaseInsensitiveSearch].location != NSNotFound
        || [ven rangeOfString:@"JoyCon" options:NSCaseInsensitiveSearch].location != NSNotFound;
}

static void joycon_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *rep, CFIndex len) {
    (void)sender; (void)type; (void)id;
    joycon_pad *d = ctx;
    if (result != kIOReturnSuccess || !d || len <= 0) return;
    int was_home = d->st.home;
    if (!joycon_decode_0x30(&d->st, rep, (size_t)len)) return;
    joycon_read(d);
    joycon_read_motion(d, rep, (size_t)len);
    if (d->st.home != was_home) joycon_home_changed();
}

static void joycon_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (!g_joycon_split || result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (joycon_pad *d = g_joycons; d; d = d->next) if (d->hid == hid) return;
    NSNumber *vendor = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey));
    NSNumber *product = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
    long vid = vendor.longValue, pid = product.longValue;
    if (!joycon_is_pid(vid, pid)) return;
    joycon_pad *d = calloc(1, sizeof *d);
    if (!d) { LOG("[RED] Split Joy-Con: cannot allocate device state"); return; }
    joycon_init(&d->st, (uint16_t)pid);
    snprintf(d->name, sizeof d->name, "%s", pid == JOYCON_PID_L ? "Joy-Con (L)" : "Joy-Con (R)");
    d->hid = (IOHIDDeviceRef)CFRetain(hid);
    joycon_pad **link = &g_joycons;
    while (*link) link = &(*link)->next;
    *link = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->rep, sizeof d->rep, joycon_report, d);
    joycon_attach(d);
}

static void joycon_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    joycon_pad **link = &g_joycons;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    joycon_pad *d = *link;
    *link = d->next;
    IOHIDDeviceRegisterInputReportCallback(hid, d->rep, 0, NULL, NULL);
    joycon_detach(d);
    CFRelease(d->hid);
    free(d);
}

static void joycon_start(void) {
    const char *e = getenv("DM_JOYCON_SPLIT");
    g_joycon_split = e && atoi(e);
    if (!g_joycon_split || g_joycon_manager) return;
    g_joycon_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_joycon_manager) return;
    IOHIDManagerSetDeviceMatchingMultiple(g_joycon_manager, (__bridge CFArrayRef)@[
        @{@kIOHIDVendorIDKey: @(JOYCON_VID), @kIOHIDProductIDKey: @(JOYCON_PID_L)},
        @{@kIOHIDVendorIDKey: @(JOYCON_VID), @kIOHIDProductIDKey: @(JOYCON_PID_R)}]);
    IOHIDManagerRegisterDeviceMatchingCallback(g_joycon_manager, joycon_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_joycon_manager, joycon_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_joycon_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_joycon_manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        LOG("[RED] Split Joy-Con: cannot open HID (0x%x), keeping GameController's combined pair", r);
        IOHIDManagerUnscheduleFromRunLoop(g_joycon_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_joycon_manager);
        g_joycon_manager = NULL;
        g_joycon_split = 0;
        return;
    }
    // Attach Joy-Con (L) before Joy-Con (R) when both are already connected at launch so (L) gets Player 1 and (R) gets Player 2.
    NSSet *devs = CFBridgingRelease(IOHIDManagerCopyDevices(g_joycon_manager));
    const int order[2] = {JOYCON_PID_L, JOYCON_PID_R};
    for (int i = 0; i < 2; i++) {
        for (id obj in devs) {
            IOHIDDeviceRef hid = (__bridge IOHIDDeviceRef)obj;
            NSNumber *pid = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
            if (pid.intValue == order[i]) joycon_found(NULL, kIOReturnSuccess, NULL, hid);
        }
    }
}
