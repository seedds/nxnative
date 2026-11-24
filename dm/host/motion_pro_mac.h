// Mac-only passive controller HID transport: pads whose GameController motion (or a Switch Pro Controller's sticks) stays zero, read from the reports the OS driver already
// receives. All callbacks and routing run on the main run loop, alongside front_mac.m's player slots.
// Workaround for macOS's zero-only GCMotion (#296, #1770) and zero-only Switch Pro sticks (#2655, #382): the OS driver cannot be fixed here, but its underlying reports carry
// the live IMU and stick data.
// A separate buffer / decoder belongs to each retained IOHIDDevice. Disconnect or ambiguity clears only the raw source; GameController keeps its buttons, haptics and working axes.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "motion_pro.h"
#include "pad_pro.h"
#include "dm_input.h"
#include <stdlib.h>
#include <string.h>

// One row per model: its HID IDs, the report that carries its IMU, its minimum length, GameController's product category or vendorName for it (the pairing key),
// whether to restrict matching to GamePad/Joystick interfaces, and the decoder.
// Each model is paired on its own, so one pad of each model is paired even with another model connected
typedef int (*pro_decoder)(pro_motion *, const uint8_t *, size_t, uint64_t, pro_motion_sample[3]);
typedef struct { const char *name, *category, *vendor_name; int vendor, product[2]; uint8_t report; size_t min_len; int gamepad_only; pro_decoder decode; } pro_model;
enum { PRO_MODEL_PRO, PRO_MODEL_DS4, PRO_MODEL_8BITDO_U2W, PRO_MODELS };
enum { PRO_STICK_TRIG = DM_STICK_MAX / 2 };              // half deflection in report 0x30 before suspecting a zero-only GameController stick
#define PRO_STICK_HOLD_NS 150000000ull                   // sustained deflection while GameController (mapped and unmapped) stays zero before latching HID
static const pro_model g_pro_models[PRO_MODELS] = {
    {"Pro Controller", "Switch Pro Controller", NULL, 0x057E, {0x2009}, 0x30, 49, 0, pro_motion_decode},
    // Over USB a DualShock 4 sends report 0x01, never 0x11, so its entry never counts: GameController's motion works there (issue #1770)
    {"DualShock 4", "DualShock 4", NULL, 0x054C, {0x05C4, 0x09CC}, 0x11, 78, 0, ds4_motion_decode},
    // Bluetooth firmware 1.03+ streams 34-byte report 0x01 with IMU; firmware 1.02 sends 12-byte reports without sensors (issue #2997)
    {"8BitDo Ultimate 2 Wireless", NULL, "8BitDo Ultimate 2 Wireless", 0x2DC8, {0x6012}, 0x01, 34, 1, abitdo_motion_decode},
};
static int pro_model_of(int vendor, int product) {
    for (int m = 0; m < PRO_MODELS; m++) for (int i = 0; i < 2; i++) if (vendor == g_pro_models[m].vendor && product && product == g_pro_models[m].product[i]) return m;
    return -1;
}
static int pro_model_of_controller(const char *category, const char *vendor_name) {   // GameController's controller of a model (front_mac.m)
    for (int m = 0; m < PRO_MODELS; m++) if (g_pro_models[m].category && category && !strcmp(category, g_pro_models[m].category)) return m;
    for (int m = 0; m < PRO_MODELS; m++) if (g_pro_models[m].vendor_name && vendor_name && !strcmp(vendor_name, g_pro_models[m].vendor_name)) return m;
    return -1;
}

typedef struct pro_device {
    IOHIDDeviceRef hid; uint8_t report[512]; pro_motion motion; uint64_t heard; int model, short_logged;
    // Automatic per-stick fallback for Pro Controllers whose GameController stick stays zero (#2655):
    // stick_live: GameController (unmappedInput or extendedGamepad) was non-zero for this stick -> GC owns it;
    // stick_hid: latched onto report 0x30; stick_since: when continuous raw deflection >= PRO_STICK_TRIG began while GC stayed zero;
    // stick_sent / stick_sent_mask: last values pushed through g_pro_stick_changed (so idle packets never trigger publishes)
    pad_pro sticks; int32_t stick[4]; int stick_live[2], stick_hid[2]; uint64_t stick_since[2];
    int32_t stick_sent[4]; int stick_sent_mask;
    struct pro_device *next;
} pro_device;   // heard: its last complete report (pro_live)
// A model's pairing: its GameController controllers and the player of the last one (from front_mac.m), the device bound and its player, what the last log line counted
typedef struct { pro_device *bound; int player, controllers, slot, seen_devices, seen_controllers; } pro_pairing;
static IOHIDManagerRef g_pro_manager;
static pro_device *g_pro_devices;
static pro_pairing g_pro_pair[PRO_MODELS] = {[0 ... PRO_MODELS - 1] = {NULL, -1, 0, -1, -1, -1}};
static int g_pro_active = 1, g_pro_gyro = 1, g_pro_sticks = 1, g_pro_stick_timer;
static void (*g_pro_changed)(void);
static int (*g_pro_gc_sticks)(int player, int nonzero[2]);   // front_mac.m: 1 if a paired Pro GCController exists; nonzero[0/1]: left/right stick non-zero in unmappedInput or extendedGamepad
static void (*g_pro_stick_changed)(int player);              // front_mac.m: reread the paired Pro controller (called only when a latched stick's output changes or clears)

