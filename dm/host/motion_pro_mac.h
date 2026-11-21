// Mac-only controller raw HID motion transport: pads whose GameController motion is empty, read from the reports the OS driver already receives. All callbacks and routing
// run on the main run loop, alongside front_mac.m's player slots.
// Workaround for macOS's zero-only GCMotion of these pads: the OS driver cannot be fixed here, but report 0x30/0x11 carries raw IMU samples (#296, #1770).
// Pro Controller and DualShock 4 are strictly passive. Switch 1 Joy-Con (L/R) needs bounded subcommand 0x40 (IMU enable) and 0x10 (SPI gyro cal) while bound, because macOS
// keeps Joy-Con 1 in report 0x30 with IMU disabled. Disconnect or ambiguity clears only the raw source; GameController keeps its buttons and motion.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "motion_pro.h"
#include "dm_input.h"
#include <stdlib.h>
#include <string.h>

// One row per model: its HID IDs, the report that carries its IMU, GameController's product category for it (the pairing key), pair/active_imu flags, and the decoder.
// Each model is paired on its own, so one pad of each model is paired even with another model connected.
// pair: two HID devices (product[0] preferred over product[1]) belong to one GameController (Switch 1 Joy-Con L/R).
// active_imu: bound device may receive bounded subcommands 0x40 (IMU enable) and 0x10 (SPI gyro cal) when its 0x30 IMU stream is zero/uncalibrated.
typedef int (*pro_decoder)(pro_motion *, const uint8_t *, size_t, uint64_t, pro_motion_sample[3]);
typedef struct { const char *name, *category; int vendor, product[2]; uint8_t report, pair, active_imu; pro_decoder decode; } pro_model;
enum { PRO_MODEL_PRO, PRO_MODEL_JC1, PRO_MODEL_DS4, PRO_MODELS };
enum { PRO_ACTIVE_TRIES = 5 };
static const pro_model g_pro_models[PRO_MODELS] = {
    {"Pro Controller", "Switch Pro Controller", 0x057E, {0x2009}, 0x30, 0, 0, pro_motion_decode},
    {"Joy-Con (L/R)", "Nintendo Switch Joy-Con (L/R)", 0x057E, {0x2007, 0x2006}, 0x30, 1, 1, pro_motion_decode},
    // Over USB a DualShock 4 sends report 0x01, never 0x11, so its entry never counts: GameController's motion works there (issue #1770)
    {"DualShock 4", "DualShock 4", 0x054C, {0x05C4, 0x09CC}, 0x11, 0, 0, ds4_motion_decode},
};
static int pro_model_of(int vendor, int product) {
    for (int m = 0; m < PRO_MODELS; m++) for (int i = 0; i < 2; i++) if (vendor == g_pro_models[m].vendor && product && product == g_pro_models[m].product[i]) return m;
    return -1;
}
static int pro_model_of_category(const char *category) {   // GameController's controller of a model (front_mac.m)
    for (int m = 0; m < PRO_MODELS; m++) if (category && !strcmp(category, g_pro_models[m].category)) return m;
    return -1;
}

typedef struct pro_device {
    IOHIDDeviceRef hid; uint8_t report[512]; pro_motion motion;
    uint64_t heard, last_imu_kick, last_cal_kick;
    int model, product; uint8_t sub_counter, imu_tries, cal_tries, send_failed;
    struct pro_device *next;
} pro_device;   // heard: its last motion report (pro_live)
// A model's pairing: its GameController controllers and the player of the last one (from front_mac.m), the device bound and its player, what the last log line counted
typedef struct { pro_device *bound; int player, controllers, slot, seen_devices, seen_controllers; } pro_pairing;
static IOHIDManagerRef g_pro_manager;
static pro_device *g_pro_devices;
static pro_pairing g_pro_pair[PRO_MODELS] = {[0 ... PRO_MODELS - 1] = {NULL, -1, 0, -1, -1, -1}};
static int g_pro_active = 1;
static void (*g_pro_changed)(void);

static void pro_send_subcmd(pro_device *d, uint8_t subcmd, const uint8_t *args, size_t n) {
    if (d->send_failed) return;
    uint8_t cmd[16]; size_t len = pro_subcmd_build(cmd, d->sub_counter++, subcmd, args, n);
    if (!len) return;
    IOReturn r = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, cmd, (CFIndex)len);
    if (r != kIOReturnSuccess) {
        d->send_failed = 1;
        LOG("[hid] %s raw motion: subcmd 0x%02x failed (0x%x); stopping active IMU requests", g_pro_models[d->model].name, subcmd, r);
    }
}
static void pro_kick_imu(pro_device *d, uint64_t now) {
    d->last_imu_kick = now; d->imu_tries++;
    uint8_t on = 1;
    pro_send_subcmd(d, 0x40, &on, 1);
}
static void pro_kick_cal(pro_device *d, uint64_t now) {
    d->last_cal_kick = now; d->cal_tries++;
    uint8_t spi[5] = {PRO_IMU_SPI_FACTORY & 0xff, PRO_IMU_SPI_FACTORY >> 8, 0x00, 0x00, PRO_IMU_SPI_FACTORY_LEN};
    pro_send_subcmd(d, 0x10, spi, sizeof(spi));
}

