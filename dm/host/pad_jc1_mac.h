// Mac-only transport for original Joy-Cons (Switch 1) over Bluetooth Classic HID, beside motion_pro_mac.h and
// pad_hid_mac.h. These controllers are paired in System Settings -> Bluetooth; macOS keeps the link and its HID
// driver enumerates them as one GameController (a combined L + R appears as "Joy-Con (L/R)") whose buttons,
// sticks and player slot the front end already reads. That driver gives no GCMotion, though, and zeroes the IMU
// in report 0x30, so the host has no orientation until it turns the sensor on itself: the handshake below is
// what the console does at connection, and macOS does not undo it (a device that wakes gets it again at its
// match). It is one motion source per player, not per Joy-Con: both halves of a combined pad feed the same
// player, and the game reads that player's merged six-axis stream, as it does for a DualSense.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "dm_input.h"
#include "pad_jc1.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

typedef struct jc1_dev {
    IOHIDDeviceRef hid; int pid, left;
    uint8_t report[512], counter;
    jc1_cal cal; jc1_stream stream; jc1_rest rest;
    int16_t origin[3];              // the factory's gyro origin, before the learned rest offset
    uint64_t last_report, last_reset, logged;
    int said;
    struct jc1_dev *next;
} jc1_dev;

static IOHIDManagerRef g_jc1_manager;
static dispatch_source_t g_jc1_timer;
static jc1_dev *g_jc1_devs;
static int g_jc1_route_player = -1;           // the player the wide mask and the per-Joy-Con streams were last pointed at
static int (*g_jc1_player)(GCController *);   // front_mac: the player of this Joy-Con's GameController, -1 without one
static int g_jc1_side, g_jc1_log;             // DM_JC1_SIDE: 0 auto (right of a pair, one Joy-Con alone -> the merged stream), 1 left, 2 right

