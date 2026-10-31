// Mac-only Joy-Con IMU transport (#296's sibling: the Pro Controller reader is motion_pro_mac.h). macOS drives a Joy-Con in report
// mode 0x3F, which carries no IMU, and Apple gives a Joy-Con pair no motion at all, so the guest's six-axis sensor has no source until
// the controller is put in 0x30 mode over HID. The right unit is the pair's source, as on a Switch; a left one is used only while no
// right one is open. One device is bound and the player slot comes from the front end, which owns the controller-to-player map.
// Opened without seizing: GameController keeps the buttons and sticks.
// Protocol: the subcommands ride in output report 0x01 ([report][packet][8 rumble bytes][subcommand][args], padded to 0x40) and the
// report id goes IN the buffer - left out, SetReport writes the packet number as the id and every field lands one byte early, which the
// controller still ACKs, for the argument byte's value instead of the subcommand's. Zeros mean the sensor is off (a unit at rest reads
// about 1 G), and macOS puts a Joy-Con back to simple mode, so the sequence is re-sent while the stream is not live.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"
#include "joycon_imu.h"

enum { JC_REPORT_LEN = 0x40, JC_ASSERT_NS = 2000000000 };
typedef struct jc_device {
    IOHIDDeviceRef hid; uint8_t in[128], out[JC_REPORT_LEN]; uint8_t packet;
    int right; uint64_t last_raw_log; jc_imu imu; struct jc_device *next;
} jc_device;
static IOHIDManagerRef g_jc_manager;
static jc_device *g_jc_devices, *g_jc_bound;
static int g_jc_player = -1, g_jc_active = 1, g_jc_controllers, g_jc_slot = -1;
static void (*g_jc_changed)(void);
static dispatch_source_t g_jc_timer;
// Diagnostic override, DM_PAD_IMU_MAP="x,y,z,x,y,z": six output axes (acc x,y,z then gyro x,y,z), each read from one of its half's own
// converted axes with a sign. It exists to identify a wrong axis or sign on a unit whose mounting disagrees with the driver convention;
// the compiled default is that convention, so nothing has to be set.
static int g_jc_map_on, g_jc_map_src[6], g_jc_raw_log;
static float g_jc_map_sgn[6];

