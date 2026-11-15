// Mac-only transport for original Joy-Cons (Switch 1) over Bluetooth Classic HID, beside pad_hid_mac.h. These controllers are paired in System Settings -> Bluetooth; macOS
// keeps the link and its HID driver also lists them as GameController controllers (a combined L + R as one, "Joy-Con (L/R)", a lone one with no extended gamepad profile). That
// driver gives them no GCMotion and zeroes the IMU in report 0x30, and its controllers carry no HID identity, so they cannot be told apart or tied to a sensor. So the host reads
// each Joy-Con wholly from its own report 0x30 (pad_jc1.h): buttons, stick, six-axis sensor; it drives the rumble (output report 0x10) and the player lights itself, and
// front_mac.m leaves GameController's Joy-Con controllers unread (jc1_gc_unread), keeping only their Home and Capture away from macOS.
// The handshake below is what the console does at connection (device info, standard full mode, the sensor and the vibration on, the IMU and stick calibrations, the lights), and
// macOS does not undo it (a device that wakes gets it again from the watchdog).
// Pads: a left and a right Joy-Con make one pad (jc1_pair_pick), a Joy-Con alone is a pad held sideways; holding SL + SR on one of a pair for 3 s makes it a pad of its own, holding
// L on a left one alone and R on a right one alone together for 3 s pairs them (jc1_gesture). A pad is a player from its first button press, as a raw HID pad (front_mac.m jc1pad_read).
// Each Joy-Con is a device of its own to the npad layer (dm_joy_attach, dm_joy_set, dm_motion_set_joy), as the Switch 2 transport's Joy-Con 2s are, so the game decides whether a
// pair is one dual npad or two single ones (hle_hid.c); the pad's merged state (dm_pad_set_player) and merged six-axis stream (dm_motion_set_hid) serve a game that supports no
// Joy-Con style. All callbacks and timers run on the main run loop, alongside front_mac.m's player slots; output reports go out on a serial queue of their own, so a Bluetooth write
// never holds up the run loop.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <os/lock.h>
#include "dm_input.h"
#include "pad_jc1.h"
#include <stdlib.h>
#include <stdio.h>

enum { JC1_HANDSHAKE_STEPS = 8 };
static const uint64_t JC1_STEP_NS = 60000000, JC1_WAIT_NS = 1000000000, JC1_WAIT_MAX_NS = 8000000000;
#define JC1_RUMBLE_TICK_NS 15000000ull
typedef struct jc1_pad jc1_pad;
typedef struct jc1_dev {
    IOHIDDeviceRef hid; int pid, right; uint64_t id;
    uint8_t report[512], counter;
    uint8_t last[JC1_REPORT_LEN]; int have;   // its last report 0x30 (have: one came)
    uint64_t buttons; int32_t st[2];          // its own buttons (NpadButtons, rails included) and stick, from that report
    jc1_unit unit; pad_pro stick;             // the sensor's calibration, rest and stream; the stick's calibration
    jc1_pad *pad;
    int steps;                      // handshake subcommands still to send (jc1_step)
    uint64_t asked, wait;           // when the last handshake began; the watchdog's wait (1 s, doubling to 8 s while the sensor stays off)
    uint64_t logged;
    int said, quiet_said, lights, rumble_off;   // lights: the pattern last sent (-1 none); rumble_off: silent reports still to send after a buzz
    jc1_hold hold;                  // the gesture it holds now (jc1_gesture)
    struct jc1_dev *next;
} jc1_dev;
struct jc1_pad {
    jc1_dev *side[2]; char name[32];
    int player; uint32_t told;      // the player and the sides last given to the npad layer (jc1_pad_sync)
    struct jc1_pad *next;
};

