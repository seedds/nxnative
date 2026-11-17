// Mac: raw HID gamepads that GameController does not list (pad_hid.h, GitHub issue #637), read on the main run loop alongside front_mac.m's GameController players.
// Each device takes a player slot at its first button press (front_mac.m hidpad_first_press: a keyboard receiver that declares a GamePad collection never presses one, GitHub
// issue #1657); front_mac.m defines the slot side (hidpad_attach / hidpad_detach / hidpad_read / hidpad_motion).
// Skipped, so no controller is read twice: GameController's synthetic devices, and a device GameController lists itself. +[GCController supportsHIDDevice:] is only a hint for
// that: it answers YES for the 8BitDo Ultimate 2C (2dc8:301b), which GameController then never lists (GitHub issue #965). So a device it says YES for waits HIDPAD_GC_WAIT
// seconds for a GCController of its name; without one it is read as HID, and given back if that GCController connects later. Microsoft's, Sony's and Nintendo's pads
// (hidpad_gc_native) are trusted without the wait only for physical devices: GameController supports them itself, under names that need not be their HID product strings.
// A virtual device can reuse those vendors without GameController publishing it (Steam Link, GitHub issue #2465), so it needs the same usable-controller check and wait.
// DM_HID_PADS=0 turns this off.
// Identical pads (GitHub issue #1218): GameController can list fewer pads of one model than IOKit has; two Switch Pro Controllers over USB are listed as one (their USB serial
// numbers match, which may be why; over Bluetooth both are listed). So a device GameController reads is kept, unread, in g_hidpads_gc, to count each model's pads. When IOKit
// has at least two pads of a model and more than GameController lists, after HIDPAD_TWIN_WAIT for a pad not listed yet (pad_dupes_want), every pad of that model is read here,
// a player each, by USB location (twins): a Pro Controller from its report 0x30 (pad_pro.h, pro_motion), with its gyro and its own stick calibration, any other model from
// its HID elements. GameController gives a controller no HID identity, so front_mac.m leaves GameController's controllers of that model unread (hidpad_gc_unread) rather than
// read a pad twice. When GameController lists them all again, they go back to it. A model is matched to GameController's controllers by its name (vendorName is the HID product
// string), a Pro Controller by its product category; the other pads of Microsoft, Sony and Nintendo cannot be matched by name, so they are never read this way.
// Such pads have no rumble or player lights, and Home does not quit from them. DM_PAD_IDENTICAL=0 turns this part off.
// Only entries with a pad behind them are counted (hidpad_live): a Pro Controller's while it sends report 0x30 (pro_live), another model's once it has sent any input
// report. After a long Bluetooth disconnect IOKit kept a second entry of one Pro Controller, at USB location 0, that sends nothing and whose output fails with
// MACH_SEND_INVALID_DEST; counted, it made one pad two and both players (GitHub issue #2237).
// Nintendo's protocol (pad_hid.h, PAD_NIN_PROTOCOL; GitHub PR #989): a pad read here whose report descriptor is Nintendo's (a Pro Controller clone of another vendor, such as a
// PowerA NANO, "Lic Pro Controller", HID vendor and product 0) is asked for its device info; one that answers in report 0x21 gets the console's handshake (jc1_handshake_report:
// full mode, its sensor and vibration on, its stick calibrations) and its player lights, and from its first report 0x30 it is read as a Pro Controller (pad_pro.h, its own
// stick calibration; pad_jc1.h, its calibrated gyro), with rumble (output report 0x10) when its descriptor has that report. Player lights need only the descriptor's subcommand
// channel (0x3F, 0x21 and output 0x01), so a pad that lacks full mode or does not answer device info can still show its player. Until full mode, and for good when it does not answer
// (HIDPAD_NIN_TRIES asks) or stays in its simple report 0x3F, it is read from that report's elements (pad_hid.h's numbering and raw sticks). One that falls back to report 0x3F
// later (a wake) is read from it again and asked again.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <os/lock.h>
#include "pad_hid.h"
#include "pad_pro.h"
#include "pad_jc1.h"
#include "motion_pro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hid_pad {
    IOHIDDeviceRef hid; pad_hid st; char name[64]; int vid, pid, gc_hint; struct hid_pad *next;
    int twin, twin_want;   // twin: read here although GameController lists its model; twin_want: hidpad_twins_eval's choice for it
    uint64_t heard; int streams, shown;   // heard: its last input report; streams: it sends a Pro Controller's 0x30 (hidpad_live); shown: 1 / 2 logged as not counted / counted
    pad_pro pro; uint64_t pro_m; int32_t pro_s[4]; pro_motion motion; uint8_t pro_counter; int pro_logged;   // from its report 0x30 (a twin Pro Controller, or full), not from st
    jc1_unit unit;         // a protocol pad's IMU calibration, rest offset and sample clock (the Pro Controller uses the left Joy-Con's axes)
    int nin;               // its descriptor's Nintendo reports (pad_hid_nin_bit)
    int proto, talks, full, steps, tries;   // proto: driven by Nintendo's protocol here; talks: it answered; full: it sends report 0x30; steps: handshake steps left; tries: this round's asks
    int asks, answered;    // device info asks so far, and the last one answered
    uint64_t asked;        // when the last handshake began
    int lights, rumble_off, sink;   // lights: the pattern last sent (-1 none); rumble_off: silent reports still to send after a buzz; sink: 1 + the player its rumble goes to
    uint8_t report[512];   // its report buffer
} hid_pad;
static IOHIDManagerRef g_hidpad_manager;
static hid_pad *g_hidpads, *g_hidpads_waiting;   // waiting: GameController says it lists the device (gc_hint); read as HID if it has not after HIDPAD_GC_WAIT
static hid_pad *g_hidpads_gc;                    // GameController reads them: kept, unread, to count identical pads
#define HIDPAD_GC_WAIT 2.0
#define HIDPAD_TWIN_WAIT 3.0
static int g_hidpad_log, g_hidpad_motion = 1, g_twins_off, g_twins_due, g_twins_waiting;
static void hidpad_attach(hid_pad *d);
static void hidpad_detach(hid_pad *d);
static void hidpad_read(hid_pad *d);
static void hidpad_motion(hid_pad *d, const float acc[3], const float rot[3], uint64_t ns);
static void hidpad_models_changed(void);   // the pads of a model changed: front_mac.m runs hidpad_twins_eval / _release / _take again
static int hidpad_player(const hid_pad *d);   // front_mac.m: its player, -1 before it joins