static int pro_sticks_fresh(const pro_device *d, uint64_t now) { return d && d->sticks.seen && now >= d->heard && now - d->heard < PRO_MOTION_STALE_NS; }
static void pro_stick_reset(pro_device *d) {
    memset(d->stick_live, 0, sizeof d->stick_live); memset(d->stick_hid, 0, sizeof d->stick_hid); memset(d->stick_since, 0, sizeof d->stick_since);
    memset(d->stick_sent, 0, sizeof d->stick_sent); d->stick_sent_mask = 0;
}
static int pro_stick_nonzero_sent(const pro_device *d) {
    return d && d->stick_sent_mask && (d->stick_sent[0] || d->stick_sent[1] || d->stick_sent[2] || d->stick_sent[3]);
}
static void pro_stick_arm_expire(uint64_t delay_ns);
// Release a latched tilted stick if report 0x30 stops arriving without a removal callback.
static void pro_stick_expire(void) {
    pro_pairing *g = &g_pro_pair[PRO_MODEL_PRO]; pro_device *d = g->bound;
    if (!g_pro_sticks || !g_pro_active || g->player < 0 || !pro_stick_nonzero_sent(d)) return;
    uint64_t now = now_ns();
    if (pro_sticks_fresh(d, now)) { pro_stick_arm_expire(PRO_MOTION_STALE_NS - (now - d->heard)); return; }
    memset(d->stick_sent, 0, sizeof d->stick_sent); d->stick_sent_mask = 0;
    if (g_pro_stick_changed) g_pro_stick_changed(g->player);
}
static void pro_stick_arm_expire(uint64_t delay_ns) {
    if (!g_pro_manager || g_pro_stick_timer) return;   // unit tests have no manager and call pro_stick_expire() directly
    g_pro_stick_timer = 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay_ns), dispatch_get_main_queue(), ^{
        g_pro_stick_timer = 0;
        pro_stick_expire();
    });
}
// Called from front_mac.m's pad_read before publishing a Switch Pro Controller's state: any non-zero GC stick marks GC live (and returns a latched stick to GC),
// while any latched stick overrides GC's zero so button presses cannot wipe out a moving HID stick. Returns the substituted stick mask (bit 0 left, bit 1 right).
static int dm_pro_sticks(int player, const int gc_nonzero[2], int32_t st[4]) {
    pro_pairing *g = &g_pro_pair[PRO_MODEL_PRO]; pro_device *d = g->bound;
    if (!g_pro_sticks || !g_pro_active || player != g->player || !d) return 0;
    if (gc_nonzero) for (int s = 0; s < 2; s++) if (gc_nonzero[s]) {
        d->stick_live[s] = 1; d->stick_since[s] = 0;
        if (d->stick_hid[s]) {
            d->stick_hid[s] = 0; d->stick_sent_mask &= ~(1 << s); d->stick_sent[2 * s] = d->stick_sent[2 * s + 1] = 0;
            LOG("[hid] Pro Controller %s stick: GameController active again; returning to GameController", s ? "right" : "left");
        }
    }
    if (!pro_sticks_fresh(d, now_ns())) return 0;
    int mask = (d->stick_hid[0] ? 1 : 0) | (d->stick_hid[1] ? 2 : 0);
    if (!mask) return 0;
    for (int a = 0; a < 4; a++) if (mask & (1 << (a / 2))) st[a] = d->stick[a];
    return mask;
}
// Evaluate per-stick fallback on each Pro report 0x30: latch a stick only after continuous deflection >= PRO_STICK_TRIG for PRO_STICK_HOLD_NS while both mapped and
// unmapped GameController stay exact zero, and notify front_mac.m only when a latched stick's value or mask changes.
static void pro_stick_update(pro_device *d, int player, uint64_t now) {
    if (!g_pro_gc_sticks) return;
    int nz[2] = {0, 0};
    if (!g_pro_gc_sticks(player, nz)) return;
    for (int s = 0; s < 2; s++) {
        if (nz[s]) {
            d->stick_live[s] = 1; d->stick_since[s] = 0;
            if (d->stick_hid[s]) {
                d->stick_hid[s] = 0;
                LOG("[hid] Pro Controller %s stick: GameController active again; returning to GameController", s ? "right" : "left");
            }
        } else if (!d->stick_live[s] && !d->stick_hid[s]) {
            int moved = abs(d->stick[2 * s]) >= PRO_STICK_TRIG || abs(d->stick[2 * s + 1]) >= PRO_STICK_TRIG;
            if (!moved) d->stick_since[s] = 0;
            else if (!d->stick_since[s]) d->stick_since[s] = now;
            else if (now >= d->stick_since[s] && now - d->stick_since[s] >= PRO_STICK_HOLD_NS) {
                d->stick_hid[s] = 1;
                LOG("[hid] Pro Controller %s stick: GameController reads zero while HID moves; reading from report 0x30 (DM_PRO_STICKS=0 keeps GameController)", s ? "right" : "left");
            }
        }
    }
    int mask = (d->stick_hid[0] ? 1 : 0) | (d->stick_hid[1] ? 2 : 0);
    int32_t cur[4] = {0};
    for (int a = 0; a < 4; a++) if (mask & (1 << (a / 2))) cur[a] = d->stick[a];
    if (mask == d->stick_sent_mask && !memcmp(cur, d->stick_sent, sizeof cur)) return;
    d->stick_sent_mask = mask; memcpy(d->stick_sent, cur, sizeof cur);
    if (pro_stick_nonzero_sent(d)) pro_stick_arm_expire(PRO_MOTION_STALE_NS);
    if (g_pro_stick_changed) g_pro_stick_changed(player);
}