static void jc1_send(jc1_dev *d, uint8_t sub, const uint8_t *arg, size_t n) {
    uint8_t out[JC1_REPORT_LEN];
    jc1_output_report(out, d->counter++, sub, arg, n);
    IOReturn r = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, out, sizeof out);
    if (r != kIOReturnSuccess && g_jc1_log) LOG("[jc1] subcommand 0x%02x on pid 0x%04x failed: 0x%x", sub, d->pid, r);
}
// The order matters (report mode before the sensor, and the controller drops a subcommand that comes too soon):
// device info, standard full mode, EnableIMU, then the factory IMU calibration read. 60 ms apart, as the console's.
static void jc1_handshake(jc1_dev *d) {
    const uint8_t info = 0, mode = JC1_INPUT_MODE_FULL, on = 1;
    const uint8_t spi[5] = {(uint8_t)(JC1_SPI_IMU_CAL & 0xff), (uint8_t)(JC1_SPI_IMU_CAL >> 8 & 0xff), (uint8_t)(JC1_SPI_IMU_CAL >> 16 & 0xff), (uint8_t)(JC1_SPI_IMU_CAL >> 24), JC1_IMU_CAL_LEN};
    jc1_send(d, JC1_SUB_DEVICE_INFO, &info, 0); usleep(60000);
    jc1_send(d, JC1_SUB_INPUT_MODE, &mode, 1); usleep(60000);
    jc1_send(d, JC1_SUB_ENABLE_IMU, &on, 1); usleep(60000);
    jc1_send(d, JC1_SUB_SPI_READ, spi, sizeof spi);
    d->last_reset = now_ns();
    if (g_jc1_log) LOG("[jc1] %s Joy-Con (pid 0x%04x): standard full mode and the 6-axis sensor asked for", d->left ? "left" : "right", d->pid);
}
static uint32_t jc1_sides(void) {   // the Joy-Cons attached now: bit 0 left, bit 1 right
    uint32_t m = 0;
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) m |= 1u << (d->left ? 0 : 1);
    return m;
}
// The per-Joy-Con streams and the pad's wide mask follow the player slot as it changes (a reconnected pair may take
// another one). The mask tells the npad layer each handle has its own sensor, so the game's fusion reads a real
// device 0 and device 1 instead of one stream on both; the pad's buttons and sticks stay with GameController.
static void jc1_route(int p) {
    if (g_jc1_route_player >= 0 && g_jc1_route_player != p) dm_joy_motion(g_jc1_route_player, 0);
    g_jc1_route_player = p;
    if (p >= 0) dm_joy_motion(p, jc1_sides());
}
// The pad's player: a combined Joy-Con pair is one GameController, so both devices resolve to the same player.
static int jc1_player_of(void) {
    for (GCController *c in GCController.controllers) {
        NSString *k = c.productCategory;
        if (k && [k containsString:@"Joy-Con"] && g_jc1_player) return g_jc1_player(c);
    }
    return -1;
}
// Which Joy-Con supplies the player's sensor: the right one of a pair (as SDL and the Switch 2 transport take a
// pair's sensor), the left one when it is alone or when the right one has not streamed.
static int jc1_chosen_left(void) {
    if (g_jc1_side) return g_jc1_side == 1;
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) if (!d->left && d->stream.live) return 0;
    return 1;
}
static void jc1_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    jc1_dev *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput) return;
    if (report_id == 0x21) {
        if (jc1_parse_cal(&d->cal, r, (size_t)len) && g_jc1_log) {
            for (int i = 0; i < 3; i++) d->origin[i] = (int16_t)d->cal.gyro_offset[i];   // the factory's origin, before the learned rest
            memset(&d->rest, 0, sizeof d->rest);   // what was learned without it is of no use now
            LOG("[jc1] %s Joy-Con: factory IMU calibration read (gyro %.4f/%.4f/%.4f deg/s per count, origin %.0f/%.0f/%.0f counts)",
                d->left ? "left" : "right", d->cal.gyro_scale[0], d->cal.gyro_scale[1], d->cal.gyro_scale[2],
                d->origin[0], d->origin[1], d->origin[2]);
        }
        return;
    }
    if (report_id != 0x30) return;
    uint64_t now = now_ns();
    d->last_report = now;
    if (!d->said && g_jc1_log) { d->said = 1; LOG("[jc1] %s Joy-Con (pid 0x%04x): report 0x30 at %ld bytes", d->left ? "left" : "right", d->pid, (long)len); }
    // The controller's own still offset, from a second of samples in which it hardly moves (jc1_rest_update)
    for (int k = 0; k < 3; k++) {
        const uint8_t *b = r + 13 + 12 * k;
        int16_t res[6];
        for (int i = 0; i < 3; i++) { res[i] = jc1_s16(b + 2 * i); res[3 + i] = (int16_t)(jc1_s16(b + 6 + 2 * i) - d->origin[i]); }
        jc1_rest_update(&d->rest, res);
    }
    jc1_rest_apply(&d->cal, &d->rest, d->origin);
    if (g_jc1_log >= 3 && now - d->logged > 1000000000ull) {   // the chip's own counts, against the calibration's origin
        d->logged = now;
        const uint8_t *b = r + 13 + 12 * 2;
        LOG("[jc1] %s Joy-Con raw: accel %d/%d/%d gyro %d/%d/%d counts (origin %d/%d/%d, the rest offset %.0f/%.0f/%.0f)",
            d->left ? "left" : "right", jc1_s16(b), jc1_s16(b + 2), jc1_s16(b + 4),
            jc1_s16(b + 6), jc1_s16(b + 8), jc1_s16(b + 10),
            d->origin[0], d->origin[1], d->origin[2], d->rest.bias[0], d->rest.bias[1], d->rest.bias[2]);
    }
    jc1_sample s[JC1_IMU_SAMPLES];
    int n = jc1_decode(&d->stream, r, (size_t)len, d->left, &d->cal, now, s);
    int p = jc1_player_of();
    jc1_route(p);
    if (n <= 0 || p < 0) return;
    if (g_jc1_log >= 2 && now - d->logged > 1000000000ull) {   // DM_JC1_LOG=2: the orientation the host hands the game, per Joy-Con
        d->logged = now;
        const float *a = s[2].acc, *w = s[2].rot;
        LOG("[jc1] %s Joy-Con frame: gravity %+.2f %+.2f %+.2f G (vertical, top up = 0 -1 0), turning %+.1f %+.1f %+.1f deg/s",
            d->left ? "left" : "right", a[0], a[1], a[2], w[0] * 57.2958f, w[1] * 57.2958f, w[2] * 57.2958f);
    }
    for (int i = 0; i < n; i++) {
        dm_motion_set_joy(p, d->left ? 0 : 1, s[i].acc, s[i].rot, s[i].ns);   // this Joy-Con's own sensor
        if (d->left == jc1_chosen_left()) dm_motion_set_hid(p, s[i].acc, s[i].rot, s[i].ns);   // the merged stream the handles fall back to
    }
}
static void jc1_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess) return;
    if (IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;   // GameController's own device
    int pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey)) intValue];
    if (pid != 0x2006 && pid != 0x2007) return;   // Joy-Con (L) and (R)
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) if (d->hid == hid) return;
    jc1_dev *d = calloc(1, sizeof *d);
    if (!d) { LOG("[RED] Joy-Con IMU: cannot allocate device state"); return; }
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->pid = pid; d->left = pid == 0x2006;
    jc1_cal_default(&d->cal);
    d->next = g_jc1_devs; g_jc1_devs = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, jc1_report, d);
    // Nonexclusive: macOS's own driver keeps the pad and its buttons, and both readbacks coexist (verified on a
    // paired L + R: GameController still lists the pad and both devices afterwards).
    IOReturn o = IOHIDDeviceOpen(hid, kIOHIDOptionsTypeNone);
    if (o != kIOReturnSuccess) {
        LOG("[RED] Joy-Con IMU: IOHIDDeviceOpen failed (0x%x); no motion from pid 0x%04x", o, pid);
        IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
        g_jc1_devs = d->next; CFRelease(d->hid); free(d);
        return;
    }
    jc1_handshake(d);
    jc1_route(jc1_player_of());
}
static void jc1_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    jc1_dev **link = &g_jc1_devs;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    jc1_dev *d = *link; *link = d->next;
    LOG("[jc1] %s Joy-Con gone", d->left ? "left" : "right");
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
    IOHIDDeviceClose(hid, kIOHIDOptionsTypeNone);
    CFRelease(d->hid); free(d);
    jc1_route(jc1_player_of());
}
// macOS resets the input report mode itself in some wakes (the IMU then reads zero, or no 0x30 arrives at all), and
// the controller drops a subcommand sent too soon: ask again once a second while a controller has gone quiet.
static void jc1_watchdog(void) {
    uint64_t now = now_ns();
    jc1_route(jc1_player_of());
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) {
        if (now - d->last_reset < 1000000000ull) continue;
        if (d->last_report && now - d->last_report < 1000000000ull && (d->stream.live || now - d->last_report < 2000000000ull)) continue;
        LOG("[jc1] %s Joy-Con: no live IMU for a second: asking for the sensor again", d->left ? "left" : "right");
        jc1_handshake(d);
    }
}
// DM_JC1=0 turns it off; DM_JC1_SIDE=L|R picks the sensor of a pair (default: right, or left when it is alone);
// DM_JC1_LOG=1 logs the handshake, the calibration and the first report of each controller.
static void dm_jc1_start(int (*player)(GCController *)) {
    const char *e = getenv("DM_JC1");
    if (e && !atoi(e)) return;
    g_jc1_player = player;
    {   // not getenv() && atoi(): that is 0 or 1, and DM_JC1_LOG=2 would never reach the frame log
        const char *l = getenv("DM_JC1_LOG");
        g_jc1_log = l ? atoi(l) : 0;
    }
    e = getenv("DM_JC1_SIDE");
    g_jc1_side = e && (*e == 'L' || *e == 'l') ? 1 : e && (*e == 'R' || *e == 'r') ? 2 : 0;
    NSArray *match = @[@{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2006},
                       @{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2007}];
    g_jc1_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_jc1_manager) return;
    IOHIDManagerSetDeviceMatchingMultiple(g_jc1_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_jc1_manager, jc1_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_jc1_manager, jc1_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_jc1_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_jc1_manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        LOG("[RED] Joy-Con IMU: cannot open the HID manager (0x%x)", r);
        IOHIDManagerUnscheduleFromRunLoop(g_jc1_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_jc1_manager); g_jc1_manager = NULL;
        return;
    }
    g_jc1_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(g_jc1_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_jc1_timer, ^{ jc1_watchdog(); });
    dispatch_resume(g_jc1_timer);
}