static int hidpad_is_pro(const hid_pad *d) { return d->vid == 0x057e && d->pid == 0x2009; }
// Whether its buttons and sticks come from report 0x30 (pro_m, pro_s) rather than its elements (st): a twin Pro Controller, or a pad of Nintendo's protocol in full mode
static int hidpad_full(const hid_pad *d) { return (d->twin && hidpad_is_pro(d)) || d->full; }
static void hidpad_value(void *ctx, IOReturn result, void *sender, IOHIDValueRef v) {
    (void)sender;
    hid_pad *d = ctx; if (result != kIOReturnSuccess || d->full) return;   // full: report 0x3F's elements are not sent any more
    IOHIDElementRef e = IOHIDValueGetElement(v);
    uint32_t page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e);
    CFIndex x = IOHIDValueGetIntegerValue(v), min = IOHIDElementGetLogicalMin(e), max = IOHIDElementGetLogicalMax(e);
    if (page == kHIDPage_Button && usage >= 1 && usage <= 32) {
        uint32_t bit = 1u << (usage - 1), was = d->st.buttons & bit;
        d->st.buttons = x ? d->st.buttons | bit : d->st.buttons & ~bit;
        if (g_hidpad_log && x && !was) LOG("[hid] %s: button usage %u", d->name, usage);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch) {
        d->st.hat = pad_hid_hat(x, min, max);
    } else {
        int a = pad_hid_axis_of(page, usage); if (a < 0) return;
        float n = pad_hid_norm(x, min, max);
        d->st.axis[a] = d->st.raw ? pad_hid_raw_axis(&d->st, a, n) : n;
    }
    hidpad_read(d);
}
static long hidpad_location(const hid_pad *d) { return [(__bridge NSNumber *)IOHIDDeviceGetProperty(d->hid, CFSTR(kIOHIDLocationIDKey)) longValue]; }
// Whether a pad is behind the entry, so that it counts as one of its model (hidpad_model_count)
static int hidpad_live(const hid_pad *d, uint64_t now) { return d->streams ? pro_live(d->heard, now) : d->heard != 0; }
static int g_hidpad_recount;
static void hidpad_recount(void) {   // pads_changed once this report is handled: it registers callbacks on the device the report came from
    if (g_hidpad_recount) return;
    g_hidpad_recount = 1;
    dispatch_async(dispatch_get_main_queue(), ^{ g_hidpad_recount = 0; hidpad_models_changed(); });
}
static void hidpad_heard(hid_pad *d, uint8_t id) {
    uint64_t now = now_ns(); int was = hidpad_live(d, now);
    if (hidpad_is_pro(d) && id == 0x30) d->streams = 1;
    d->heard = now;
    if (was) return;
    hidpad_recount();   // it counts from now on
    if (hidpad_is_pro(d) || d->proto) return;   // a pad of Nintendo's protocol is read from its reports
    IOHIDDeviceRef hid = d->hid;   // another model counts for good after one report: its reports are not needed any more
    dispatch_async(dispatch_get_main_queue(), ^{
        for (int pass = 0; pass < 2; pass++) for (hid_pad *o = pass ? g_hidpads : g_hidpads_gc; o; o = o->next)
            if (o->hid == hid && o->heard) IOHIDDeviceRegisterInputReportCallback(hid, o->report, sizeof o->report, NULL, NULL);
    });
}
static hid_pad *hidpad_read_by(IOHIDDeviceRef hid) { hid_pad *d = g_hidpads; while (d && d->hid != hid) d = d->next; return d; }   // read here, not given back or gone