static IOHIDManagerRef g_jc1_manager;
static dispatch_source_t g_jc1_timer, g_jc1_rumble_timer;
static dispatch_queue_t g_jc1_out;   // output reports, in order
static jc1_dev *g_jc1_devs;
static jc1_pad *g_jc1_pads;
static uint64_t g_jc1_next_id;
static int g_jc1_prefer = 1, g_jc1_log, g_jc1_joy = 1, g_jc1_active = 1;   // g_jc1_prefer: the side of a pair that feeds the merged stream (DM_JC1_SIDE); g_jc1_joy: not DM_PAD_MERGE
static jc1_rumble_hold g_jc1_rv[DM_PLAYERS];   // each player's vibration, from the haptics queue (dm_jc1_rumble)
static os_unfair_lock g_jc1_rv_lock = OS_UNFAIR_LOCK_INIT;
// front_mac.m's side: a pad's player (-1 without one), its state changed (it may join at a first press), a new pad, a pad gone (already off the list), whether the game takes no
// controller input now, and that the Joy-Cons read here changed (GameController's controllers of them are left unread or given back)
static int jc1pad_player(jc1_pad *p);
static void jc1pad_read(jc1_pad *p);
static void jc1pad_attach(jc1_pad *p);
static void jc1pad_detach(jc1_pad *p);
static int jc1pad_blocked(void);
static void jc1_models_changed(void);

static const char *jc1_name(const jc1_dev *d) { return d->right ? "right" : "left"; }
static jc1_dev *jc1_by_id(uint64_t id) { for (jc1_dev *d = g_jc1_devs; d; d = d->next) if (d->id == id) return d; return NULL; }
typedef struct { uint8_t b[JC1_REPORT_LEN]; } jc1_buf;
static void jc1_write(jc1_dev *d, const uint8_t *out, size_t n, uint8_t sub) {
    jc1_buf buf; memcpy(buf.b, out, n < sizeof buf.b ? n : sizeof buf.b);
    IOHIDDeviceRef hid = (IOHIDDeviceRef)CFRetain(d->hid); int pid = d->pid, log = g_jc1_log;
    dispatch_async(g_jc1_out, ^{
        IOReturn r = IOHIDDeviceSetReport(hid, kIOHIDReportTypeOutput, buf.b[0], buf.b, (CFIndex)n);
        if (r != kIOReturnSuccess && log) LOG("[jc1] output report 0x%02x (subcommand 0x%02x) on pid 0x%04x failed: 0x%x", buf.b[0], sub, pid, r);
        CFRelease(hid);
    });
}
static void jc1_send(jc1_dev *d, uint8_t sub, const uint8_t *arg, size_t n) {
    uint8_t out[JC1_REPORT_LEN];
    jc1_output_report(out, d->counter++, sub, arg, n);
    jc1_write(d, out, sizeof out, sub);
}
static void jc1_lights(jc1_dev *d) {
    int pl = d->pad ? jc1pad_player(d->pad) : -1; uint8_t pat = jc1_led_pattern(pl);
    if (d->lights == pat) return;
    d->lights = pat; jc1_send(d, JC1_SUB_LEDS, &pat, 1);
}
// The order matters (report mode before the sensor, and the controller drops a subcommand that comes too soon): device info, standard full mode, EnableIMU, the vibration, the
// factory IMU calibration read, the stick calibrations (factory, then the user's), then the player lights, 60 ms apart as the console's. Each step is its own main-queue block, so
// the run loop (the front end's input among it) never waits on the controller; a device removed in between is no longer found by its id.
static void jc1_step(uint64_t id) {
    jc1_dev *d = jc1_by_id(id);
    if (!d || !d->steps) return;
    const uint8_t mode = JC1_INPUT_MODE_FULL, on = 1;
    const uint8_t imu[5] = {(uint8_t)(JC1_SPI_IMU_CAL & 0xff), (uint8_t)(JC1_SPI_IMU_CAL >> 8 & 0xff), 0, 0, JC1_IMU_CAL_LEN};
    const uint8_t fac[5] = {(uint8_t)(PAD_PRO_SPI_FACTORY & 0xff), (uint8_t)(PAD_PRO_SPI_FACTORY >> 8 & 0xff), 0, 0, PAD_PRO_SPI_FACTORY_LEN};
    const uint8_t usr[5] = {(uint8_t)(PAD_PRO_SPI_USER & 0xff), (uint8_t)(PAD_PRO_SPI_USER >> 8 & 0xff), 0, 0, PAD_PRO_SPI_USER_LEN};
    switch (JC1_HANDSHAKE_STEPS - d->steps--) {
    case 0: jc1_send(d, JC1_SUB_DEVICE_INFO, NULL, 0); break;
    case 1: jc1_send(d, JC1_SUB_INPUT_MODE, &mode, 1); break;
    case 2: jc1_send(d, JC1_SUB_ENABLE_IMU, &on, 1); break;
    case 3: jc1_send(d, JC1_SUB_VIBRATION, &on, 1); break;
    case 4: jc1_send(d, JC1_SUB_SPI_READ, imu, sizeof imu); break;
    case 5: jc1_send(d, JC1_SUB_SPI_READ, fac, sizeof fac); break;
    case 6: jc1_send(d, JC1_SUB_SPI_READ, usr, sizeof usr); break;
    default: d->lights = -1; jc1_lights(d); break;
    }
    if (d->steps) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)JC1_STEP_NS), dispatch_get_main_queue(), ^{ jc1_step(id); });
}
static void jc1_handshake(jc1_dev *d) {
    if (d->steps) return;   // one is under way
    d->steps = JC1_HANDSHAKE_STEPS; d->asked = now_ns();
    if (g_jc1_log) LOG("[jc1] %s Joy-Con (pid 0x%04x): standard full mode, the 6-axis sensor, the vibration and the calibrations asked for", jc1_name(d), d->pid);
    jc1_step(d->id);
}

