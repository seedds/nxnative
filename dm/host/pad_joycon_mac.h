// Mac: Bluetooth Joy-Con (L) (0x057e:0x2006) and Joy-Con (R) (0x057e:0x2007) over IOHIDManager (pad_joycon.h, LNPANDY/NX2Apple#740).
// Enabled by default (DM_JOYCON_SPLIT=0 opts out to GameController): a Left + Right pair shares one player slot with dm_joy_attach(slot, 3)
// so games see JoyDual or auto-split JoyLeft + JoyRight, and SL+SR / L+R held 3 s (or DM_JOYCON_SPLIT=1) switches between paired and two-player sideways slots.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "pad_joycon.h"
#include "haptics_apple.h"
#include "joycon_rumble.h"
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
    int rumble_on;                // 0x48 0x01 has been sent: the Joy-Con ignores rumble data without it
    int rumble_reports;           // input reports seen, so the enable goes out once a report proves the link is up
    uint8_t rumble[4];            // the actuator's bytes as last sent, the neutral value until the game sends one
    struct joycon_pad *partner;   // the other half when paired on one player slot
    int primary;                  // 1 when this half owns the player slot
    // DM_JOYCON_LOG's meter, one line a second: what the Bluetooth link carried (joycon_meter)
    uint64_t meter_t0, reports, writes, max_gap_ns, last_rep_ns;
    uint8_t kinds[4]; int nkinds;   // the input report ids seen in the second: 0x30 is the one that carries IMU samples
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

// Output report 0x01: the actuator bytes first, then an optional subcommand. Every subcommand carries the actuator's
// current bytes, as SDL does: a report that reset them to neutral would cut a punch's feedback short whenever the IMU
// enable is repeated (below) or the rumble setting is sent again
static const uint8_t joycon_neutral[4] = {0x00, 0x01, 0x40, 0x40};
static void joycon_cmd(joycon_pad *d, uint8_t sub, uint8_t arg) {
    if (!d || !d->hid) return;
    uint8_t rep[49] = {0};
    rep[0] = 0x01;
    rep[1] = (uint8_t)(d->subcmd_seq++ & 0x0f);
    memcpy(rep + 2, d->rumble, 4);   // the left actuator
    memcpy(rep + 6, d->rumble, 4);   // the right one: a Joy-Con alone answers whichever of the two it is
    rep[10] = sub;
    rep[11] = arg;
    IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, rep, sizeof rep);
}
static void joycon_enable_imu(joycon_pad *d) {
    joycon_cmd(d, 0x40, 0x01);   // the 6-axis IMU sensor
    joycon_cmd(d, 0x03, 0x30);   // 60 Hz standard full input reports
}
// Without this the Joy-Con takes its rumble bytes and stays still: the firmware's vibration switch is off by default
// over Bluetooth (SDL sends the same subcommand at init). Once is enough; the console keeps it until it disconnects
static void joycon_enable_rumble(joycon_pad *d) {
    joycon_cmd(d, 0x48, 0x01);
    d->rumble_on = 1;
}
// The game's per-motor values as this Joy-Con's actuators (joycon_rumble.h), sent only when they change: dm_haptics_send
// already drops equal values, so this is one report per change, and a Joy-Con takes about 66 a second over Bluetooth
static void joycon_rumble_write(joycon_pad *d, const uint8_t v[4]) {
    if (!d || !d->hid || !memcmp(v, d->rumble, sizeof d->rumble)) return;
    d->writes++;   // what DM_JOYCON_LOG reports: the output reports this Joy-Con took
    memcpy(d->rumble, v, sizeof d->rumble);
    uint8_t rep[49] = {0};
    rep[0] = 0x01;
    rep[1] = (uint8_t)(d->subcmd_seq++ & 0x0f);
    memcpy(rep + 2, v, 4);
    memcpy(rep + 6, v, 4);
    IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, 0x01, rep, sizeof rep);
}
// A player's sink (dm_haptics_apple_set_sink). macOS's GCController gives a 1st-generation Joy-Con no haptics at all, so
// the Core Haptics route found no output and every value the game sent was dropped: the motors are driven here instead,
// the left Joy-Con's by motor 0 and the right one's by motor 1 as nn::hid addresses them (hle_hid.c vib_targets)
static joycon_pad *g_joycon_sink[DM_PLAYERS];
static float g_joycon_gain = 1;
static float g_joycon_freq = 1;   // how far each band follows the game's frequency (0 = SDL's fixed pair)
static int g_joycon_log;          // DM_JOYCON_LOG=1: a line a second per Joy-Con with what the link carried
// What the link carried in the last second: input reports, the longest gap between two, the report kinds, the rumble reports
// written. A punch the game did not see shows up here first: as a gap of tens of milliseconds, or as reports of a kind that
// carries no IMU samples
static void joycon_meter(joycon_pad *d, uint8_t kind, uint64_t now) {
    if (!g_joycon_log || !d) return;
    if (d->last_rep_ns && now > d->last_rep_ns && now - d->last_rep_ns > d->max_gap_ns) d->max_gap_ns = now - d->last_rep_ns;
    d->last_rep_ns = now; d->reports++;
    int known = 0;
    for (int i = 0; i < d->nkinds; i++) if (d->kinds[i] == kind) known = 1;
    if (!known && d->nkinds < 4) d->kinds[d->nkinds++] = kind;
    if (!d->meter_t0) { d->meter_t0 = now; return; }
    if (now - d->meter_t0 < 1000000000ull) return;
    char kinds[16] = {0};
    for (int i = 0; i < d->nkinds; i++) { char one[6]; snprintf(one, sizeof one, i ? " %02X" : "%02X", d->kinds[i]); strncat(kinds, one, sizeof kinds - strlen(kinds) - 1); }
    LOG("joycon '%s': %llu input reports, longest gap %.0f ms, kinds %s, %llu rumble writes",
        d->name, (unsigned long long)d->reports, (double)d->max_gap_ns / 1e6, kinds[0] ? kinds : "none", (unsigned long long)d->writes);
    d->reports = d->writes = d->max_gap_ns = 0; d->nkinds = 0; d->meter_t0 = now;
}
static void joycon_rumble_sink(const dm_haptic_state *st) {
    if (!st || st->player < 0 || st->player >= DM_PLAYERS) return;
    joycon_pad *d = g_joycon_sink[st->player];
    if (!d) return;
    uint8_t left[4], right[4];
    joycon_rumble_bytes(left, &st->motor[0], g_joycon_gain, g_joycon_freq);
    joycon_rumble_bytes(right, &st->motor[1], g_joycon_gain, g_joycon_freq);
    if (d->partner) {
        joycon_rumble_write(d->st.pid == JOYCON_PID_L ? d : d->partner, left);
        joycon_rumble_write(d->st.pid == JOYCON_PID_R ? d : d->partner, right);
        return;
    }
    // One Joy-Con held alone: a sideways pad is the full-key controller, so the game may drive either motor of it.
    // Its own side's value while that is the louder one, the other's otherwise
    int side = joycon_side(d->st.pid);
    const dm_vibration *own = &st->motor[side], *peer = &st->motor[side ^ 1];
    float own_level = own->amplitude_low + own->amplitude_high;
    if (peer->amplitude_low + peer->amplitude_high > own_level || own_level == 0) side ^= 1;
    joycon_rumble_write(d, side ? right : left);
}
static void joycon_sink_bind(int p, joycon_pad *d) {
    if (p < 0 || p >= DM_PLAYERS) return;
    joycon_pad *old = g_joycon_sink[p];
    g_joycon_sink[p] = d;
    if (!d && old) {   // silence before the route goes, or the last value keeps running on a pad left connected
        joycon_rumble_write(old, joycon_neutral);
        if (old->partner) joycon_rumble_write(old->partner, joycon_neutral);
    }
    if (d && d != old) LOG("controller '%s': rumble over its HID output reports (player %d, gain %.2f, frequency %.2f)", d->name, p + 1, g_joycon_gain, g_joycon_freq);
    dm_haptics_apple_set_sink(p, d ? joycon_rumble_sink : NULL);
}
// A removed pad is freed after its detach: nothing may still point at it
static void joycon_sink_forget(joycon_pad *d) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_joycon_sink[p] == d) joycon_sink_bind(p, NULL);
}