static void jc_map_parse(void) {
    const char *e = getenv("DM_PAD_IMU_MAP");
    if (!e || !*e) return;
    int n = 0;
    for (const char *p = e; *p && n < 6; ) {
        float sg = 1; if (*p == '-') { sg = -1; p++; } else if (*p == '+') p++;
        int a = *p == 'x' ? 0 : *p == 'y' ? 1 : *p == 'z' ? 2 : -1;
        if (a < 0) { LOG("[hid] Joy-Con: DM_PAD_IMU_MAP='%s' is not like \"x,-y,z,x,-y,z\": ignored", e); return; }
        g_jc_map_src[n] = a; g_jc_map_sgn[n] = sg; n++; p++;
        if (*p == ',') p++;
    }
    if (n != 6) { LOG("[hid] Joy-Con: DM_PAD_IMU_MAP='%s' needs six axes: ignored", e); return; }
    g_jc_map_on = 1;
    LOG("[hid] Joy-Con: IMU map override '%s'", e);
}
// Each half's three tokens index its own three axes: a gyro token can never reach into the accelerometer's slots, which differ in sign
static void jc_map_samples(jc_imu_sample *s, int n) {
    if (!g_jc_map_on) return;
    for (int k = 0; k < n; k++) {
        float v[6] = {s[k].acc[0], s[k].acc[1], s[k].acc[2], s[k].rot[0], s[k].rot[1], s[k].rot[2]}, o[6];
        for (int i = 0; i < 6; i++) o[i] = g_jc_map_sgn[i] * v[(i < 3 ? 0 : 3) + g_jc_map_src[i]];
        s[k] = (jc_imu_sample){{o[0], o[1], o[2]}, {o[3], o[4], o[5]}, s[k].ns};
    }
}
// One subcommand in output report 0x01, with the report id as the buffer's first byte (see above)
static void jc_subcommand(jc_device *d, uint8_t sub, const uint8_t *args, size_t n) {
    static const uint8_t rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    if (n > JC_REPORT_LEN - 12) n = JC_REPORT_LEN - 12;
    memset(d->out, 0, sizeof d->out);
    d->out[0] = 0x01;
    d->out[1] = d->packet; d->packet = (uint8_t)((d->packet + 1) & 0x0f);
    memcpy(d->out + 2, rumble, sizeof rumble);
    d->out[10] = sub; if (n) memcpy(d->out + 11, args, n);
    IOReturn r = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, d->out, JC_REPORT_LEN);
    if (r != kIOReturnSuccess) LOG("[hid] Joy-Con: subcommand 0x%02x was not written (0x%x)", sub, r);
}
// Six-axis on, then vibration, then full report mode last (0x40 0x01, 0x48 0x01, 0x03 0x30), about 60 ms apart: the controller ignores
// the next output report when they arrive back to back
static void jc_assert_mode(jc_device *d) {
    uint8_t on = 1;
    jc_subcommand(d, 0x40, &on, 1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        uint8_t vib = 1;
        if (d == g_jc_bound && g_jc_active && !d->imu.live) jc_subcommand(d, 0x48, &vib, 1);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        uint8_t mode = 0x30;
        if (d == g_jc_bound && g_jc_active && !d->imu.live) jc_subcommand(d, 0x03, &mode, 1);
    });
}
static int jc_count(void) { int n = 0; for (jc_device *d = g_jc_devices; d; d = d->next) n++; return n; }
static void jc_route(void) {
    jc_device *best = NULL;
    for (jc_device *d = g_jc_devices; d; d = d->next) if (d->right) { best = d; break; }   // the right unit's sensor is the pair's
    for (jc_device *d = g_jc_devices; !best && d; d = d->next) best = d;                    // a left one only while there is no right
    int p = g_jc_controllers == 1 && g_jc_slot >= 0 ? g_jc_slot : -1;   // one Joy-Con pad and its slot: no guessing
    jc_device *d = p >= 0 ? best : NULL;
    if (d == g_jc_bound && p == g_jc_player) return;
    if (g_jc_player >= 0) dm_motion_clear_hid(g_jc_player);
    g_jc_bound = d; g_jc_player = p;
    if (d) {
        d->imu = (jc_imu){0};
        LOG("[hid] Joy-Con raw motion: player %d (%s unit, passive HID 0x30)", p + 1, d->right ? "right" : "left");
        jc_assert_mode(d);   // the pair has a player at once: ask for 0x30 rather than wait for the timer
    }
}
static void dm_motion_joycon_players(int joycon_controllers, int slot) {
    g_jc_controllers = joycon_controllers; g_jc_slot = slot; jc_route();
}
static void dm_motion_joycon_active(int active) {
    g_jc_active = active;
    if (!active && g_jc_player >= 0) dm_motion_clear_hid(g_jc_player);
}
static void jc_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    jc_device *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len <= 0 || !g_jc_active || d != g_jc_bound) return;
    if (report_id != 0x30) return;   // a subcommand's 0x21 reply: the stream is the proof, an ACK proves nothing on its own
    jc_imu_sample s[3];
    int n = joycon_imu_decode(&d->imu, r, (size_t)len, now_ns(), s);
    if (!n) return;
    jc_map_samples(s, n);
    for (int i = 0; i < n; i++) {
        dm_motion_set_hid(g_jc_player, s[i].acc, s[i].rot, s[i].ns);
        if (g_jc_raw_log && d->imu.received - d->last_raw_log > 100000000ull) {   // DM_MOTION_LOG=2: the raw counts, ten a second
            d->last_raw_log = d->imu.received;
            const uint8_t *b = r + 13 + 12 * i;
            int16_t v[6]; for (int q = 0; q < 6; q++) v[q] = (int16_t)((uint16_t)b[2 * q] | (uint16_t)b[2 * q + 1] << 8);
            LOG("[hid] Joy-Con raw: acc %d %d %d gyro %d %d %d", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
    }
}
static void jc_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess) return;
    for (jc_device *d = g_jc_devices; d; d = d->next) if (d->hid == hid) return;
    int right = 0;
    CFTypeRef pid = IOHIDDeviceGetProperty(hid, CFSTR("ProductID"));   // 0x2006 is the left unit, 0x2007 the right one
    if (pid && CFGetTypeID(pid) == CFNumberGetTypeID()) { int v = 0; if (CFNumberGetValue((CFNumberRef)pid, kCFNumberIntType, &v)) right = v == 0x2007; }
    LOG("[hid] Joy-Con raw motion: HID device matched (product %#x, %s unit)", (unsigned)(right ? 0x2007 : 0x2006), right ? "right" : "left");
    jc_device *d = calloc(1, sizeof *d);
    if (!d) { LOG("[RED] Joy-Con raw motion: cannot allocate device state"); return; }
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->right = right; d->next = g_jc_devices; g_jc_devices = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->in, sizeof d->in, jc_report, d);
    if (g_jc_changed) g_jc_changed(); else jc_route();
}
static void jc_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    jc_device **link = &g_jc_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    jc_device *d = *link; *link = d->next;
    IOHIDDeviceRegisterInputReportCallback(hid, d->in, sizeof d->in, NULL, NULL);
    if (g_jc_changed) g_jc_changed(); else jc_route();
    CFRelease(d->hid); free(d);
}
// macOS puts a Joy-Con back to simple mode (its own sleep, or its driver), so a bound device that has stopped reporting asks again
static void jc_assert_bound(void) {
    jc_device *d = g_jc_bound;
    if (!g_jc_active || !d || d->imu.live) return;   // the timer's own period bounds how often the sequence is re-sent
    jc_assert_mode(d);
}
static void dm_motion_joycon_start(void (*changed)(void)) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    e = getenv("DM_MOTION_HID"); if (e && !atoi(e)) return;   // the switch the Pro Controller's reader answers to
    e = getenv("DM_MOTION_LOG"); g_jc_raw_log = e && atoi(e) >= 2;
    jc_map_parse();
    g_jc_changed = changed;
    g_jc_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_jc_manager) return;
    CFMutableArrayRef matches = CFArrayCreateMutable(kCFAllocatorDefault, 2, &kCFTypeArrayCallBacks);
    if (!matches) { CFRelease(g_jc_manager); g_jc_manager = NULL; return; }
    for (int i = 0; i < 2; i++) {   // the left and the right unit are separate HID devices
        CFDictionaryRef m = (__bridge CFDictionaryRef)@{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @(i ? 0x2007 : 0x2006)};
        CFArrayAppendValue(matches, m);
    }
    IOHIDManagerSetDeviceMatchingMultiple(g_jc_manager, matches);
    CFRelease(matches);
    IOHIDManagerRegisterDeviceMatchingCallback(g_jc_manager, jc_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_jc_manager, jc_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_jc_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_jc_manager, kIOHIDOptionsTypeNone);   // not seized: GameController keeps the buttons and sticks
    CFSetRef seen = IOHIDManagerCopyDevices(g_jc_manager);   // none without the Input Monitoring grant, and Open still succeeds
    LOG("[hid] Joy-Con raw motion: HID %s, %ld matched device(s)", r == kIOReturnSuccess ? "open" : "open failed", seen ? (long)CFSetGetCount(seen) : 0);
    if (seen) CFRelease(seen);
    if (r != kIOReturnSuccess) {
        LOG("[RED] Joy-Con raw motion: cannot open HID (0x%x); a Joy-Con has no six-axis source", r);
        IOHIDManagerUnscheduleFromRunLoop(g_jc_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_jc_manager); g_jc_manager = NULL; return;
    }
    // The re-assert timer is a file-scope static: a dispatch source made inside a block dies with the block (and the log then lies)
    g_jc_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(g_jc_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)JC_ASSERT_NS), (uint64_t)JC_ASSERT_NS / 2, (uint64_t)JC_ASSERT_NS / 4);
    dispatch_source_set_event_handler(g_jc_timer, ^{ jc_assert_bound(); });
    dispatch_resume(g_jc_timer);
}