// GameController's controllers of these Joy-Cons, by product category ("Nintendo Switch Joy-Con (L)", "(R)", "(L/R)", as pad_apple.h): the sides each holds (bit 0 left, bit 1 right)
static int jc1_gc_sides(GCController *c) {
    NSString *k = c.productCategory;
    if (![k hasPrefix:@"Nintendo Switch Joy-Con ("]) return 0;
    return [k hasSuffix:@"(L)"] ? 1 : [k hasSuffix:@"(R)"] ? 2 : 3;
}
// Whether front_mac.m leaves a GameController controller unread: a Joy-Con of its sides is read here (one that has sent a report: an entry with nothing behind it takes nothing
// from GameController)
static int jc1_gc_unread(GCController *c) {
    int sides = g_jc1_manager ? jc1_gc_sides(c) : 0;
    for (jc1_dev *d = g_jc1_devs; sides && d; d = d->next) if (d->have && (sides >> d->right & 1)) return 1;
    return 0;
}

// ---- Pads ----
static uint32_t jc1_pad_sides(const jc1_pad *p) { return (p->side[0] ? 1u : 0) | (p->side[1] ? 2u : 0); }
// The pad's state as one whole controller (pad_jc1.h jc1_pad_input), from each Joy-Con's last report. 0: none has reported yet
static int jc1_pad_state(const jc1_pad *p, uint64_t *m, int32_t st[4]) {
    int have[2]; uint64_t b[2]; int32_t s[2][2];
    for (int k = 0; k < 2; k++) { const jc1_dev *d = p->side[k]; have[k] = d && d->have; b[k] = have[k] ? d->buttons : 0; s[k][0] = have[k] ? d->st[0] : 0; s[k][1] = have[k] ? d->st[1] : 0; }
    return jc1_pad_input(have, b, s, m, st);
}
static int jc1_pad_home(const jc1_pad *p) { for (int k = 0; k < 2; k++) if (p->side[k] && p->side[k]->have && jc1_home(p->side[k]->last)) return 1; return 0; }
static void jc1_pad_ab(const jc1_pad *p, int *a, int *b) {   // A and B are on the right Joy-Con (a left one alone has none: the prompts also take the keyboard)
    const jc1_dev *d = p->side[1]; if (!d || !d->have) return;
    *a |= (d->buttons & DM_BTN_A) != 0; *b |= (d->buttons & DM_BTN_B) != 0;
}
static void jc1_joy_release(int pl, int side) { if (g_jc1_joy && pl >= 0) { dm_joy_set(pl, side, 0, 0, 0); dm_motion_clear_joy(pl, side); } }
// The npad layer and the lights follow the pad's player and its Joy-Cons: after a join at a first press, a pair made or split, a Joy-Con come or gone
static void jc1_pad_sync(jc1_pad *p) {
    int pl = jc1pad_player(p); uint32_t sides = jc1_pad_sides(p);
    if (pl != p->player || sides != p->told) {
        if (g_jc1_joy && p->player >= 0 && p->player != pl) dm_joy_attach(p->player, 0);
        if (g_jc1_joy && pl >= 0) dm_joy_attach(pl, sides);
        for (int k = 0; k < 2; k++) if (pl >= 0 && !(sides >> k & 1) && p->told >> k & 1) jc1_joy_release(pl, k);   // a Joy-Con that left: nothing of it stays pressed
        p->player = pl; p->told = sides;
    }
    for (int k = 0; k < 2; k++) if (p->side[k] && !p->side[k]->steps) jc1_lights(p->side[k]);
}
static void jc1_pad_name(jc1_pad *p) { uint32_t s = jc1_pad_sides(p); snprintf(p->name, sizeof p->name, "Joy-Con (%s)", s == 3 ? "L/R" : s == 2 ? "R" : "L"); }
// The rumble tick runs while there is a pad, not for the whole game
static int g_jc1_ticking;
static void jc1_rumble_timer_sync(void) {
    if (!g_jc1_rumble_timer || g_jc1_ticking == (g_jc1_pads != NULL)) return;
    g_jc1_ticking = g_jc1_pads != NULL;
    if (g_jc1_ticking) dispatch_resume(g_jc1_rumble_timer); else dispatch_suspend(g_jc1_rumble_timer);
}
static jc1_pad *jc1_pad_new(void) { jc1_pad *p = calloc(1, sizeof *p); if (p) { p->player = -1; p->next = g_jc1_pads; g_jc1_pads = p; jc1_rumble_timer_sync(); } return p; }
static void jc1_pad_unlink(jc1_pad *p) { jc1_pad **link = &g_jc1_pads; while (*link && *link != p) link = &(*link)->next; if (*link) *link = p->next; jc1_rumble_timer_sync(); }
// A Joy-Con leaves its pad (removed, split or moved): the pad keeps the other one, or it goes when it is empty
static void jc1_leave(jc1_dev *d) {
    jc1_pad *p = d->pad; if (!p) return;
    p->side[d->right] = NULL; d->pad = NULL;
    if (jc1_pad_sides(p)) { jc1_pad_name(p); jc1_pad_sync(p); jc1pad_read(p); return; }
    if (p->player >= 0) { if (g_jc1_joy) { jc1_joy_release(p->player, d->right); dm_joy_attach(p->player, 0); } dm_motion_clear_hid(p->player); }
    jc1_pad_unlink(p); jc1pad_detach(p); free(p);
}
static void jc1_enter(jc1_dev *d, jc1_pad *p) { p->side[d->right] = d; d->pad = p; jc1_pad_name(p); }
// SL + SR held on a Joy-Con of a pair: it plays alone. L on a left one alone and R on a right one alone, held together: they pair (pad_jc1.h jc1_gesture)
static void jc1_gesture_check(jc1_dev *d, uint64_t now) {
    jc1_pad *p = d->pad; int paired = jc1_pad_sides(p) == 3;
    if (!jc1_hold_update(&d->hold, jc1_gesture(d->buttons, d->right, paired), now) || d->hold.done) return;
    if (d->hold.kind == JC1_GESTURE_SPLIT) {
        d->hold.done = 1;
        int was = jc1pad_player(p); jc1_pad *n = jc1_pad_new(); if (!n) return;
        jc1_leave(d); jc1_enter(d, n); jc1pad_attach(n); jc1_pad_sync(n);   // the held SL + SR are its first press: attach reads it at once
        LOG("[jc1] %s Joy-Con plays alone now (SL + SR held)%s; the other one stays player %d", jc1_name(d), jc1pad_player(n) < 0 ? ", once a button is pressed" : "", was + 1);
        return;
    }
    for (jc1_dev *o = g_jc1_devs; o; o = o->next) {   // the partner: the other side, alone, holding its own L or R as long
        if (o == d || o->right == d->right || !o->pad || jc1_pad_sides(o->pad) == 3 || o->hold.kind != JC1_GESTURE_JOIN || o->hold.done || now - o->hold.since < JC1_GESTURE_NS) continue;
        d->hold.done = o->hold.done = 1;
        int pd = jc1pad_player(d->pad), po = jc1pad_player(o->pad);   // the one with the later player (or none) moves into the other's pad
        jc1_dev *mover = (pd < 0 ? DM_PLAYERS : pd) > (po < 0 ? DM_PLAYERS : po) ? d : o, *stay = mover == d ? o : d;
        jc1_leave(mover); jc1_enter(mover, stay->pad); jc1_pad_sync(stay->pad); jc1pad_read(stay->pad);
        LOG("[jc1] left and right Joy-Con play as one pair now (L + R held): player %d", jc1pad_player(stay->pad) + 1);
        return;
    }
}

