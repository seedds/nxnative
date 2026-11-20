// Mac-only passive controller motion transport: pads whose GameController motion is empty, read from the reports the OS driver already receives. All callbacks and routing
// run on the main run loop, alongside front_mac.m's player slots.
// Workaround for macOS's zero-only GCMotion of these pads: the OS driver cannot be fixed here, but its underlying reports contain the IMU samples (#296, #1770).
// A separate buffer / decoder belongs to each retained IOHIDDevice. Disconnect or ambiguity clears only the raw source; GameController keeps its buttons and motion.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "motion_pro.h"
#include "dm_input.h"
#include <stdlib.h>
#include <string.h>

// One row per model: its HID IDs, the report that carries its IMU, GameController's product category for it (the pairing key), and the decoder. Each model is paired on its own,
// so one pad of each model is paired even with another model connected
typedef int (*pro_decoder)(pro_motion *, const uint8_t *, size_t, uint64_t, pro_motion_sample[3]);
typedef struct { const char *name, *category; int vendor, product[2]; uint8_t report; pro_decoder decode; } pro_model;
enum { PRO_MODEL_PRO, PRO_MODEL_DS4, PRO_MODELS };
static const pro_model g_pro_models[PRO_MODELS] = {
    {"Pro Controller", "Switch Pro Controller", 0x057E, {0x2009}, 0x30, pro_motion_decode},
    // Over USB a DualShock 4 sends report 0x01, never 0x11, so its entry never counts: GameController's motion works there (issue #1770)
    {"DualShock 4", "DualShock 4", 0x054C, {0x05C4, 0x09CC}, 0x11, ds4_motion_decode},
};
static int pro_model_of(int vendor, int product) {
    for (int m = 0; m < PRO_MODELS; m++) for (int i = 0; i < 2; i++) if (vendor == g_pro_models[m].vendor && product && product == g_pro_models[m].product[i]) return m;
    return -1;
}
static int pro_model_of_category(const char *category) {   // GameController's controller of a model (front_mac.m)
    for (int m = 0; m < PRO_MODELS; m++) if (category && !strcmp(category, g_pro_models[m].category)) return m;
    return -1;
}

typedef struct pro_device { IOHIDDeviceRef hid; uint8_t report[512]; pro_motion motion; uint64_t heard; int model; struct pro_device *next; } pro_device;   // heard: its last motion report (pro_live)
// A model's pairing: its GameController controllers and the player of the last one (from front_mac.m), the device bound and its player, what the last log line counted
typedef struct { pro_device *bound; int player, controllers, slot, seen_devices, seen_controllers; } pro_pairing;
static IOHIDManagerRef g_pro_manager;
static pro_device *g_pro_devices;
static pro_pairing g_pro_pair[PRO_MODELS] = {[0 ... PRO_MODELS - 1] = {NULL, -1, 0, -1, -1, -1}};
static int g_pro_active = 1;
static void (*g_pro_changed)(void);

