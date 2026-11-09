// Mac: Bluetooth Joy-Con (L) (0x057e:0x2006) and Joy-Con (R) (0x057e:0x2007) over IOHIDManager (pad_joycon.h, LNPANDY/NX2Apple#740).
// Enabled by default (DM_JOYCON_SPLIT=0 opts out to GameController): a Left + Right pair shares one player slot with dm_joy_attach(slot, 3)
// so games see JoyDual or auto-split JoyLeft + JoyRight, and SL+SR / L+R held 3 s (or DM_JOYCON_SPLIT=1) switches between paired and two-player sideways slots.
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
    joycon_rest rest;
    joycon_hold hold;
    uint8_t subcmd_seq;
    unsigned imu_retry;
    struct joycon_pad *partner;   // the other half when paired on one player slot
    int primary;                  // 1 when this half owns the player slot
    char name[64];
    uint8_t rep[128];
    struct joycon_pad *next;
} joycon_pad;

static IOHIDManagerRef g_joycon_manager;
static joycon_pad *g_joycons;
static int g_joycon_split;
static int g_joycon_force_split;
static void joycon_attach(joycon_pad *d);
static void joycon_detach(joycon_pad *d);
static void joycon_read(joycon_pad *d);
static void joycon_read_motion(joycon_pad *d, const uint8_t *rep, size_t len);
static void joycon_home_changed(void);
static void joycon_pair_changed(joycon_pad *pri);
static void joycon_split_pair(joycon_pad *pri, joycon_pad *sec);
static void joycon_join_pair(joycon_pad *a, joycon_pad *b);
static void joycon_unpair_removed(joycon_pad *gone_pad, joycon_pad *keep_pad);

static inline uint32_t joycon_pad_sides(const joycon_pad *d) {
    if (!d) return 0;
    uint32_t s = d->st.pid == JOYCON_PID_R ? 2u : 1u;
    if (d->partner) s |= d->partner->st.pid == JOYCON_PID_R ? 2u : 1u;
    return s;
}

static inline joycon_pad *joycon_pad_primary(joycon_pad *d) {
    return d && d->partner && !d->primary ? d->partner : d;
}

static inline void joycon_update_name(joycon_pad *d) {
    if (!d) return;
    snprintf(d->name, sizeof d->name, "%s", d->partner ? "Joy-Con (L/R)" : (d->st.pid == JOYCON_PID_L ? "Joy-Con (L)" : "Joy-Con (R)"));
}

static int joycon_is_gc_controller(GCController *c) {
    if (!c) return 0;
    NSString *cat = c.productCategory ?: @"";
    NSString *ven = c.vendorName ?: @"";
    return [cat rangeOfString:@"Joy-Con" options:NSCaseInsensitiveSearch].location != NSNotFound
        || [cat rangeOfString:@"JoyCon" options:NSCaseInsensitiveSearch].location != NSNotFound
        || [ven rangeOfString:@"Joy-Con" options:NSCaseInsensitiveSearch].location != NSNotFound
        || [ven rangeOfString:@"JoyCon" options:NSCaseInsensitiveSearch].location != NSNotFound;
}

static void joycon_check_gesture(joycon_pad *d, uint64_t now) {
    if (!d) return;
    if (!joycon_hold_update(&d->hold, joycon_gesture(&d->st, d->partner != NULL), now) || d->hold.done) return;
    if (d->hold.kind == JOYCON_GESTURE_SPLIT && d->partner) {
        joycon_pad *pri = joycon_pad_primary(d);
        joycon_pad *sec = pri->partner;
        if (!sec) return;
        pri->hold.done = sec->hold.done = 1;
        pri->partner = sec->partner = NULL;
        pri->primary = sec->primary = 1;
        joycon_update_name(pri);
        joycon_update_name(sec);
        joycon_split_pair(pri, sec);
        return;
    }
    if (d->hold.kind == JOYCON_GESTURE_JOIN && !d->partner) {
        for (joycon_pad *o = g_joycons; o; o = o->next) {
            if (o == d || o->partner || o->st.pid == d->st.pid) continue;
            if (o->hold.kind != JOYCON_GESTURE_JOIN || o->hold.done || now < o->hold.since || now - o->hold.since < JOYCON_GESTURE_NS) continue;
            d->hold.done = o->hold.done = 1;
            joycon_join_pair(d, o);
            return;
        }
    }
}

static void joycon_enable_imu(joycon_pad *d) {
    if (!d || !d->hid) return;
    // Output report 0x01 with neutral HD rumble (00 01 40 40 00 01 40 40):
    // subcommand 0x40 0x01 enables the 6-axis IMU sensor; 0x03 0x30 selects 60 Hz standard full reports.
    uint8_t enable_imu[49] = {0x01, (uint8_t)(d->subcmd_seq++ & 0x0f), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x40, 0x01};
    IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, enable_imu, sizeof enable_imu);
    uint8_t mode_30[49] = {0x01, (uint8_t)(d->subcmd_seq++ & 0x0f), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, 0x03, 0x30};
    IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, mode_30, sizeof mode_30);
}

static void joycon_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *rep, CFIndex len) {
    (void)sender; (void)type; (void)id;
    joycon_pad *d = ctx;
    if (result != kIOReturnSuccess || !d || len <= 0) return;
    int was_home = d->st.home;
    if (!joycon_decode_0x30(&d->st, rep, (size_t)len)) return;
    joycon_read(d);
    joycon_read_motion(d, rep, (size_t)len);
    if (!d->motion.live && (++d->imu_retry % 60) == 1) joycon_enable_imu(d);
    if (d->st.home != was_home) joycon_home_changed();
    joycon_check_gesture(d, now_ns());
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
    d->primary = 1;
    joycon_update_name(d);
    d->hid = (IOHIDDeviceRef)CFRetain(hid);
    joycon_pad *mate = NULL;
    if (!g_joycon_force_split) {
        for (joycon_pad *p = g_joycons; p; p = p->next)
            if (!p->partner && p->st.pid != d->st.pid) { mate = p; break; }
    }
    joycon_pad **link = &g_joycons;
    while (*link) link = &(*link)->next;
    *link = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->rep, sizeof d->rep, joycon_report, d);
    joycon_enable_imu(d);
    if (mate) {
        mate->partner = d;
        d->partner = mate;
        mate->primary = 1;
        d->primary = 0;
        joycon_update_name(mate);
        joycon_update_name(d);
        joycon_pair_changed(mate);
    } else {
        joycon_attach(d);
    }
}

static void joycon_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    joycon_pad **link = &g_joycons;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    joycon_pad *d = *link;
    *link = d->next;
    IOHIDDeviceRegisterInputReportCallback(hid, d->rep, 0, NULL, NULL);
    if (d->partner) joycon_unpair_removed(d, d->partner);
    else joycon_detach(d);
    CFRelease(d->hid);
    free(d);
}

static void joycon_start(void) {
    const char *e = getenv("DM_JOYCON_SPLIT");
    g_joycon_split = !e || atoi(e) != 0;
    g_joycon_force_split = e && atoi(e) > 0;
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
