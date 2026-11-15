// Mac-only transport for original Joy-Cons (Switch 1) over Bluetooth Classic HID, beside motion_pro_mac.h and
// pad_hid_mac.h. These controllers are paired in System Settings -> Bluetooth; macOS keeps the link and its HID
// driver enumerates them as GameController controllers (a combined L + R appears as one, "Joy-Con (L/R)") whose
// buttons, sticks, player slot and rumble the front end already reads. That driver gives no GCMotion, though, and
// zeroes the IMU in report 0x30, so the host has no orientation until it turns the sensor on itself: the handshake
// below is what the console does at connection, and macOS does not undo it (a device that wakes gets it again from
// the watchdog). Each Joy-Con feeds its own handle of its player's pad (dm_joy_motion, dm_motion_set_joy), and one
// of them the player's merged six-axis stream (dm_motion_set_hid), as a DualSense's does.
// Which player a Joy-Con is: the player of its GameController controller, found as jc1_route says (pad_jc1.h's
// routing: the only Joy-Con controller, else a button press). All callbacks, timers and routing run on the main run
// loop, alongside front_mac.m's player slots.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "dm_input.h"
#include "pad_jc1.h"
#include <stdlib.h>
#include <stdio.h>

enum { JC1_HANDSHAKE_STEPS = 4 };
static const uint64_t JC1_STEP_NS = 60000000, JC1_WAIT_NS = 1000000000, JC1_WAIT_MAX_NS = 8000000000;
typedef struct jc1_dev {
    IOHIDDeviceRef hid; int pid, left; uint64_t id;
    uint8_t report[512], counter;
    jc1_unit unit; jc1_press press;
    CFTypeRef owner;                // its GameController controller (retained), NULL while it is not known
    int player;                     // the owner's player, -1 without one
    int steps;                      // handshake subcommands still to send (jc1_step)
    uint64_t asked, wait;           // when the last handshake began; the watchdog's wait (1 s, doubling to 8 s while the sensor stays off)
    uint64_t logged;
    int said, quiet_said, unsure_said;
    struct jc1_dev *next;
} jc1_dev;

static IOHIDManagerRef g_jc1_manager;
static dispatch_source_t g_jc1_timer;
static jc1_dev *g_jc1_devs;
static uint64_t g_jc1_next_id;
static jc1_dev *g_jc1_feed[DM_PLAYERS][2];    // the Joy-Con whose sensor feeds the player's handle of that side (jc1_route)
static uint32_t g_jc1_sides_set[DM_PLAYERS];  // the sides last given to dm_joy_motion
static int (*g_jc1_player)(GCController *);   // front_mac: the player of a GameController controller, -1 without one
static int g_jc1_side, g_jc1_log;             // DM_JC1_SIDE: 0 auto (right of a pair, the left of a lone one -> the merged stream), 1 left, 2 right

static const char *jc1_name(const jc1_dev *d) { return d->left ? "left" : "right"; }
static void jc1_send(jc1_dev *d, uint8_t sub, const uint8_t *arg, size_t n) {
    uint8_t out[JC1_REPORT_LEN];
    jc1_output_report(out, d->counter++, sub, arg, n);
    IOReturn r = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, out, sizeof out);
    if (r != kIOReturnSuccess && g_jc1_log) LOG("[jc1] subcommand 0x%02x on pid 0x%04x failed: 0x%x", sub, d->pid, r);
}
static jc1_dev *jc1_by_id(uint64_t id) { for (jc1_dev *d = g_jc1_devs; d; d = d->next) if (d->id == id) return d; return NULL; }
// The order matters (report mode before the sensor, and the controller drops a subcommand that comes too soon): device info, standard full mode, EnableIMU, then the factory
// IMU calibration read, 60 ms apart as the console's. Each step is its own main-queue block, so the run loop (the front end's input among it) never waits on the controller;
// a device removed in between is no longer found by its id.
static void jc1_step(uint64_t id) {
    jc1_dev *d = jc1_by_id(id);
    if (!d || !d->steps) return;
    const uint8_t mode = JC1_INPUT_MODE_FULL, on = 1;
    const uint8_t spi[5] = {(uint8_t)(JC1_SPI_IMU_CAL & 0xff), (uint8_t)(JC1_SPI_IMU_CAL >> 8 & 0xff), 0, 0, JC1_IMU_CAL_LEN};
    switch (JC1_HANDSHAKE_STEPS - d->steps--) {
    case 0: jc1_send(d, JC1_SUB_DEVICE_INFO, NULL, 0); break;
    case 1: jc1_send(d, JC1_SUB_INPUT_MODE, &mode, 1); break;
    case 2: jc1_send(d, JC1_SUB_ENABLE_IMU, &on, 1); break;
    default: jc1_send(d, JC1_SUB_SPI_READ, spi, sizeof spi); break;
    }
    if (d->steps) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)JC1_STEP_NS), dispatch_get_main_queue(), ^{ jc1_step(id); });
}
static void jc1_handshake(jc1_dev *d) {
    if (d->steps) return;   // one is under way
    d->steps = JC1_HANDSHAKE_STEPS; d->asked = now_ns();
    if (g_jc1_log) LOG("[jc1] %s Joy-Con (pid 0x%04x): standard full mode and the 6-axis sensor asked for", jc1_name(d), d->pid);
    jc1_step(d->id);
}