static void joycon_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *rep, CFIndex len) {
    (void)sender; (void)type; (void)id;
    joycon_pad *d = ctx;
    if (result != kIOReturnSuccess || !d || len <= 0) return;
    joycon_meter(d, rep[0], now_ns());
    int was_home = d->st.home;
    if (!joycon_decode_0x30(&d->st, rep, (size_t)len)) return;
    joycon_read(d);
    joycon_read_motion(d, rep, (size_t)len);
    if (!d->motion.live && (++d->imu_retry % 60) == 1) joycon_enable_imu(d);
    if (!d->rumble_reports) joycon_enable_rumble(d);   // the link is proven only now: a subcommand at connect can precede it
    d->rumble_reports++;
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
    memcpy(d->rumble, joycon_neutral, sizeof d->rumble);
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
    joycon_enable_rumble(d);
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
    joycon_sink_forget(d);
    CFRelease(d->hid);
    free(d);
}

static void joycon_start(void) {
    const char *e = getenv("DM_JOYCON_SPLIT");
    g_joycon_split = !e || atoi(e) != 0;
    g_joycon_force_split = e && atoi(e) > 0;
    // Rumble strength, as DM_RUMBLE_GAIN is for the Core Haptics route: a Joy-Con's actuator is small and a fitness game
    // may want more of it than the console's own drive gives
    if ((e = getenv("DM_JOYCON_RUMBLE_GAIN"))) { float v = strtof(e, NULL); if (isfinite(v)) g_joycon_gain = fmaxf(0, fminf(3, v)); }
    // Whether each band keeps the game's frequency (1, the default) or SDL's fixed ~150 Hz pair (0), or a mix of the two: the
    // console also corrects a band's amplitude for its frequency, so a band driven far from where a Joy-Con's actuator is
    // loudest can feel weaker than the same amplitude at 150 Hz
    if ((e = getenv("DM_JOYCON_RUMBLE_FREQ"))) { float v = strtof(e, NULL); if (isfinite(v)) g_joycon_freq = fmaxf(0, fminf(1, v)); }
    g_joycon_log = (e = getenv("DM_JOYCON_LOG")) && atoi(e) != 0;
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