// Only the devices that send their motion report count (pro_live): an entry with no pad behind it would leave one pad unpaired. A device counts again at its first report after a
// silence (pro_report); every report also expires silent devices. Under DM_PAD_MERGE two models can share a player: the first live model in the table keeps it
static void pro_route(void) {
    uint64_t now = now_ns(); unsigned taken = 0;   // players bound by an earlier model, as bits
    for (int m = 0; m < PRO_MODELS; m++) {
        pro_pairing *g = &g_pro_pair[m]; int n = 0; pro_device *live = NULL;
        for (pro_device *d = g_pro_devices; d; d = d->next) if (d->model == m && pro_live(d->heard, now)) { n++; live = d; }
        int p = pro_motion_player(n, g->controllers, g->slot);
        if (p >= 0 && p < 32 && taken >> p & 1) p = -1;
        pro_device *d = p >= 0 ? live : NULL;
        if (p < 0 && n && g->controllers && (n != g->seen_devices || g->controllers != g->seen_controllers))
            LOG("[hid] %s raw motion: %d HID devices sending reports / %d controllers; cannot pair them uniquely, keeping GameController motion", g_pro_models[m].name, n, g->controllers);
        g->seen_devices = n; g->seen_controllers = g->controllers;
        if (p >= 0 && p < 32) taken |= 1u << p;
        if (d == g->bound && p == g->player) continue;
        if (g->player >= 0) dm_motion_clear_hid(g->player);
        g->bound = d; g->player = p;
        if (d) { d->motion = (pro_motion){0}; LOG("[hid] %s raw motion: player %d (passive HID 0x%02x)", g_pro_models[m].name, p + 1, g_pro_models[m].report); }
    }
}
// GameController's controllers of each model now, and the player of the last one of each
static void dm_motion_pro_players(const int controllers[PRO_MODELS], const int slots[PRO_MODELS]) {
    for (int m = 0; m < PRO_MODELS; m++) { g_pro_pair[m].controllers = controllers[m]; g_pro_pair[m].slot = slots[m]; }
    pro_route();
}
static void dm_motion_pro_active(int active) {
    g_pro_active = active;
    for (int m = 0; m < PRO_MODELS; m++) {
        pro_pairing *g = &g_pro_pair[m];
        if (g->bound) g->bound->motion = (pro_motion){0};
        if (!active && g->player >= 0) dm_motion_clear_hid(g->player);
    }
}
static void pro_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    pro_device *d = ctx; const pro_model *m = &g_pro_models[d->model]; pro_pairing *g = &g_pro_pair[d->model];
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || report_id != m->report || len < 0) return;
    uint64_t now = now_ns(); int was = pro_live(d->heard, now); d->heard = now;
    // A continuous stream must also release another device that fell silent, including a preferred model holding the same merged player.
    if (!was && g_pro_changed) g_pro_changed(); else pro_route();
    if (!g_pro_active || d != g->bound) return;
    pro_motion_sample samples[3];
    int n = m->decode(&d->motion, r, (size_t)len, now, samples);
    for (int i = 0; i < n; i++) dm_motion_set_hid(g->player, samples[i].acc, samples[i].rot, samples[i].ns);
}
static int pro_property(IOHIDDeviceRef hid, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(hid, key); int n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
    return n;
}
static void pro_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (pro_device *d = g_pro_devices; d; d = d->next) if (d->hid == hid) return;
    int model = pro_model_of(pro_property(hid, CFSTR(kIOHIDVendorIDKey)), pro_property(hid, CFSTR(kIOHIDProductIDKey))); if (model < 0) return;
    pro_device *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] %s raw motion: cannot allocate device state", g_pro_models[model].name); return; }
    d->model = model; d->hid = (IOHIDDeviceRef)CFRetain(hid); d->next = g_pro_devices; g_pro_devices = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, pro_report, d);
    if (g_pro_changed) g_pro_changed();
}
static void pro_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    pro_device **link = &g_pro_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    pro_device *d = *link; *link = d->next;
    // Unregister while its buffer / context still exist. The main run loop serializes removal with report callbacks.
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
    if (g_pro_changed) g_pro_changed(); else pro_route();
    CFRelease(d->hid); free(d);
}
static void dm_motion_pro_start(void (*changed)(void)) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    e = getenv("DM_MOTION_HID"); if (e && !atoi(e)) return;
    g_pro_changed = changed;
    g_pro_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_pro_manager) return;
    NSMutableArray *match = [NSMutableArray array];
    for (int m = 0; m < PRO_MODELS; m++) for (int i = 0; i < 2; i++) if (g_pro_models[m].product[i])
        [match addObject:@{@kIOHIDVendorIDKey: @(g_pro_models[m].vendor), @kIOHIDProductIDKey: @(g_pro_models[m].product[i])}];
    IOHIDManagerSetDeviceMatchingMultiple(g_pro_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_pro_manager, pro_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_pro_manager, pro_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_pro_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_pro_manager, kIOHIDOptionsTypeNone);   // nonexclusive, no output / feature reports
    if (r != kIOReturnSuccess) {
        LOG("[RED] Controller raw motion: cannot open HID (0x%x); keeping GameController motion", r);
        IOHIDManagerUnscheduleFromRunLoop(g_pro_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_pro_manager); g_pro_manager = NULL;
    }
}