// GameController's Joy-Con controllers, by product category ("Nintendo Switch Joy-Con (L)", "(R)", "(L/R)", as pad_apple.h): the sides each can hold (bit 0 left, bit 1 right)
static int jc1_gc_sides(GCController *c) {
    NSString *k = c.productCategory;
    if (!c.extendedGamepad || ![k containsString:@"Joy-Con"]) return 0;
    return [k hasSuffix:@"(L)"] ? 1 : [k hasSuffix:@"(R)"] ? 2 : 3;
}
static int jc1_gc_pressed(GCController *c) {   // the buttons a Joy-Con's report also carries; the sticks are not buttons there
    GCExtendedGamepad *g = c.extendedGamepad;
    GCControllerButtonInput *b[] = {g.buttonA, g.buttonB, g.buttonX, g.buttonY, g.leftShoulder, g.rightShoulder, g.leftTrigger, g.rightTrigger, g.buttonMenu, g.buttonOptions,
                                    g.buttonHome, g.leftThumbstickButton, g.rightThumbstickButton, g.dpad.up, g.dpad.down, g.dpad.left, g.dpad.right};
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) if (b[i].isPressed) return 1;   // nil on a pad without it: not pressed
    return 0;
}
static void jc1_bind(jc1_dev *d, GCController *c, const char *why) {
    if (d->owner == (__bridge CFTypeRef)c) return;
    if (d->owner) CFRelease(d->owner);
    d->owner = c ? CFBridgingRetain(c) : NULL;
    if (c) LOG("[jc1] %s Joy-Con (pid 0x%04x): motion goes with '%s' (%s)", jc1_name(d), d->pid, c.vendorName.UTF8String ?: "?", why);
}
// Points each Joy-Con at its player, and each player's handles at its Joy-Cons. Run at every controller change (gone: a controller just removed, still listed), at a Joy-Con's
// arrival or removal, at a press that moves one, and once a second (a player slot taken at a button press, DM_PAD_PRESS_TO_JOIN).
static void jc1_route(GCController *gone) {
    NSMutableArray<GCController *> *pads = [NSMutableArray array];
    for (GCController *c in GCController.controllers) if (c != gone && jc1_gc_sides(c)) [pads addObject:c];
    int nside[2] = {0, 0};
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) {
        if (d->owner && ![pads containsObject:(__bridge GCController *)d->owner]) jc1_bind(d, nil, NULL);   // its controller went
        nside[d->left ? 0 : 1]++;
    }
    // One Joy-Con controller and no more than one Joy-Con of a side: it can only be theirs. A press still corrects it (a pair whose controller GameController lists later)
    if (pads.count == 1 && nside[0] <= 1 && nside[1] <= 1)
        for (jc1_dev *d = g_jc1_devs; d; d = d->next) if (!d->owner && (jc1_gc_sides(pads[0]) >> (d->left ? 0 : 1) & 1)) jc1_bind(d, pads[0], "the only Joy-Con controller");
    jc1_dev *feed[DM_PLAYERS][2] = {{0}};
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) {
        d->player = d->owner && g_jc1_player ? g_jc1_player((__bridge GCController *)d->owner) : -1;
        if (d->player >= 0 && d->player < DM_PLAYERS && !feed[d->player][d->left ? 0 : 1]) feed[d->player][d->left ? 0 : 1] = d;   // DM_PAD_MERGE: the first of a side
        else d->player = -1;
    }
    for (int p = 0; p < DM_PLAYERS; p++) {
        uint32_t sides = (feed[p][0] ? 1u : 0) | (feed[p][1] ? 2u : 0);
        for (int s = 0; s < 2; s++) if (g_jc1_feed[p][s] && feed[p][s] != g_jc1_feed[p][s]) dm_motion_clear_joy(p, s);   // another Joy-Con, or none
        if (sides != g_jc1_sides_set[p]) {
            dm_joy_motion(p, sides);
            if (!sides) dm_motion_clear_hid(p);
            g_jc1_sides_set[p] = sides;
        }
    }
    memcpy(g_jc1_feed, feed, sizeof feed);
}
// The Joy-Con that also feeds the player's merged stream: the right one of a pair (as SDL and the Switch 2 transport take a pair's sensor), the left one when it is alone
// or the right one is not streaming; DM_JC1_SIDE picks one
static jc1_dev *jc1_merged(int p, uint64_t now) {
    jc1_dev *l = g_jc1_feed[p][0], *r = g_jc1_feed[p][1];
    if (g_jc1_side) return g_jc1_side == 1 ? (l ? l : r) : (r ? r : l);
    return r && (jc1_live(&r->unit.stream, now) || !l) ? r : l;
}
// A press on d (pad_jc1.h's routing): judged against every Joy-Con's press and the Joy-Con controllers of its side that show a button pressed
static void jc1_judge(jc1_dev *d, uint64_t now) {
    enum { MAX = 16 };
    jc1_press all[MAX]; int n = 0, self = -1;
    for (jc1_dev *o = g_jc1_devs; o && n < MAX; o = o->next) { if (o == d) self = n; all[n++] = o->press; }
    if (self < 0) return;
    int side = d->left ? 1 : 2, pressed = 0; GCController *hit = nil;
    for (GCController *c in GCController.controllers) if ((jc1_gc_sides(c) & side) && jc1_gc_pressed(c)) { pressed++; hit = c; }
    int v = jc1_press_judge(all, n, self, pressed, now);
    if (v == JC1_PRESS_UNSURE) {
        d->press.judged = 1;
        if (!d->owner && !d->unsure_said) { d->unsure_said = 1; LOG("[jc1] %s Joy-Con (pid 0x%04x): a press named no single Joy-Con controller; its motion waits for the next", jc1_name(d), d->pid); }
    } else if (v == JC1_PRESS_BIND) {
        d->press.judged = 1;
        if (d->owner != (__bridge CFTypeRef)hit) { jc1_bind(d, hit, d->owner ? "moved by a button press" : "a button press"); jc1_route(nil); }
    }
}
static void jc1_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    jc1_dev *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len <= 0) return;
    size_t n = (size_t)len;
    if (report_id == 0x21) {
        if (jc1_unit_cal(&d->unit, r, n) && g_jc1_log) {
            const jc1_cal *c = &d->unit.cal;
            LOG("[jc1] %s Joy-Con: factory IMU calibration read (gyro %.4f/%.4f/%.4f deg/s per count, origin %.0f/%.0f/%.0f counts)",
                jc1_name(d), c->gyro_scale[0], c->gyro_scale[1], c->gyro_scale[2], c->gyro_origin[0], c->gyro_origin[1], c->gyro_origin[2]);
        }
        return;
    }
    if (report_id != 0x30 || n < JC1_REPORT_LEN || r[0] != 0x30) return;   // nothing below reads a report shorter than 0x30's 49 bytes
    uint64_t now = now_ns();
    if (!d->said && g_jc1_log) { d->said = 1; LOG("[jc1] %s Joy-Con (pid 0x%04x): report 0x30 at %ld bytes", jc1_name(d), d->pid, (long)len); }
    jc1_press_update(&d->press, jc1_buttons(r) != 0, now);
    if (d->press.pressing && !d->press.judged) jc1_judge(d, now);
    jc1_sample s[JC1_IMU_SAMPLES];
    int k = jc1_input(&d->unit, r, n, d->left, now, s);
    if (k > 0) { d->quiet_said = 0; if (d->unit.cal.have) d->wait = JC1_WAIT_NS; }   // streaming and calibrated: the watchdog's wait starts over
    if (g_jc1_log >= 3 && now - d->logged > 1000000000ull) {   // the chip's own counts, against the calibration's origin and the learned rest
        d->logged = now;
        const uint8_t *b = r + 13 + 12 * 2; const jc1_unit *u = &d->unit;
        LOG("[jc1] %s Joy-Con raw: accel %d/%d/%d gyro %d/%d/%d counts (origin %.0f/%.0f/%.0f, the rest offset %.0f/%.0f/%.0f)",
            jc1_name(d), jc1_s16(b), jc1_s16(b + 2), jc1_s16(b + 4), jc1_s16(b + 6), jc1_s16(b + 8), jc1_s16(b + 10),
            u->cal.gyro_origin[0], u->cal.gyro_origin[1], u->cal.gyro_origin[2], u->rest.bias[0], u->rest.bias[1], u->rest.bias[2]);
    }
    int p = d->player;
    if (k <= 0 || p < 0 || g_jc1_feed[p][d->left ? 0 : 1] != d) return;   // no samples, no player, or another Joy-Con of this side feeds it (DM_PAD_MERGE)
    if (g_jc1_log == 2 && now - d->logged > 1000000000ull) {   // DM_JC1_LOG=2: the orientation the host hands the game, per Joy-Con
        d->logged = now;
        const float *a = s[k - 1].acc, *w = s[k - 1].rot;
        LOG("[jc1] %s Joy-Con frame: gravity %+.2f %+.2f %+.2f G (vertical, top up = 0 -1 0), turning %+.1f %+.1f %+.1f deg/s",
            jc1_name(d), a[0], a[1], a[2], w[0] * 57.2958f, w[1] * 57.2958f, w[2] * 57.2958f);
    }
    int merged = jc1_merged(p, now) == d;
    for (int i = 0; i < k; i++) {
        dm_motion_set_joy(p, d->left ? 0 : 1, s[i].acc, s[i].rot, s[i].ns);   // this Joy-Con's own sensor
        if (merged) dm_motion_set_hid(p, s[i].acc, s[i].rot, s[i].ns);       // the merged stream a handle without its own falls back to
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
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->pid = pid; d->left = pid == 0x2006; d->id = ++g_jc1_next_id; d->player = -1; d->wait = JC1_WAIT_NS;
    jc1_unit_init(&d->unit);
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
    jc1_route(nil);
}
static void jc1_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    jc1_dev **link = &g_jc1_devs;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    jc1_dev *d = *link; *link = d->next;
    LOG("[jc1] %s Joy-Con gone", jc1_name(d));
    // Unregister while its buffer and context still exist; the main run loop serializes removal with its report callbacks and its handshake steps
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
    IOHIDDeviceClose(hid, kIOHIDOptionsTypeNone);
    jc1_bind(d, nil, NULL);
    CFRelease(d->hid); free(d);
    jc1_route(nil);
}
// macOS resets the input report mode itself in some wakes (the IMU then reads zero, or no 0x30 arrives at all), and the controller drops a subcommand sent too soon: a
// Joy-Con with no sensor data since its last handshake gets another, after a second and then after twice as long each time it stays off, up to 8 s (a Joy-Con whose sensor
// never comes on is asked every 8 s, not every second). One line says so per silence; DM_JC1_LOG names every retry. A Joy-Con that streams but never answered the
// calibration read (a subcommand it dropped) is asked again on the same schedule: without it, its gyro scale and origin are the defaults.
static void jc1_watchdog(void) {
    uint64_t now = now_ns();
    jc1_route(nil);
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) {
        if (d->steps) continue;
        if (jc1_quiet(&d->unit.stream, d->asked, d->wait, now)) {
            if (!d->quiet_said || g_jc1_log) LOG("[jc1] %s Joy-Con: no sensor data for %.0f s: asking for the sensor again", jc1_name(d), (double)d->wait / 1e9);
            d->quiet_said = 1;
        } else if (d->unit.cal.have || now - d->asked < d->wait) continue;   // streaming, and its calibration read (or not yet due again)
        else if (g_jc1_log) LOG("[jc1] %s Joy-Con: no calibration reply: asking again", jc1_name(d));
        d->wait = d->wait * 2 > JC1_WAIT_MAX_NS ? JC1_WAIT_MAX_NS : d->wait * 2;
        jc1_handshake(d);
    }
}
// front_mac.m's controllers changed (gone: one just removed, still listed)
static void dm_jc1_changed(GCController *gone) { if (g_jc1_manager) jc1_route(gone); }
// DM_JC1=0 turns it off; DM_JC1_SIDE=L|R picks the sensor of a pair for the merged stream (default: right, or left when it is alone);
// DM_JC1_LOG=1 logs the handshake, the calibration and the first report of each controller, 2 a frame line a second per Joy-Con, 3 the raw counts instead.
static void dm_jc1_start(int (*player)(GCController *)) {
    const char *e = getenv("DM_JC1");
    if (e && !atoi(e)) return;
    g_jc1_player = player;
    e = getenv("DM_JC1_LOG");
    g_jc1_log = e ? atoi(e) : 0;
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
