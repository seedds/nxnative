// One unambiguous GameController Joy-Con pair owns one physical left/right HID pair.
// macOS exposes the pair's buttons but no GCMotion. Enable only volatile IMU sampling
// (subcommand 0x40); never change pairing, report mode, firmware or calibration storage.
#pragma once
#include "motion_joy.h"
typedef struct joy_device {
    IOHIDDeviceRef hid;
    uint8_t report[512], sequence;
    int side, player, owned, attempted, logged;
    pro_motion motion;
    joy_calibration calibration;
    int calibration_requested, calibration_pending; uint64_t calibration_at;
    struct joy_device *next;
} joy_device;
static IOHIDManagerRef g_joy_manager;
static joy_device *g_joy_devices;
static int g_joy_controllers, g_joy_slot = -1, g_joy_active = 1;
static void (*g_joy_changed)(void);

static void joy_imu(joy_device *d, int on) {
    if ((!on && !d->owned) || (on && d->attempted)) return;
    uint8_t b[12] = {1, d->sequence++ & 15, 0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40, 0x40, (uint8_t)on};
    IOReturn r = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 1, b, sizeof b);
    if (on) { d->attempted = 1; d->owned = r == kIOReturnSuccess; }
    else { d->owned = 0; d->attempted = 0; }
    if (r != kIOReturnSuccess) LOG("[RED] Joy-Con %s IMU %s failed: %#x", d->side ? "right" : "left", on ? "enable" : "disable", r);
}
// Read only the documented 24-byte factory IMU region; no SPI writes.
static void joy_factory_read(joy_device *d) {
    uint8_t b[16] = {1, d->sequence++ & 15, 0,1,0x40,0x40,0,1,0x40,0x40, 0x10,0x20,0x60,0,0,24};
    d->calibration_requested = d->calibration_pending = 1; d->calibration_at = now_ns();
    IOReturn r = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 1, b, sizeof b);
    if (r != kIOReturnSuccess) { d->calibration_pending = 0; LOG("[RED] Joy-Con factory calibration read failed: %#x", r); }
}
static void joy_clear(joy_device *d) {
    if (d->player >= 0) dm_motion_clear_joycon(d->player, d->side);
    joy_imu(d, 0); d->attempted = d->logged = 0; d->motion = (pro_motion){0};
}
static void joy_route(void) {
    int counts[2] = {0}; for (joy_device *d = g_joy_devices; d; d = d->next) counts[d->side]++;
    int p = g_joy_controllers == 1 && counts[0] == 1 && counts[1] == 1 ? g_joy_slot : -1;
    for (joy_device *d = g_joy_devices; d; d = d->next) if (d->player != p) { joy_clear(d); d->player = p; }
}
static void dm_motion_joy_players(int count, int slot) { g_joy_controllers = count; g_joy_slot = slot; joy_route(); }
static void dm_motion_joy_active(int active) {
    if (g_joy_active == active) return;
    g_joy_active = active;
    for (joy_device *d = g_joy_devices; d; d = d->next) joy_clear(d);
}
static void joy_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender; joy_device *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || !g_joy_active || d->player < 0) return;
    if (report_id == 0x21 && len >= 44 && r[14] == 0x10 && r[15] == 0x20 && r[16] == 0x60 && !r[17] && !r[18] && r[19] == 24 && d->calibration_pending) {
        d->calibration_pending = 0;
        if ((r[13] & 0x80) && joy_calibration_parse(&d->calibration, r + 20, 24)) {
            LOG("[hid] Joy-Con %s factory IMU calibration applied (read only)", d->side ? "right" : "left");
            dm_motion_clear_joycon(d->player, d->side); d->motion = (pro_motion){0};
        } else LOG("[RED] Joy-Con factory calibration reply invalid; using nominal scales");
        return;
    }
    if (report_id == 0x21 && len >= 15 && r[14] == 0x40 && !(r[13] & 0x80)) {
        LOG("[RED] Joy-Con %s rejected IMU enable", d->side ? "right" : "left"); return;
    }
    if (report_id != 0x30 || len < 49) return;
    if (!d->calibration_requested) joy_factory_read(d);
    if (d->calibration_pending) {
        if (now_ns() - d->calibration_at < 300000000ull) return;
        d->calibration_pending = 0; LOG("[RED] Joy-Con factory calibration read timed out; using nominal scales");
    }
    pro_motion_sample samples[3];
    int n = joy_motion_decode(&d->motion, d->side, r, (size_t)len, now_ns(), samples, &d->calibration);
    if (!n && !d->motion.live) joy_imu(d, 1);   // an already-live stream is read passively and is not disabled by us
    for (int i = 0; i < n; i++) dm_motion_set_joycon(d->player, d->side, samples[i].acc, samples[i].rot, samples[i].ns);
    if (n && !d->logged++) LOG("[hid] Joy-Con %s raw motion: player %d, real 0x30 IMU samples", d->side ? "right" : "left", d->player + 1);
}
static void joy_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (joy_device *d = g_joy_devices; d; d = d->next) if (d->hid == hid) return;
    NSNumber *pid = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
    if (pid.intValue != 0x2006 && pid.intValue != 0x2007) return;
    joy_device *d = calloc(1, sizeof *d); if (!d) return;
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->side = pid.intValue == 0x2007; d->player = -1;
    d->next = g_joy_devices; g_joy_devices = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, joy_report, d);
    if (g_joy_changed) g_joy_changed(); else joy_route();
}
static void joy_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    joy_device **link = &g_joy_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    joy_device *d = *link;
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
    joy_clear(d);
    *link = d->next; CFRelease(d->hid); free(d);
    if (g_joy_changed) g_joy_changed(); else joy_route();
}
static void dm_motion_joy_start(void (*changed)(void)) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    e = getenv("DM_MOTION_HID"); if (e && !atoi(e)) return;
    g_joy_changed = changed;
    g_joy_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_joy_manager) return;
    IOHIDManagerSetDeviceMatchingMultiple(g_joy_manager, (__bridge CFArrayRef)@[
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @0x2006},
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @0x2007}]);
    IOHIDManagerRegisterDeviceMatchingCallback(g_joy_manager, joy_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_joy_manager, joy_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_joy_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_joy_manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) LOG("[RED] Joy-Con motion HID open failed: %#x", r);
}