// Only the devices that send their motion report count (pro_live): an entry with no pad behind it would leave one pad unpaired. A device counts again at its first report after a
// silence (pro_report); every report also expires silent devices. Under DM_PAD_MERGE two models can share a player: the first live model in the table keeps it
static void pro_route(void) {
    uint64_t now = now_ns(); unsigned taken = 0;   // players bound by an earlier model for motion, as bits
    for (int m = 0; m < PRO_MODELS; m++) {
        pro_pairing *g = &g_pro_pair[m]; int n = 0; pro_device *live = NULL;
        for (pro_device *d = g_pro_devices; d; d = d->next) if (d->model == m && pro_live(d->heard, now)) { n++; live = d; }
        int p = pro_motion_player(n, g->controllers, g->slot);
        if (g_pro_gyro && p >= 0 && p < 32 && taken >> p & 1) p = -1;
        pro_device *d = p >= 0 ? live : NULL;
        if (p < 0 && n && g->controllers && (n != g->seen_devices || g->controllers != g->seen_controllers)) {
            if (g_pro_gyro) LOG("[hid] %s raw motion: %d HID devices sending reports / %d controllers; cannot pair them uniquely, keeping GameController motion", g_pro_models[m].name, n, g->controllers);
            else if (m == PRO_MODEL_PRO && g_pro_sticks) LOG("[hid] Pro Controller HID sticks: %d HID devices sending reports / %d controllers; cannot pair them uniquely, keeping GameController sticks", n, g->controllers);
        }
        g->seen_devices = n; g->seen_controllers = g->controllers;
        if (g_pro_gyro && p >= 0 && p < 32) taken |= 1u << p;
        if (d == g->bound && p == g->player) continue;
        int old_p = g->player, notify = m == PRO_MODEL_PRO && g->bound && g->bound->stick_sent_mask;
        if (g->bound && m == PRO_MODEL_PRO) pro_stick_reset(g->bound);
        if (g_pro_gyro && g->player >= 0) dm_motion_clear_hid(g->player);
        g->bound = d; g->player = p;
        if (d) {
            d->motion = (pro_motion){0};
            if (m == PRO_MODEL_PRO) pro_stick_reset(d);
            if (g_pro_gyro) LOG("[hid] %s raw motion: player %d (passive HID 0x%02x)", g_pro_models[m].name, p + 1, g_pro_models[m].report);
        }
        if (notify && old_p >= 0 && g_pro_stick_changed) g_pro_stick_changed(old_p);
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
        if (g->bound) {
            g->bound->motion = (pro_motion){0};
            if (m == PRO_MODEL_PRO) {
                memset(g->bound->stick_since, 0, sizeof g->bound->stick_since);
                memset(g->bound->stick_sent, 0, sizeof g->bound->stick_sent);
                g->bound->stick_sent_mask = 0;
            }
        }
        if (!active && g_pro_gyro && g->player >= 0) dm_motion_clear_hid(g->player);
    }
}
static void pro_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    pro_device *d = ctx; const pro_model *m = &g_pro_models[d->model]; pro_pairing *g = &g_pro_pair[d->model];
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || report_id != m->report || len < 1 || r[0] != m->report) return;
    if (len < (CFIndex)m->min_len) {
        if (d->model == PRO_MODEL_8BITDO_U2W && !d->short_logged) {
            d->short_logged = 1;
            LOG("[hid] %s raw motion: %ld-byte report 0x%02x has no sensor data (firmware before 1.03?)", m->name, (long)len, m->report);
        }
        return;
    }
    uint64_t now = now_ns(); int was = pro_live(d->heard, now);
    if (d->model == PRO_MODEL_PRO && g_pro_sticks) {
        if (!was) d->sticks = (pad_pro){0};
        uint64_t buttons;
        pad_pro_decode(&d->sticks, r, (size_t)len, &buttons, d->stick);
    }
    d->heard = now;
    // A continuous stream must also release another device that fell silent, including a preferred model holding the same merged player.
    if (!was && g_pro_changed) g_pro_changed(); else pro_route();
    if (!g_pro_active || d != g->bound) return;
    if (d->model == PRO_MODEL_PRO && g_pro_sticks) pro_stick_update(d, g->player, now);
    if (!g_pro_gyro) return;
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
    if (g_pro_models[model].gamepad_only) {
        int page = pro_property(hid, CFSTR(kIOHIDPrimaryUsagePageKey)), usage = pro_property(hid, CFSTR(kIOHIDPrimaryUsageKey));
        if (page != kHIDPage_GenericDesktop || (usage != kHIDUsage_GD_Joystick && usage != kHIDUsage_GD_GamePad)) return;
    }
    if (model != PRO_MODEL_PRO && !g_pro_gyro) return;
    pro_device *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] %s raw HID: cannot allocate device state", g_pro_models[model].name); return; }
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
static void dm_motion_pro_start(void (*changed)(void), int (*gc_sticks)(int, int[2]), void (*stick_changed)(int)) {
    const char *e = getenv("DM_MOTION"), *h = getenv("DM_MOTION_HID"), *s = getenv("DM_PRO_STICKS");
    g_pro_gyro = !(e && !atoi(e)) && !(h && !atoi(h));
    g_pro_sticks = !(s && !atoi(s));
    if (!g_pro_gyro && !g_pro_sticks) return;
    g_pro_changed = changed; g_pro_gc_sticks = gc_sticks; g_pro_stick_changed = stick_changed;
    g_pro_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_pro_manager) return;
    NSMutableArray *match = [NSMutableArray array];
    for (int m = 0; m < PRO_MODELS; m++) {
        if (m != PRO_MODEL_PRO && !g_pro_gyro) continue;
        for (int i = 0; i < 2; i++) if (g_pro_models[m].product[i]) {
            if (g_pro_models[m].gamepad_only) {
                [match addObject:@{@kIOHIDVendorIDKey: @(g_pro_models[m].vendor), @kIOHIDProductIDKey: @(g_pro_models[m].product[i]),
                                   @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad)}];
                [match addObject:@{@kIOHIDVendorIDKey: @(g_pro_models[m].vendor), @kIOHIDProductIDKey: @(g_pro_models[m].product[i]),
                                   @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_Joystick)}];
            } else {
                [match addObject:@{@kIOHIDVendorIDKey: @(g_pro_models[m].vendor), @kIOHIDProductIDKey: @(g_pro_models[m].product[i])}];
            }
        }
    }
    IOHIDManagerSetDeviceMatchingMultiple(g_pro_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_pro_manager, pro_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_pro_manager, pro_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_pro_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_pro_manager, kIOHIDOptionsTypeNone);   // nonexclusive, no output / feature reports
    if (r != kIOReturnSuccess) {
        LOG("[RED] Controller passive HID: cannot open HID (0x%x); keeping GameController input", r);
        IOHIDManagerUnscheduleFromRunLoop(g_pro_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_pro_manager); g_pro_manager = NULL;
    }
}