// ---- Nintendo's protocol (a Pro Controller clone, pad_hid.h's PAD_NIN_PROTOCOL) ----
// Output reports go out in order on a serial queue (hidpad_start), so a Bluetooth write never holds up the run loop; without the queue (the tests), at once
static dispatch_queue_t g_hidpad_out;
#ifndef HIDPAD_NIN_STEP_NS
#define HIDPAD_NIN_STEP_NS 60000000ull     // between handshake steps, as the console's
#endif
#ifndef HIDPAD_NIN_WAIT_NS
#define HIDPAD_NIN_WAIT_NS 1000000000ull   // for the device info reply, and after the handshake for report 0x30
#endif
enum { HIDPAD_NIN_TRIES = 3, HIDPAD_NIN_STEPS = JC1_HANDSHAKE_SUBS + 1 };   // the handshake's subcommands, then the lights
typedef struct { uint8_t b[JC1_REPORT_LEN]; char name[64]; } hidpad_out;   // by value into the block
static void hidpad_write(hid_pad *d, const uint8_t *out, size_t n) {
    IOHIDDeviceRef hid = d->hid; hidpad_out o = {{0}, {0}};
    if (n > sizeof o.b) n = sizeof o.b;
    memcpy(o.b, out, n); memcpy(o.name, d->name, sizeof o.name);
    if (!g_hidpad_out) { IOReturn e = IOHIDDeviceSetReport(hid, kIOHIDReportTypeOutput, o.b[0], o.b, (CFIndex)n); if (e != kIOReturnSuccess && g_hidpad_log) LOG("[hid] '%s': output report 0x%02x failed (0x%x)", o.name, o.b[0], e); return; }
    int log = g_hidpad_log; CFRetain(hid);
    dispatch_async(g_hidpad_out, ^{
        IOReturn e = IOHIDDeviceSetReport(hid, kIOHIDReportTypeOutput, o.b[0], o.b, (CFIndex)n);
        if (e != kIOReturnSuccess && log) LOG("[hid] '%s': output report 0x%02x failed (0x%x)", o.name, o.b[0], e);
        CFRelease(hid);
    });
}
static void hidpad_send(hid_pad *d, uint8_t sub, const uint8_t *arg, size_t n) { uint8_t out[JC1_REPORT_LEN]; jc1_output_report(out, d->pro_counter++, sub, arg, n); hidpad_write(d, out, sizeof out); }
// Its player on its four lights (jc1_led_pattern: the console's patterns, all four flashing before it joins), sent when that changes: from hidpad_read and the handshake's end
static void hidpad_lights(hid_pad *d, int p) {
    if (d->twin || !pad_hid_nin_lights(d->nin) || d->steps) return;   // the existing twin path does not drive player lights
    uint8_t pat = jc1_led_pattern(p); if (d->lights == pat) return;
    d->lights = pat; hidpad_send(d, JC1_SUB_LEDS, &pat, 1);
}
// Rumble: the game's two motors per player, sent every 15 ms to each pad of the protocol with a player and the rumble report 0x10 in its descriptor (jc1_rumble_encode, HD
// rumble: the left motor on its left actuator, the right on its right); a buzz goes out every tick while it lasts, then three reports of silence, then nothing (as pad_jc1_mac.h)
#define HIDPAD_RUMBLE_TICK_NS 15000000ull
static jc1_rumble_hold g_hidpad_rv[DM_PLAYERS];
static os_unfair_lock g_hidpad_rv_lock = OS_UNFAIR_LOCK_INIT;
static dispatch_source_t g_hidpad_rumble_timer; static int g_hidpad_ticking;
static int hidpad_rumbles(const hid_pad *d) { return d->proto && d->talks && (d->nin & PAD_NIN_RUMBLE); }
// The sink for a player whose pad rumbles (front_mac.m, dm_haptics_apple_set_sink); called on the haptics queue
static void dm_hidpad_rumble(const dm_haptic_state *st) {
    if (!st || st->player < 0 || st->player >= DM_PLAYERS) return;
    os_unfair_lock_lock(&g_hidpad_rv_lock); jc1_rumble_set(&g_hidpad_rv[st->player], st->motor); os_unfair_lock_unlock(&g_hidpad_rv_lock);
}
static void hidpad_rumble_tick(void) {
    dm_vibration m[DM_PLAYERS][2]; uint32_t taken = 0;
    for (hid_pad *d = g_hidpads; d; d = d->next) {
        int p = hidpad_player(d); if (p < 0 || p >= DM_PLAYERS || !hidpad_rumbles(d) || d->steps) continue;   // a handshake in progress: its subcommands first
        if (!(taken >> p & 1)) { taken |= 1u << p; os_unfair_lock_lock(&g_hidpad_rv_lock); jc1_rumble_take(&g_hidpad_rv[p], m[p]); os_unfair_lock_unlock(&g_hidpad_rv_lock); }
        if (jc1_vibration_on(&m[p][0]) || jc1_vibration_on(&m[p][1])) d->rumble_off = 3;
        else if (d->rumble_off > 0) d->rumble_off--;
        else continue;
        uint8_t l[4], r[4], out[JC1_REPORT_LEN] = {0}; jc1_rumble_encode(&m[p][0], l); jc1_rumble_encode(&m[p][1], r); jc1_rumble_report(out, d->pro_counter++, l, r);
        hidpad_write(d, out, sizeof out);   // padded to the output report's 49 bytes, as for a Joy-Con
    }
}
static void hidpad_rumble_sync(void) {   // the timer runs while a pad rumbles
    if (!g_hidpad_rumble_timer) return;
    int any = 0; for (hid_pad *d = g_hidpads; d; d = d->next) any |= hidpad_rumbles(d);
    if (any == g_hidpad_ticking) return;
    g_hidpad_ticking = any;
    if (any) dispatch_resume(g_hidpad_rumble_timer); else dispatch_suspend(g_hidpad_rumble_timer);
}
static void hidpad_nin_step(IOHIDDeviceRef hid) {
    hid_pad *d = hidpad_read_by(hid); if (!d || !d->proto || !d->steps) return;
    uint8_t out[JC1_REPORT_LEN], sub = jc1_handshake_report(HIDPAD_NIN_STEPS - d->steps--, d->pro_counter++, out);
    if (sub) hidpad_write(d, out, sizeof out); else { d->lights = -1; hidpad_lights(d, hidpad_player(d)); }
    if (d->steps) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)HIDPAD_NIN_STEP_NS), dispatch_get_main_queue(), ^{ hidpad_nin_step(hid); }); return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)HIDPAD_NIN_WAIT_NS), dispatch_get_main_queue(), ^{
        hid_pad *h = hidpad_read_by(hid);
        if (h && h->proto && !h->full && !h->steps) LOG("controller '%s' (HID): answers Nintendo's subcommands but sends no report 0x30 after the handshake: read from its report 0x3F (lights set; no motion)", h->name);
    });
}
// The handshake: device info first, which a pad of the protocol answers in report 0x21 (hidpad_nin_reply goes on from there); asked HIDPAD_NIN_TRIES times, a second apart
static void hidpad_nin_ask(hid_pad *d) {
    d->tries++; d->asked = now_ns(); d->steps = 0; int ask = ++d->asks;
    uint8_t out[JC1_REPORT_LEN]; jc1_handshake_report(0, d->pro_counter++, out); hidpad_write(d, out, sizeof out);
    IOHIDDeviceRef hid = d->hid;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)HIDPAD_NIN_WAIT_NS), dispatch_get_main_queue(), ^{
        hid_pad *h = hidpad_read_by(hid); if (!h || !h->proto || h->answered >= ask || h->asks != ask) return;   // answered, or asked again since
        if (h->tries < HIDPAD_NIN_TRIES) { hidpad_nin_ask(h); return; }
        hidpad_lights(h, hidpad_player(h));   // lights can work even when device info or full mode does not
        if (!h->talks) LOG("controller '%s' (HID): its descriptor is Nintendo's but it does not answer the device info subcommand: read from its report 0x3F (player lights sent; no motion or rumble)", h->name);
        else LOG("controller '%s' (HID): no answer to the device info subcommand since it fell back to report 0x3F: read from that report", h->name);
    });
}
static void hidpad_nin_reply(hid_pad *d, const uint8_t *r, size_t n) {
    if (jc1_reply_sub(r, n) == JC1_SUB_DEVICE_INFO) d->answered = d->asks;
    if (jc1_reply_sub(r, n) == JC1_SUB_DEVICE_INFO && !d->steps && (!d->talks || !d->full)) {   // the rest of the handshake (again, after a fall back to report 0x3F)
        if (!d->talks) LOG("controller '%s' (HID): answers Nintendo's subcommands: asking for full mode, its sensor, vibration and stick calibrations", d->name);
        d->talks = 1; d->steps = HIDPAD_NIN_STEPS - 1; d->rumble_off = 0; hidpad_rumble_sync();
        IOHIDDeviceRef hid = d->hid;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)HIDPAD_NIN_STEP_NS), dispatch_get_main_queue(), ^{ hidpad_nin_step(hid); });
        return;
    }
    if (jc1_unit_cal(&d->unit, r, n)) { LOG("[hid] '%s': IMU factory calibration received", d->name); return; }
    int set = pad_pro_spi_reply(&d->pro, r, n);
    for (int k = 0; k < 2; k++) if (set >> k & 1)
        LOG("[hid] '%s': %s stick, %s calibration: rest %d %d, reach %d-%d %d-%d", d->name, k ? "right" : "left", d->pro.cal[k] == 2 ? "user" : "factory",
            d->pro.rest[2 * k], d->pro.rest[2 * k + 1], d->pro.reach[2 * k][0], d->pro.reach[2 * k][1], d->pro.reach[2 * k + 1][0], d->pro.reach[2 * k + 1][1]);
}
// A report 0x30: buttons, sticks and gyro, as a Pro Controller's
static void hidpad_full_report(hid_pad *d, const uint8_t *r, size_t n) {
    if (!pad_pro_decode(&d->pro, r, n, &d->pro_m, d->pro_s)) return;
    if (d->proto) d->pro_m = pad_hid_nin_buttons(&d->st, d->pro_m, r[4]);
    if (!d->pro_logged) { d->pro_logged = 1; LOG("[hid] '%s' at USB location 0x%lx: first report 0x30, sticks at rest %d %d, %d %d", d->name, hidpad_location(d), d->pro.rest[0], d->pro.rest[1], d->pro.rest[2], d->pro.rest[3]); }
    hidpad_read(d);
    if (!g_hidpad_motion) return;
    if (d->proto) {
        jc1_sample s[JC1_IMU_SAMPLES]; int k = jc1_input(&d->unit, r, n, 1, now_ns(), s);
        for (int i = 0; i < k; i++) hidpad_motion(d, s[i].acc, s[i].rot, s[i].ns);
    } else {
        pro_motion_sample s[3]; int k = pro_motion_decode(&d->motion, r, n, now_ns(), s);
        for (int i = 0; i < k; i++) hidpad_motion(d, s[i].acc, s[i].rot, s[i].ns);
    }
}
// Its input reports: each stamps heard (hidpad_listen). A twin Pro Controller's are read: 0x30 its buttons, sticks and gyro, 0x21 the answer to its stick calibration read
// (hidpad_pro_cal_ask). A pad of Nintendo's protocol's: 0x21 its subcommand replies, 0x30 as a Pro Controller's, 0x3F (read from its elements) while it is not in full mode
static void hidpad_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender; (void)report_id;
    hid_pad *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len < 1) return;
    hidpad_heard(d, r[0]);
    size_t n = (size_t)len;
    if (d->proto) {
        if (r[0] == 0x21) { hidpad_nin_reply(d, r, n); return; }
        if (r[0] == PAD_HID_NIN_REPORT && d->full) {   // back in its simple report (a wake resets it): read from it, and the handshake again
            d->full = 0; d->pro_logged = 0; LOG("controller '%s' (HID): back in report 0x3F: read from it, and asked for full mode again", d->name);
            if (!d->steps) { d->tries = 0; hidpad_nin_ask(d); }
            return;
        }
        if (r[0] == 0x30 && !d->full && n >= JC1_REPORT_LEN) {
            d->full = 1;   // d->pro keeps the calibration it answered, else measures itself from this report on (pad_pro_decode)
            LOG("controller '%s' (HID): in full mode: read from its report 0x30 (buttons, sticks by its %s, motion%s)", d->name,
                d->pro.cal[0] || d->pro.cal[1] ? "calibration" : "own reach", d->nin & PAD_NIN_RUMBLE ? ", rumble" : "");
        }
        if (d->full) hidpad_full_report(d, r, n);
        return;
    }
    if (!d->twin || !hidpad_is_pro(d)) return;
    if (r[0] == 0x21) {
        int set = pad_pro_spi_reply(&d->pro, r, n);
        for (int k = 0; k < 2; k++) if (set >> k & 1)
            LOG("[hid] Pro Controller at USB location 0x%lx: %s stick, %s calibration: rest %d %d, reach %d-%d %d-%d", hidpad_location(d), k ? "right" : "left", d->pro.cal[k] == 2 ? "user" : "factory",
                d->pro.rest[2 * k], d->pro.rest[2 * k + 1], d->pro.reach[2 * k][0], d->pro.reach[2 * k][1], d->pro.reach[2 * k + 1][0], d->pro.reach[2 * k + 1][1]);
        return;
    }
    hidpad_full_report(d, r, n);
}
static hid_pad *hidpad_unlink(hid_pad **list, IOHIDDeviceRef hid) {
    while (*list && (*list)->hid != hid) list = &(*list)->next;
    hid_pad *d = *list; if (d) *list = d->next; return d;
}
// Whether GameController lists a controller of this name with the profile front_mac.m reads (vendorName is the HID product string). A second pad of the same model counts as listed with the first
static int hidpad_gc_listed(const char *name) { for (GCController *c in GCController.controllers) if (c.extendedGamepad && c.vendorName && !strcmp(c.vendorName.UTF8String, name)) return 1; return 0; }
// The vendors whose pads GameController supports itself (Xbox, PlayStation, Switch controllers). The name match cannot vouch for them: a DualShock 4's HID product string is
// "Wireless Controller", and combined Joy-Cons are one GCController for two HID devices, so each would also be read as HID after the wait and take a player of its own
static int hidpad_gc_native(int vid) { return vid == 0x045e || vid == 0x054c || vid == 0x057e; }
static int hidpad_twin_able(const hid_pad *d) { return hidpad_is_pro(d) || !hidpad_gc_native(d->vid); }
// Its reading stops: no callback is left with it
static void hidpad_stop(hid_pad *d) {
    IOHIDDeviceRegisterInputValueCallback(d->hid, NULL, NULL);
    IOHIDDeviceRegisterInputReportCallback(d->hid, d->report, sizeof d->report, NULL, NULL);
}
// Its reports stamp heard while it may still be counted as one of its model: always for a Pro Controller (its 0x30 says it is there), another model until its first
static void hidpad_listen(hid_pad *d) {
    if (!g_twins_off && hidpad_twin_able(d) && (hidpad_is_pro(d) || !d->heard)) IOHIDDeviceRegisterInputReportCallback(d->hid, d->report, sizeof d->report, hidpad_report, d);
}
static void hidpad_drop(hid_pad *d) { hidpad_stop(d); CFRelease(d->hid); free(d); }
static void hidpad_keep_gc(hid_pad *d) {
    hidpad_stop(d); d->twin = d->twin_want = 0; d->proto = d->talks = d->full = d->steps = 0; d->next = g_hidpads_gc; g_hidpads_gc = d; hidpad_listen(d);
    hidpad_rumble_sync();
}
static void hidpad_use(hid_pad *d) {
    d->next = g_hidpads; g_hidpads = d;
    IOHIDDeviceRegisterInputValueCallback(d->hid, hidpad_value, d);
    d->proto = pad_hid_nin_protocol(d->nin); d->lights = -1;
    jc1_unit_init(&d->unit);
    if (d->proto) {   // its reports, from now on: the device info reply first
        LOG("controller '%s' (HID, %04x:%04x): Nintendo's report descriptor (input 0x3F, 0x21, 0x30; output 0x01%s): asking for its device info", d->name, d->vid, d->pid,
            d->nin & PAD_NIN_RUMBLE ? ", 0x10" : "");
        IOHIDDeviceRegisterInputReportCallback(d->hid, d->report, sizeof d->report, hidpad_report, d);
    }
    hidpad_attach(d);
    if (d->proto) hidpad_nin_ask(d);
    else hidpad_lights(d, hidpad_player(d));
    // HID has no standard button numbering: the default map is 8BitDo's, so another vendor's pad may have its buttons in other places
    if (d->nin & PAD_NIN_SIMPLE) {
        if (!d->proto) LOG("controller '%s' (HID, %04x:%04x): buttons in Nintendo's report 0x3F: read with its numbering", d->name, d->vid, d->pid);
    } else if (d->vid != PAD_HID_DEFAULT_VENDOR && !getenv("DM_HID_MAP"))
        LOG("controller '%s' (HID, %04x:%04x): buttons use 8BitDo's numbering; if they are in the wrong places, DM_HID_LOG=1 logs each button's number for a DM_HID_MAP", d->name, d->vid, d->pid);
}
// The device's I/O Registry entry: IOHIDManager hands one device over once per matching dictionary it meets (a Pro Controller meets both the GamePad usage
// and its vendor / product one), each time as a new IOHIDDeviceRef, so a pad is known by its entry, not by its reference
static uint64_t hidpad_entry(IOHIDDeviceRef hid) { uint64_t id = 0; IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(hid), &id); return id; }
static int hidpad_known(const hid_pad *list, IOHIDDeviceRef hid, uint64_t entry) {
    for (const hid_pad *d = list; d; d = d->next) if (d->hid == hid || (entry && hidpad_entry(d->hid) == entry)) return 1;
    return 0;
}
static void hidpad_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    uint64_t entry = hidpad_entry(hid);
    if (hidpad_known(g_hidpads, hid, entry) || hidpad_known(g_hidpads_waiting, hid, entry) || hidpad_known(g_hidpads_gc, hid, entry)) return;
    hid_pad *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] HID gamepad: cannot allocate device state"); return; }
    NSString *name = (__bridge NSString *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductKey));
    snprintf(d->name, sizeof d->name, "%s", name.UTF8String ?: "HID gamepad");
    d->vid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey)) intValue];
    d->pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey)) intValue];
    NSArray *els = CFBridgingRelease(IOHIDDeviceCopyMatchingElements(hid, NULL, kIOHIDOptionsTypeNone));
    uint8_t has[PH_AXES] = {0};
    for (id o in els) {
        IOHIDElementRef e = (__bridge IOHIDElementRef)o; uint32_t page = IOHIDElementGetUsagePage(e);
        int a = pad_hid_axis_of(page, IOHIDElementGetUsage(e)); if (a >= 0) has[a] = 1;
        d->nin |= pad_hid_nin_bit(IOHIDElementGetType(e) == kIOHIDElementTypeOutput, IOHIDElementGetReportID(e), page);
    }
    pad_hid_init(&d->st, getenv("DM_HID_MAP"), d->nin & PAD_NIN_SIMPLE); memcpy(d->st.has, has, sizeof has);
    d->hid = (IOHIDDeviceRef)CFRetain(hid);
    d->gc_hint = [GCController supportsHIDDevice:hid];   // once per device: the call is not cheap
    if (!d->gc_hint) { hidpad_use(d); return; }
    NSNumber *virt = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVirtualHIDevice));
    if ((!virt.intValue && hidpad_gc_native(d->vid)) || hidpad_gc_listed(d->name)) { hidpad_keep_gc(d); hidpad_models_changed(); return; }   // front_mac.m reads it through its GCController
    d->next = g_hidpads_waiting; g_hidpads_waiting = d;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HIDPAD_GC_WAIT * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        hid_pad *w = hidpad_unlink(&g_hidpads_waiting, hid); if (!w) return;   // removed meanwhile
        if (hidpad_gc_listed(w->name)) { hidpad_keep_gc(w); hidpad_models_changed(); return; }
        LOG("controller '%s' (HID, %04x:%04x): GameController says it lists it but has not after %g s, so it is read as HID", w->name, w->vid, w->pid, HIDPAD_GC_WAIT);
        hidpad_use(w);
    });
}
static void hidpad_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    hid_pad *d = hidpad_unlink(&g_hidpads, hid); int counted = d && d->twin;
    if (d) hidpad_detach(d); else d = hidpad_unlink(&g_hidpads_waiting, hid);
    if (!d && (d = hidpad_unlink(&g_hidpads_gc, hid))) counted = 1;
    if (d) { hidpad_drop(d); hidpad_rumble_sync(); }
    if (counted) hidpad_models_changed();   // one pad fewer of its model
}
// A GCController connected (front_mac.m, before its pads_changed): a HID pad of its name that GameController had only promised is read through it from now on.
// ponytail: matched by name, so two pads of one model that both fell back to HID are both given back when GameController lists one; a per-device link would need an API
// GameController does not give. Twins stay: hidpad_twins_eval decides for them
static void hidpad_gc_connected(GCController *c) {
    if (!c.extendedGamepad) return;   // front_mac.m cannot read this controller yet
    const char *name = c.vendorName.UTF8String; if (!name) return;
    for (hid_pad **link = &g_hidpads; *link;) {
        hid_pad *d = *link; if (!d->gc_hint || d->twin || strcmp(d->name, name)) { link = &d->next; continue; }
        *link = d->next; LOG("controller '%s' (HID) is now a GameController controller: read through it", d->name); hidpad_detach(d); hidpad_keep_gc(d);
    }
}