// Only the devices that send their motion report count (pro_live): an entry with no pad behind it would leave one pad unpaired. A device counts again at its first report after a
// silence (pro_report); every report also expires silent devices. Under DM_PAD_MERGE two models can share a player: the first live model in the table keeps it
static void pro_route(void) {
    uint64_t now = now_ns(); unsigned taken = 0;   // players bound by an earlier model, as bits
    for (int m = 0; m < PRO_MODELS; m++) {
        pro_pairing *g = &g_pro_pair[m]; const pro_model *md = &g_pro_models[m];
        int n = 0, n0 = 0, n1 = 0; pro_device *live = NULL, *live0 = NULL, *live1 = NULL;
        for (pro_device *d = g_pro_devices; d; d = d->next) if (d->model == m && pro_live(d->heard, now)) {
            n++; live = d;
            if (d->product == md->product[0]) { n0++; live0 = d; } else { n1++; live1 = d; }
        }
        int p = md->pair ? (g->controllers == 1 && n0 <= 1 && n1 <= 1 && n > 0 ? g->slot : -1)
                         : pro_motion_player(n, g->controllers, g->slot);
        if (p >= 0 && p < 32 && taken >> p & 1) p = -1;
        pro_device *d = p >= 0 ? (md->pair ? (live0 ?: live1) : live) : NULL;
        if (p < 0 && n && g->controllers && (n != g->seen_devices || g->controllers != g->seen_controllers))
            LOG("[hid] %s raw motion: %d HID devices sending reports / %d controllers; cannot pair them uniquely, keeping GameController motion", md->name, n, g->controllers);
        g->seen_devices = n; g->seen_controllers = g->controllers;
        if (p >= 0 && p < 32) taken |= 1u << p;
        if (d == g->bound && p == g->player) continue;
        if (g->player >= 0) dm_motion_clear_hid(g->player);
        g->bound = d; g->player = p;
        if (d) {
            pro_motion_reset(&d->motion);
            LOG("[hid] %s raw motion: player %d (%s HID 0x%02x)", md->name, p + 1, md->active_imu ? "active" : "passive", md->report);
        }
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
        if (g->bound) pro_motion_reset(&g->bound->motion);
        if (!active && g->player >= 0) dm_motion_clear_hid(g->player);
    }
}
static void pro_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    pro_device *d = ctx; const pro_model *m = &g_pro_models[d->model]; pro_pairing *g = &g_pro_pair[d->model];
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len < 0) return;
    if (report_id == 0x21 && m->active_imu && d == g->bound) {
        if (len >= 15 && (r[13] & 0x80) && r[14] == 0x40) {
            if (!d->motion.cal.valid && d->cal_tries < PRO_ACTIVE_TRIES && !d->send_failed) pro_kick_cal(d, now_ns());
        } else if (pro_imu_cal_parse(&d->motion.cal, r, (size_t)len)) {
            LOG("[hid] %s SPI gyro cal loaded: off=(%d,%d,%d)", m->name, d->motion.cal.gyro_off[0], d->motion.cal.gyro_off[1], d->motion.cal.gyro_off[2]);
        }
        return;
    }
    if (report_id != m->report) return;
    uint64_t now = now_ns(); int was = pro_live(d->heard, now); d->heard = now;
    // A continuous stream must also release another device that fell silent, including a preferred model holding the same merged player.
    if (!was && g_pro_changed) g_pro_changed(); else pro_route();
    if (!g_pro_active || d != g->bound) return;
    pro_motion_sample samples[3];
    int n = m->decode(&d->motion, r, (size_t)len, now, samples);
    if (m->active_imu && !d->send_failed) {
        if (!d->motion.live && len >= 49 && d->imu_tries < PRO_ACTIVE_TRIES && (!d->last_imu_kick || now - d->last_imu_kick > 2000000000ull))
            pro_kick_imu(d, now);
        else if (d->motion.live && !d->motion.cal.valid && d->cal_tries < PRO_ACTIVE_TRIES && (!d->last_cal_kick || now - d->last_cal_kick > 500000000ull))
            pro_kick_cal(d, now);
    }
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
    int vendor = pro_property(hid, CFSTR(kIOHIDVendorIDKey)), product = pro_property(hid, CFSTR(kIOHIDProductIDKey));
    int model = pro_model_of(vendor, product); if (model < 0) return;
    pro_device *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] %s raw motion: cannot allocate device state", g_pro_models[model].name); return; }
    d->model = model; d->product = product; d->motion.flip_r = (product == 0x2007);
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->next = g_pro_devices; g_pro_devices = d;
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