// ---- Rumble: the game's two motors per player, sent every 15 ms to each Joy-Con of a pad that has a player (pad_jc1.h, HD rumble) ----
// The sink for a player whose pad is these Joy-Cons (dm_haptics_apple_set_sink); called on the haptics queue
static void dm_jc1_rumble(const dm_haptic_state *st) {
    if (!st || st->player < 0 || st->player >= DM_PLAYERS) return;
    os_unfair_lock_lock(&g_jc1_rv_lock); jc1_rumble_set(&g_jc1_rv[st->player], st->motor); os_unfair_lock_unlock(&g_jc1_rv_lock);
}
// A buzz goes out every tick while it lasts (the console's stream of reports does the same), then three reports of silence, then nothing
static void jc1_rumble_tick(void) {
    dm_vibration m[DM_PLAYERS][2]; uint32_t taken = 0;
    for (jc1_pad *p = g_jc1_pads; p; p = p->next) {
        int pl = jc1pad_player(p); if (pl < 0 || pl >= DM_PLAYERS) continue;
        if (!(taken >> pl & 1)) { taken |= 1u << pl; os_unfair_lock_lock(&g_jc1_rv_lock); jc1_rumble_take(&g_jc1_rv[pl], m[pl]); os_unfair_lock_unlock(&g_jc1_rv_lock); }
        int paired = jc1_pad_sides(p) == 3;
        for (int k = 0; k < 2; k++) {
            jc1_dev *d = p->side[k]; if (!d || d->steps) continue;   // a handshake in progress: its subcommands first
            dm_vibration v = jc1_rumble_pick(m[pl], k, paired);
            if (jc1_vibration_on(&v)) d->rumble_off = 3;
            else if (d->rumble_off > 0) d->rumble_off--;
            else continue;
            uint8_t s[4], out[JC1_REPORT_LEN] = {0}; jc1_rumble_encode(&v, s); jc1_rumble_report(out, d->counter++, s);
            jc1_write(d, out, sizeof out, 0);   // padded to the output report's 49 bytes, as SDL sends it over Bluetooth
        }
    }
}