// Identical pads (twins), from front_mac.m's pads_changed: hidpad_twins_eval, then hidpad_twins_release before GameController's controllers take their slots (leaving out
// those hidpad_gc_unread names), then hidpad_twins_take. gone: the controller just removed, not counted
static int hidpad_same_model(const hid_pad *a, const hid_pad *b) { return a->vid == b->vid && a->pid == b->pid; }
static int hidpad_gc_is_model(GCController *c, const hid_pad *d) {
    if (!c.extendedGamepad) return 0;
    if (hidpad_is_pro(d)) return [c.productCategory isEqualToString:@"Switch Pro Controller"];
    return !hidpad_gc_native(d->vid) && c.vendorName && !strcmp(c.vendorName.UTF8String, d->name);
}
static int hidpad_gc_count(const hid_pad *d, GCController *gone) { int n = 0; for (GCController *c in GCController.controllers) if (c != gone && hidpad_gc_is_model(c, d)) n++; return n; }
// IOKit's pads of its model, the entries with a pad behind them (hidpad_live); read: how many of them are twins already; all: every entry of the model
static int hidpad_model_count(const hid_pad *d, uint64_t now, int *read, int *all) {
    int n = 0; *read = 0; if (all) *all = 0;
    for (int pass = 0; pass < 2; pass++) for (hid_pad *o = pass ? g_hidpads : g_hidpads_gc; o; o = o->next) if ((!pass || o->twin) && hidpad_same_model(o, d)) {
        if (all) (*all)++;
        if (hidpad_live(o, now)) { n++; *read += pass; }
    }
    return n;
}
static const char *hidpad_transport(const hid_pad *d) { NSString *t = (__bridge NSString *)IOHIDDeviceGetProperty(d->hid, CFSTR(kIOHIDTransportKey)); return t.UTF8String ?: "no transport"; }
// One line when an entry of a model with others starts or stops counting, naming the entry, so a report shows which entries a pad has
static void hidpad_show(hid_pad *d, int live, int all, uint64_t now) {
    if (all < 2 || d->shown == 1 + live || (live && !d->shown)) { if (live) d->shown = 2; return; }
    d->shown = 1 + live;
    char why[64];
    if (live) snprintf(why, sizeof why, "sends reports: counted");
    else if (!d->heard) snprintf(why, sizeof why, "has sent no input report: not counted");
    else snprintf(why, sizeof why, "no report 0x30 for %.1f s: not counted", (double)(now - d->heard) / 1e9);
    LOG("[hid] '%s' (%04x:%04x) at USB location 0x%lx, %s, registry entry 0x%llx: %s as a pad of its model", d->name, d->vid, d->pid, hidpad_location(d), hidpad_transport(d),
        (unsigned long long)hidpad_entry(d->hid), why);
}
static void hidpad_twins_wait(void) {   // a pad connected a moment ago may simply not be listed yet
    if (g_twins_waiting) return;
    g_twins_waiting = 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HIDPAD_TWIN_WAIT * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ g_twins_waiting = 0; g_twins_due = 1; hidpad_models_changed(); g_twins_due = 0; });
}
static void hidpad_twins_eval(GCController *gone) {
    int wait = 0; uint64_t now = now_ns();
    for (int pass = 0; pass < 2; pass++) for (hid_pad *d = pass ? g_hidpads : g_hidpads_gc; d; d = d->next) {
        if (pass && !d->twin) continue;
        if (g_twins_off || !hidpad_twin_able(d)) { d->twin_want = 0; continue; }
        int read, all, n = hidpad_model_count(d, now, &read, &all), live = hidpad_live(d, now);
        hidpad_show(d, live, all, now);
        int want = !live ? 0 : pad_dupes_want(n, hidpad_gc_count(d, gone), read > 0, g_twins_due);
        if (want < 0) { want = 0; wait = 1; }
        d->twin_want = want;
    }
    if (wait) hidpad_twins_wait();
}
// Whether front_mac.m leaves a GameController controller unread, the pads of its model being read here
static int hidpad_gc_unread(GCController *c) {
    for (int pass = 0; pass < 2; pass++) for (hid_pad *d = pass ? g_hidpads : g_hidpads_gc; d; d = d->next)
        if (d->twin_want && (!pass || d->twin) && hidpad_gc_is_model(c, d)) return 1;
    return 0;
}
// The twins of a model GameController lists them all of again go back to it
static void hidpad_twins_release(void) {
    for (hid_pad **link = &g_hidpads; *link;) {
        hid_pad *d = *link; if (!d->twin || d->twin_want) { link = &d->next; continue; }
        *link = d->next; LOG("[hid] '%s': GameController lists every pad of this model again: read through it", d->name);
        hidpad_detach(d); hidpad_keep_gc(d);
    }
}
// A Pro Controller's stick calibration, from SPI flash (pad_pro.h): asked when it becomes a twin, and again a second later while it has not answered, tries times in all
static void hidpad_pro_cal_ask(hid_pad *d, int tries) {
    uint8_t out[16]; size_t n = pad_pro_spi_read(out, d->pro_counter++, PAD_PRO_SPI_FACTORY, PAD_PRO_SPI_FACTORY_LEN);
    IOReturn e = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, out[0], out, (CFIndex)n);
    n = pad_pro_spi_read(out, d->pro_counter++, PAD_PRO_SPI_USER, PAD_PRO_SPI_USER_LEN);
    if (e == kIOReturnSuccess) e = IOHIDDeviceSetReport(d->hid, kIOHIDReportTypeOutput, out[0], out, (CFIndex)n);
    if (e != kIOReturnSuccess) { LOG("[hid] Pro Controller '%s': cannot ask its stick calibration (IOReturn 0x%x): its sticks measure themselves", d->name, e); return; }
    IOHIDDeviceRef hid = d->hid;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        hid_pad *h = g_hidpads; while (h && h->hid != hid) h = h->next;
        if (!h || !h->twin || h->pro.cal[0] || h->pro.cal[1]) return;   // gone, given back, or answered
        if (tries > 1) hidpad_pro_cal_ask(h, tries - 1);
        else LOG("[hid] Pro Controller '%s': no stick calibration answer: its sticks measure themselves", h->name);
    });
}
// The pads of a model GameController lists fewer of are read here, by USB location, so the same ports give the same players at the next start
static void hidpad_twins_take(GCController *gone) {
    for (;;) {
        hid_pad **at = NULL; long loc = 0;
        for (hid_pad **link = &g_hidpads_gc; *link; link = &(*link)->next) if ((*link)->twin_want) {
            long l = hidpad_location(*link);
            if (!at || l < loc) { at = link; loc = l; }
        }
        if (!at) return;
        hid_pad *d = *at; *at = d->next; int read, n = hidpad_model_count(d, now_ns(), &read, NULL);
        int pro = hidpad_is_pro(d);
        LOG("[hid] '%s' (%04x:%04x) at USB location 0x%lx, %s, registry entry 0x%llx: GameController lists %d of %d pads of this model: read here, from its %s", d->name, d->vid, d->pid, loc,
            hidpad_transport(d), (unsigned long long)hidpad_entry(d->hid), hidpad_gc_count(d, gone), n + 1, pro ? "report 0x30" : "HID elements");
        d->twin = 1; d->next = g_hidpads; g_hidpads = d;
        if (pro) {
            d->pro = (pad_pro){0}; d->pro_m = 0; memset(d->pro_s, 0, sizeof d->pro_s); d->motion = (pro_motion){0}; d->pro_logged = 0;
            IOHIDDeviceRegisterInputReportCallback(d->hid, d->report, sizeof d->report, hidpad_report, d);
        } else {
            uint8_t has[PH_AXES]; memcpy(has, d->st.has, sizeof has);   // the axes hidpad_found saw
            pad_hid_init(&d->st, getenv("DM_HID_MAP"), d->nin & PAD_NIN_SIMPLE); memcpy(d->st.has, has, sizeof has);   // nothing held from an earlier turn
            IOHIDDeviceRegisterInputValueCallback(d->hid, hidpad_value, d);
        }
        hidpad_attach(d);
        if (pro) hidpad_pro_cal_ask(d, 3);
    }
}
static void hidpad_start(void) {
    const char *e = getenv("DM_HID_PADS"); if (e && !atoi(e)) return;
    e = getenv("DM_HID_LOG"); g_hidpad_log = e && atoi(e);
    e = getenv("DM_PAD_IDENTICAL"); g_twins_off = e && !atoi(e);
    e = getenv("DM_MOTION"); const char *h = getenv("DM_MOTION_HID"); g_hidpad_motion = !(e && !atoi(e)) && !(h && !atoi(h));   // a twin's gyro is HID motion, as motion_pro_mac.h's
    g_hidpad_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_hidpad_manager) return;
    g_hidpad_out = dispatch_queue_create("dm.hidpad.out", DISPATCH_QUEUE_SERIAL);   // Nintendo's protocol's output reports, in order
    g_hidpad_rumble_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());   // resumed while a pad rumbles (hidpad_rumble_sync)
    dispatch_source_set_timer(g_hidpad_rumble_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)HIDPAD_RUMBLE_TICK_NS), HIDPAD_RUMBLE_TICK_NS, 1000000);
    dispatch_source_set_event_handler(g_hidpad_rumble_timer, ^{ hidpad_rumble_tick(); });
    IOHIDManagerSetDeviceMatchingMultiple(g_hidpad_manager, (__bridge CFArrayRef)@[
        @{@kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad)},
        @{@kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_Joystick)},
        @{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2009}]);   // the Pro Controller, whatever usage it declares, so that identical ones are counted
    IOHIDManagerRegisterDeviceMatchingCallback(g_hidpad_manager, hidpad_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_hidpad_manager, hidpad_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_hidpad_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_hidpad_manager, kIOHIDOptionsTypeNone);   // nonexclusive: GameController and other apps keep their devices; output reports: a twin Pro Controller's calibration read, Nintendo's protocol's subcommands and rumble
    if (r != kIOReturnSuccess) {
        LOG("[RED] HID gamepads: cannot open HID (0x%x); only GameController controllers are read", r);
        IOHIDManagerUnscheduleFromRunLoop(g_hidpad_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_hidpad_manager); g_hidpad_manager = NULL;
    }
}