// ---- Reports ----
static void jc1_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    jc1_dev *d = ctx;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len <= 0) return;
    size_t n = (size_t)len;
    if (report_id == 0x21) {   // a subcommand's reply: the IMU calibration, or a stick calibration block
        if (jc1_unit_cal(&d->unit, r, n)) {
            const jc1_cal *c = &d->unit.cal;
            if (g_jc1_log) LOG("[jc1] %s Joy-Con: factory IMU calibration read (gyro %.4f/%.4f/%.4f deg/s per count, origin %.0f/%.0f/%.0f counts)",
                               jc1_name(d), c->gyro_scale[0], c->gyro_scale[1], c->gyro_scale[2], c->gyro_origin[0], c->gyro_origin[1], c->gyro_origin[2]);
        } else if (pad_pro_spi_reply(&d->stick, r, n) >> d->right & 1) {
            int k = d->right;
            if (g_jc1_log) LOG("[jc1] %s Joy-Con: stick calibration from the %s: rest %d %d, reach %d-%d %d-%d", jc1_name(d), d->stick.cal[k] == 2 ? "user's" : "factory's",
                               d->stick.rest[2 * k], d->stick.rest[2 * k + 1], d->stick.reach[2 * k][0], d->stick.reach[2 * k][1], d->stick.reach[2 * k + 1][0], d->stick.reach[2 * k + 1][1]);
        }
        return;
    }
    if (report_id != 0x30 || n < JC1_REPORT_LEN || r[0] != 0x30) return;   // nothing below reads a report shorter than 0x30's 49 bytes
    uint64_t now = now_ns();
    if (!d->said) {
        d->said = 1;
        LOG("[jc1] %s Joy-Con (pid 0x%04x): reports arriving, read here (buttons, stick, motion, rumble, lights)", jc1_name(d), d->pid);
        dispatch_async(dispatch_get_main_queue(), ^{ jc1_models_changed(); });   // GameController's controller of it is left unread from now on
    }
    memcpy(d->last, r, JC1_REPORT_LEN); d->have = 1;
    d->buttons = jc1_joy_buttons(r, d->right);
    jc1_joy_stick(&d->stick, r, n, d->right, d->st);
    jc1_sample s[JC1_IMU_SAMPLES];
    int k = jc1_input(&d->unit, r, n, !d->right, now, s);
    if (k > 0) { d->quiet_said = 0; if (d->unit.cal.have) d->wait = JC1_WAIT_NS; }   // streaming and calibrated: the watchdog's wait starts over
    if (g_jc1_log >= 3 && now - d->logged > 1000000000ull) {   // the chip's own counts, against the calibration's origin and the learned rest
        d->logged = now;
        const uint8_t *b = r + 13 + 12 * 2; const jc1_unit *u = &d->unit;
        LOG("[jc1] %s Joy-Con raw: accel %d/%d/%d gyro %d/%d/%d counts (origin %.0f/%.0f/%.0f, the rest offset %.0f/%.0f/%.0f)",
            jc1_name(d), jc1_s16(b), jc1_s16(b + 2), jc1_s16(b + 4), jc1_s16(b + 6), jc1_s16(b + 8), jc1_s16(b + 10),
            u->cal.gyro_origin[0], u->cal.gyro_origin[1], u->cal.gyro_origin[2], u->rest.bias[0], u->rest.bias[1], u->rest.bias[2]);
    }
    jc1_pad *p = d->pad; if (!p) return;
    jc1pad_read(p);   // the pad's whole state; a first press makes it a player
    jc1_pad_sync(p);
    int pl = jc1pad_player(p);
    if (pl >= 0 && g_jc1_active && !jc1pad_blocked()) {
        if (g_jc1_joy) {   // the Joy-Con's own buttons, stick and sensor, upright
            dm_joy_set(pl, d->right, d->buttons, d->st[0], d->st[1]);
            for (int i = 0; i < k; i++) dm_motion_set_joy(pl, d->right, s[i].acc, s[i].rot, s[i].ns);
        }
        int have[2] = {p->side[0] != NULL, p->side[1] != NULL}, live[2];
        for (int i = 0; i < 2; i++) live[i] = p->side[i] && jc1_live(&p->side[i]->unit.stream, now);
        if (k > 0 && jc1_merged_side(have, live, g_jc1_prefer) == d->right) {   // the pad's merged stream: a Joy-Con alone in the frame of one held sideways
            int alone = jc1_pad_sides(p) != 3;
            for (int i = 0; i < k; i++) {
                float acc[3], rot[3];
                if (alone) { jc1_sideways_vec(s[i].acc, d->right, acc); jc1_sideways_vec(s[i].rot, d->right, rot); } else { memcpy(acc, s[i].acc, sizeof acc); memcpy(rot, s[i].rot, sizeof rot); }
                dm_motion_set_hid(pl, acc, rot, s[i].ns);
            }
        }
        if (g_jc1_log == 2 && k > 0 && now - d->logged > 1000000000ull) {   // DM_JC1_LOG=2: the orientation the host hands the game, per Joy-Con
            d->logged = now;
            const float *a = s[k - 1].acc, *w = s[k - 1].rot;
            LOG("[jc1] %s Joy-Con frame: gravity %+.2f %+.2f %+.2f G (vertical, top up = 0 -1 0), turning %+.1f %+.1f %+.1f deg/s",
                jc1_name(d), a[0], a[1], a[2], w[0] * 57.2958f, w[1] * 57.2958f, w[2] * 57.2958f);
        }
    }
    if (g_jc1_joy) jc1_gesture_check(d, now);
}
static void jc1_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess) return;
    if (IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;   // GameController's own device
    int pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey)) intValue];
    if (pid != 0x2006 && pid != 0x2007) return;   // Joy-Con (L) and (R)
    for (jc1_dev *d = g_jc1_devs; d; d = d->next) if (d->hid == hid) return;
    jc1_dev *d = calloc(1, sizeof *d);
    if (!d) { LOG("[RED] Joy-Con: cannot allocate device state"); return; }
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->pid = pid; d->right = pid == 0x2007; d->id = ++g_jc1_next_id; d->wait = JC1_WAIT_NS; d->lights = -1;
    jc1_unit_init(&d->unit);
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, jc1_report, d);
    // Nonexclusive: macOS's own driver keeps the device and GameController keeps listing it (verified on a paired L + R); its controller is left unread (jc1_gc_unread)
    IOReturn o = IOHIDDeviceOpen(hid, kIOHIDOptionsTypeNone);
    if (o != kIOReturnSuccess) {
        LOG("[RED] Joy-Con: IOHIDDeviceOpen failed (0x%x); pid 0x%04x stays with GameController (no motion)", o, pid);
        IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
        CFRelease(d->hid); free(d);
        return;
    }
    d->next = g_jc1_devs; g_jc1_devs = d;
    // Its pad: the other side alone, else one of its own
    uint32_t sides[64]; jc1_pad *pads[64]; int n = 0;
    for (jc1_pad *p = g_jc1_pads; p && n < 64; p = p->next) { sides[n] = jc1_pad_sides(p); pads[n++] = p; }
    int at = g_jc1_joy ? jc1_pair_pick(sides, n, d->right) : -1;
    jc1_pad *p = at >= 0 ? pads[at] : jc1_pad_new();
    if (!p) { LOG("[RED] Joy-Con: cannot allocate pad state"); g_jc1_devs = d->next; IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL); IOHIDDeviceClose(hid, kIOHIDOptionsTypeNone); CFRelease(d->hid); free(d); return; }
    jc1_enter(d, p);
    LOG("[jc1] %s Joy-Con connected%s", jc1_name(d), at >= 0 ? ": paired with the other one" : "");
    if (at < 0) jc1pad_attach(p); else { jc1_pad_sync(p); jc1pad_read(p); }
    jc1_handshake(d);
}
static void jc1_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    jc1_dev **link = &g_jc1_devs;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    jc1_dev *d = *link; *link = d->next;
    LOG("[jc1] %s Joy-Con gone", jc1_name(d));
    // Unregister while its buffer and context still exist; the main run loop serializes removal with its report callbacks, its handshake steps and the rumble tick. Its writes
    // still queued finish (or fail) before the device is closed
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
    dispatch_sync(g_jc1_out, ^{});
    IOHIDDeviceClose(hid, kIOHIDOptionsTypeNone);
    jc1_leave(d);
    CFRelease(d->hid); free(d);
    jc1_models_changed();
}
// macOS resets the input report mode itself in some wakes (the IMU then reads zero, or no 0x30 arrives at all), and the controller drops a subcommand sent too soon: a
// Joy-Con with no sensor data since its last handshake gets another, after a second and then after twice as long each time it stays off, up to 8 s (a Joy-Con whose sensor
// never comes on is asked every 8 s, not every second). One line says so per silence; DM_JC1_LOG names every retry. A Joy-Con that streams but never answered the
// calibration read (a subcommand it dropped) is asked again on the same schedule: without it, its gyro scale and origin are the defaults.
static void jc1_watchdog(void) {
    uint64_t now = now_ns();
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
// Focus lost (0): the Joy-Cons stop driving the game (nothing stays pressed, the sensors lie still); back (1): they go on
static void dm_jc1_active(int on) {
    g_jc1_active = on;
    if (!on) for (jc1_pad *p = g_jc1_pads; p; p = p->next) { int pl = jc1pad_player(p); if (pl >= 0) { dm_motion_clear_hid(pl); jc1_joy_release(pl, 0); jc1_joy_release(pl, 1); } }
}
// DM_JC1=0 turns it off (the Joy-Cons then play through GameController, without motion); DM_JC1_SIDE=L|R picks the half of a pair that feeds the merged six-axis stream (default:
// right, or the left one when the right one is not streaming); DM_JC1_LOG=1 logs the handshake, the calibrations and failed writes, 2 a frame line a second per Joy-Con, 3 the raw
// counts instead. merge: DM_PAD_MERGE, every pad is player 1 and no pad holds a game's Joy-Cons apart
static void dm_jc1_start(int merge) {
    const char *e = getenv("DM_JC1");
    if (e && !atoi(e)) return;
    g_jc1_joy = !merge;
    e = getenv("DM_JC1_LOG");
    g_jc1_log = e ? atoi(e) : 0;
    e = getenv("DM_JC1_SIDE");
    g_jc1_prefer = e && (*e == 'L' || *e == 'l') ? 0 : 1;
    NSArray *match = @[@{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2006},
                       @{@kIOHIDVendorIDKey: @0x057E, @kIOHIDProductIDKey: @0x2007}];
    g_jc1_out = dispatch_queue_create("dm.jc1.out", DISPATCH_QUEUE_SERIAL);
    g_jc1_rumble_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());   // before any device: jc1_rumble_timer_sync resumes it with the first pad
    dispatch_source_set_timer(g_jc1_rumble_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)JC1_RUMBLE_TICK_NS), JC1_RUMBLE_TICK_NS, 1000000);
    dispatch_source_set_event_handler(g_jc1_rumble_timer, ^{ jc1_rumble_tick(); });
    g_jc1_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_jc1_manager) return;
    IOHIDManagerSetDeviceMatchingMultiple(g_jc1_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_jc1_manager, jc1_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_jc1_manager, jc1_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_jc1_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_jc1_manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        LOG("[RED] Joy-Con: cannot open the HID manager (0x%x); Joy-Cons play through GameController, without motion", r);
        IOHIDManagerUnscheduleFromRunLoop(g_jc1_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_jc1_manager); g_jc1_manager = NULL;
        return;
    }
    g_jc1_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(g_jc1_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_jc1_timer, ^{ jc1_watchdog(); });
    dispatch_resume(g_jc1_timer);
}
