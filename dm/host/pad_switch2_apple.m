// CoreBluetooth transport for Switch 2 controllers, shared by the Mac and iOS front ends (pad_switch2_apple.h says what it does; switch2_proto.h is the protocol).
// Everything runs on the main queue, where the front ends' own pad callbacks run.
#import "pad_switch2_apple.h"
#import "switch2_ops_apple.h"
#import <CoreBluetooth/CoreBluetooth.h>
#import <TargetConditionals.h>
#if TARGET_OS_OSX
#import <IOBluetooth/IOBluetooth.h>   // IOBluetoothHostController: this Mac's own Bluetooth address, which a bonded controller wakes toward
#endif
#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <os/lock.h>

static dm_switch2_front g_front;
static s2_pad *g_pads;
static int g_verbose, g_motion_right = 1, g_motion = 1, g_active = 1, g_joy = 1;   // g_joy: each Joy-Con reported on its own (not with DM_PAD_MERGE)
static float g_gyro = S2_GYRO_RAD;
static float g_dead = S2_DEADZONE;   // the sticks' dead zone in raw counts (DM_SW2_DEADZONE)
static int8_t g_map[6];
static int g_map_set;               // DM_SW2_MAP overrides the per-layout maps
static uint8_t g_host[6];           // this host's Bluetooth address, little endian, as a bonded controller's wake advertisement carries it
static int g_host_known;
static int g_on;                                // dm_switch2_start returned 1
static uint32_t g_next_id;                      // the id of the next connection (dm_sw2_info.id)
static int g_saving;                            // calibration saves running, on any controller (dm_switch2_saving)
static void (^g_changed)(void);                 // the tester's hooks (dm_switch2_observe)
static void (^g_report)(uint32_t);
static int g_calibrate;                         // DM_SW2_CALIBRATE: each controller's stick rest written as its user calibration, once a run
static NSMutableSet<NSString *> *g_calibrated;  // the serials written (or tried) this run

s2_pad *dm_switch2_pads(void) { return g_pads; }

// Rumble: each player's values as the game sets them, with their times (s2_rumble_log), written by the rumble queue (dm_switch2_rumble) and read by the main queue's tick. Every
// 15 ms each controller sends a packet of the next three 5 ms slots (switch2_proto.h, Rumble timing), the last one ending S2_RUMBLE_LAG_NS before the tick
static s2_rumble_log g_rlog[DM_PLAYERS];
static os_unfair_lock g_rlog_lock = OS_UNFAIR_LOCK_INIT;
#define S2_RUMBLE_TICK_S 0.015
#define S2_RUMBLE_LAG_NS 5000000ull   // so a late tick (the timer's own jitter) still finds every value of its slots
void dm_switch2_rumble(const dm_haptic_state *st) {
    if (!st || st->player < 0 || st->player >= DM_PLAYERS) return;
    s2_haptic m[2];
    for (int k = 0; k < 2; k++) { const dm_vibration *v = &st->motor[k]; m[k] = s2_haptic_from(v->amplitude_low, v->frequency_low, v->amplitude_high, v->frequency_high); }
    uint64_t t = now_ns();
    os_unfair_lock_lock(&g_rlog_lock); s2_rumble_log_add(&g_rlog[st->player], t, m); os_unfair_lock_unlock(&g_rlog_lock);
}
static NSTimer *g_rumble_timer;
static struct { uint64_t last, maxgap, ticks, sent, busy, t0; } g_rstat;   // DM_SW2_VERBOSE: the rumble tick's spacing and the packets sent, each second
// One sample as the console treats it before coding: both bands scaled so that they add up to at most 0.98 (switch2_proto.h s2_rumble_cap)
static void s2_rumble_limit(s2_haptic *v) { float lo = v->lf_amp / 1023.f, hi = v->hf_amp / 1023.f; s2_rumble_cap(&lo, &hi); v->lf_amp = s2_amp_code(lo); v->hf_amp = s2_amp_code(hi); }

@interface S2Command : NSObject
@property (nonatomic) uint8_t cmd, sub; @property (nonatomic, copy) NSData *payload; @property (nonatomic, copy) void (^done)(NSData *);
@end
@implementation S2Command
@end

@interface S2Session : NSObject <CBPeripheralDelegate, S2Commander>   // S2Commander: the memory writes and bonding of switch2_ops_apple.m run through it
@property (nonatomic, strong) CBPeripheral *peripheral;
@property (nonatomic) uint16_t pid;
@property (nonatomic) int use;      // S2_ADV_PAIR, S2_ADV_TAKE or S2_ADV_WAKE: why it was connected
@property (nonatomic) s2_pad *pad;
@property (nonatomic) BOOL live;
@property (nonatomic, readonly) uint32_t cid;   // this connection's id (dm_sw2_info.id)
@property (nonatomic, readonly) BOOL saving;    // a calibration save is running on it
@end

static NSMutableDictionary<NSUUID *, S2Session *> *g_sessions;
static CBCentralManager *g_central;
static void s2_session_closed(S2Session *s);
static void s2_rumble_timer(void);

static const char *s2_kind(uint16_t pid) { return pid == S2_PID_PRO ? "Pro Controller 2" : pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)"; }
static s2_dev *s2_dev_of(s2_pad *p, uint16_t pid) { return pid == S2_PID_PRO ? &p->st.pro : pid == S2_PID_JOYCON_L ? &p->st.left : &p->st.right; }
static int s2_pad_empty(const s2_pad *p) { return !p->st.pro.have && !p->st.left.have && !p->st.right.have; }
static int s2_player(s2_pad *p) { return p && g_front.player ? g_front.player(p) : -1; }
static int s2_blocked(void) { return !g_active || (g_front.blocked && g_front.blocked()); }
// Which Joy-Cons a pad holds, to the npad layer (dm_joy_attach): a Pro Controller 2 is 0
static void s2_sync_sides(s2_pad *p) {
    int pl = g_joy ? s2_player(p) : -1; if (pl < 0) return;
    dm_joy_attach(pl, p->st.pro.have ? 0 : (uint32_t)((p->st.left.have ? 1 : 0) | (p->st.right.have ? 2 : 0) | DM_JOY_2));
}
static void s2_joy_release(int pl, int side) { if (g_joy && pl >= 0) { dm_joy_set(pl, side, 0, 0, 0); dm_motion_clear_joy(pl, side); } }
static void s2_pad_unlink(s2_pad *p) { s2_pad **link = &g_pads; while (*link && *link != p) link = &(*link)->next; if (*link) *link = p->next; }
static s2_pad *s2_pad_for(uint16_t pid) {   // a Joy-Con joins a pad that has the other one alone; anything else starts a pad
    if (pid != S2_PID_PRO)
        for (s2_pad *p = g_pads; p; p = p->next)
            if (!p->st.pro.have && ((pid == S2_PID_JOYCON_L && p->st.right.have && !p->st.left.have) || (pid == S2_PID_JOYCON_R && p->st.left.have && !p->st.right.have))) return p;
    s2_pad *p = calloc(1, sizeof *p); if (!p) return NULL;
    snprintf(p->name, sizeof p->name, "%s", pid == S2_PID_PRO ? "Pro Controller 2" : "Joy-Con 2");
    p->next = g_pads; g_pads = p;
    return p;
}

@implementation S2Session {
    CBCharacteristic *_wr, *_rsp, *_inp;
    NSMutableArray<S2Command *> *_queue;
    BOOL _pending; uint8_t _pendingCmd; NSUInteger _seq; void (^_cb)(NSData *);
    s2_cal _cal[2], _factory[2]; BOOL _hasFactory[2];   // _cal: what the game uses (user, else factory); _factory: the factory block, _cal's copy while it did not read
    NSTimer *_tick; uint64_t _reports; BOOL _handshaken; s2_rest _rest;
    s2_clock _clk; uint64_t _lost; char _serial[15]; uint64_t _restSaved;   // sample times from the controller's clock; the serial its learned gyro offset is kept under
    CBCharacteristic *_vib; uint8_t _vseq; int _offLeft; s2_rumble_cursor _rcur;   // _rcur: where this controller is in its player's rumble log
    int _player;   // the player at the last read (playerSync): a pad waiting for its first button press (DM_PAD_PRESS_TO_JOIN) gets one later
    int16_t _mouseX, _mouseY; BOOL _mouseSeen, _mouseOn;   // _mouseOn: its mouse sensor is turned on (features)   // the mouse sensor's counters at the last report
    s2_hold _hold;   // the choosing gesture this Joy-Con holds now (s2_gesture)
    int _calStep; s2_stick_rest _stickRest;   // DM_SW2_CALIBRATE: 1 while it waits for the released stick's rest, 2 while it writes
    uint32_t _cid; BOOL _saving;
    BOOL _closed;   // torn down: every command is answered nil at once, so an operation in progress (a calibration save) ends and says so
}
- (void)dealloc { [self saveRest]; }
- (uint32_t)cid { return _cid; }
- (BOOL)saving { return _saving; }
- (void)fill:(dm_sw2_info *)o {   // dm_switch2_controllers' entry
    memset(o, 0, sizeof *o);
    const s2_dev *d = s2_dev_of(_pad, _pid);
    o->id = _cid; o->pid = _pid; o->kind = s2_kind(_pid); memcpy(o->serial, _serial, sizeof o->serial); o->player = s2_player(_pad);
    for (int i = 0; i < 2; i++) { o->cal[i] = _cal[i]; o->factory[i] = _factory[i]; }
    o->stick[0][0] = _pid == S2_PID_JOYCON_R ? d->r.rx : d->r.lx; o->stick[0][1] = _pid == S2_PID_JOYCON_R ? d->r.ry : d->r.ly;   // a Joy-Con's only stick is the one its side reports
    if (_pid == S2_PID_PRO) { o->stick[1][0] = d->r.rx; o->stick[1][1] = d->r.ry; }
    o->clock_ms = _clk.ms; o->saving = _saving;
}
- (instancetype)initWith:(CBPeripheral *)p pid:(uint16_t)pid use:(int)use {
    if ((self = [super init])) { _peripheral = p; _pid = pid; _use = use; p.delegate = self; _queue = [NSMutableArray new]; s2_cal_default(&_cal[0]); s2_cal_default(&_cal[1]); _factory[0] = _cal[0]; _factory[1] = _cal[1]; _cid = ++g_next_id; }
    return self;
}
- (void)begin {
    [_peripheral discoverServices:nil];
    __weak S2Session *w = self;   // a controller that connects but never finishes the handshake is dropped
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ S2Session *s = w; if (s && !s->_handshaken) [s fail:@"handshake timed out"]; });
}
- (void)fail:(NSString *)why {
    if (_closed) return;   // already gone (a reply answered nil after the teardown): the peripheral may be another session's by now
    LOG("[sw2] %s: %s", s2_kind(_pid), why.UTF8String);
    [g_central cancelPeripheralConnection:_peripheral];   // didDisconnect cleans up; a session that never connected is dropped there too
    if (!_live) s2_session_closed(self);
}
- (void)teardown {
    [_tick invalidate]; _tick = nil;
    _closed = YES;
    if (_pending) { _pending = NO; void (^d)(NSData *) = _cb; _cb = nil; if (d) dispatch_async(dispatch_get_main_queue(), ^{ d(nil); }); }
    [self pump];
    if (_pad) {
        s2_pad *p = _pad; _pad = NULL;
        int player = s2_player(p);
        s2_dev *d = s2_dev_of(p, _pid); memset(d, 0, sizeof *d);
        if (player >= 0) dm_motion_clear_hid(player);
        if (_pid != S2_PID_PRO) s2_joy_release(player, _pid == S2_PID_JOYCON_R);
        if (s2_pad_empty(p)) {
            if (player >= 0 && g_joy) dm_joy_attach(player, 0);
            s2_pad_unlink(p);
            if (g_front.detach) g_front.detach(p);
            free(p);
        } else { s2_sync_sides(p); if (g_front.read) g_front.read(p); }
    }
}
// ---- commands: one in flight (the controller has one reply channel) ----
- (void)send:(uint8_t)cmd sub:(uint8_t)sub payload:(const uint8_t *)pl len:(size_t)n done:(void (^)(NSData *))done {
    S2Command *c = [S2Command new]; c.cmd = cmd; c.sub = sub; c.payload = [NSData dataWithBytes:pl length:n]; c.done = done;
    [_queue addObject:c]; [self pump];
}
- (void)pump {
    if (_pending || !_queue.count) return;
    if (_closed) { NSArray<S2Command *> *q = [_queue copy]; [_queue removeAllObjects]; dispatch_async(dispatch_get_main_queue(), ^{ for (S2Command *c in q) if (c.done) c.done(nil); }); return; }
    if (!_wr) return;
    S2Command *c = _queue.firstObject; [_queue removeObjectAtIndex:0];
    uint8_t f[8 + 255]; size_t n = s2_command(f, sizeof f, c.cmd, c.sub, c.payload.bytes, c.payload.length);
    if (!n) { if (c.done) c.done(nil); [self pump]; return; }
    _pending = YES; _pendingCmd = c.cmd; _cb = c.done; NSUInteger mine = ++_seq; uint8_t id = c.cmd;
    __weak S2Session *w = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        S2Session *s = w; if (!s || !s->_pending || s->_seq != mine) return;
        s->_pending = NO; void (^d)(NSData *) = s->_cb; s->_cb = nil;
        LOG("[sw2] %s: command 0x%02x timed out", s2_kind(s->_pid), id);
        if (d) d(nil);
        [s pump];
    });
    [_peripheral writeValue:[NSData dataWithBytes:f length:n] forCharacteristic:_wr type:CBCharacteristicWriteWithoutResponse];
}
- (void)readMemory:(uint8_t)len at:(uint32_t)addr done:(void (^)(NSData *))done {
    uint8_t p[8]; s2_memory_payload(p, len, addr);
    [self send:S2_CMD_MEMORY sub:S2_SUB_MEMORY_READ payload:p len:8 done:^(NSData *r) {
        done(r.length >= S2_REPLY_DATA + len && ((const uint8_t *)r.bytes)[8] == len ? [r subdataWithRange:NSMakeRange(S2_REPLY_DATA, len)] : nil);
    }];
}
// Slot's calibration from the controller, each block read verified (s2op_read): the user's when it parses, else the factory's, and the factory block kept apart (s2_cal_pick), as a
// travel is judged against it. keep: the re-read after a save, where a block that does not answer leaves what the session holds (the stick's calibration is unknown, not blank)
- (void)readCal:(int)slot keep:(BOOL)keep done:(void (^)(void))done {
    __weak S2Session *w = self;
    uint32_t user = slot ? S2_ADDR_USER_STICK2 : S2_ADDR_USER_STICK1, factory = slot ? S2_ADDR_FACTORY_STICK2 : S2_ADDR_FACTORY_STICK1;
    // The handshake reads each block once (a bounded wait: 2 slots x 2 blocks x 2 s stays inside its 20 s timer); the re-read after a save retries (S2_CAL_TRIES). Both take a reply
    // only when it echoes the request (a late reply to the user block's read is not the factory block)
    void (^log)(NSString *) = [self opsLog];
    void (^rd)(S2Session *, uint32_t, void (^)(NSData *)) = ^(S2Session *x, uint32_t a, void (^d)(NSData *)) { s2op_read(x, a, 0x0B, keep ? S2_CAL_TRIES : 1, log, d); };
    rd(self, user, ^(NSData *u) {
        S2Session *s = w; if (!s) return;
        if (!u && keep) { done(); return; }
        rd(s, factory, ^(NSData *f) {
            S2Session *t = w; if (!t) return;
            s2_cal uc, fc; s2_cal_default(&uc); s2_cal_default(&fc);
            int hasU = u && s2_cal_parse(u.bytes, u.length, &uc), hasF = f && s2_cal_parse(f.bytes, f.length, &fc);
            if (!f && keep) { fc = t->_factory[slot]; hasF = t->_hasFactory[slot]; }   // the factory block does not change: the one read before stands
            if (!f && keep && !hasU && !hasF) { done(); return; }   // neither the stick's user calibration nor its factory one is known: what the session holds stays
            s2_cal_pick(&uc, hasU, &fc, hasF, &t->_cal[slot], &t->_factory[slot]); t->_hasFactory[slot] = hasF;
            LOG("[sw2] %s: stick %d uses its %s calibration%s", s2_kind(t->_pid), slot + 1, hasU ? "user" : hasF ? "factory" : "default (none reads)", hasF ? "" : "; its factory block does not read");
            if (t->_pad) s2_dev_of(t->_pad, t->_pid)->cal[slot] = t->_cal[slot];
            done();
        });
    });
}
// ---- DM_SW2_CALIBRATE: the released stick's rest written into the controller as its user calibration. Only the first stick slot (a Pro Controller 2's left stick, a Joy-Con
// 2's only one). The write itself (backup, write, read back, restore) is s2op_save_user_cal (switch2_ops_apple.m) ----
- (NSUInteger)writeChunk {   // data bytes a write carries: what one write without response takes less the two 8-byte headers, in 16s, 16 to 64
    NSInteger m = (NSInteger)[_peripheral maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse] - 16;
    return (NSUInteger)MAX((NSInteger)16, MIN((NSInteger)64, m & ~(NSInteger)15));
}
- (void (^)(NSString *))opsLog {   // switch2_ops_apple.m's lines, as this controller's
    uint16_t pid = _pid;
    return ^(NSString *m) { LOG("[sw2] %s: %s", s2_kind(pid), m.UTF8String); };
}
- (void)startCalibration {
    if (!_serial[0] || [g_calibrated containsObject:@(_serial)]) return;
    if (!_cal[0].valid) { LOG("[sw2] %s: not calibrated (DM_SW2_CALIBRATE): its stick's travel could not be read from the controller, and the calibration keeps it", s2_kind(_pid)); return; }
    memset(&_stickRest, 0, sizeof _stickRest); _calStep = 1;
    LOG("[sw2] %s: calibrating (DM_SW2_CALIBRATE): let go of the %sstick and leave it untouched for two seconds", s2_kind(_pid), _pid == S2_PID_PRO ? "left " : "");
}
- (void)calibrateAt:(float)x y:(float)y {
    s2_cal now = _cal[0], cal;
    if (!s2_stick_rest_near(&now, x, y)) {   // held: wait for it to be let go
        LOG("[sw2] %s: the stick rests %.0f, %.0f counts from its centre, more than drift does: let go of it (DM_SW2_CALIBRATE)", s2_kind(_pid), x - now.cx, y - now.cy);
        memset(&_stickRest, 0, sizeof _stickRest); return;
    }
    if (!s2_cal_recentre(&now, x, y, &cal)) { LOG("[sw2] %s: a centre of %.0f, %.0f leaves the stick too little travel: nothing written", s2_kind(_pid), x, y); _calStep = 0; return; }
    _calStep = 2; [g_calibrated addObject:@(_serial)];
    LOG("[sw2] %s: the stick rests at %.0f, %.0f, its calibration's centre is %.0f, %.0f: writing the rest as its user calibration", s2_kind(_pid), x, y, now.cx, now.cy);
    __weak S2Session *w = self;
    [self saveCal:cal slot:0 done:^(S2CalOutcome o, NSString *path) { (void)o; (void)path; S2Session *s = w; if (s) s->_calStep = 0; }];
}
// cal as the user calibration of slot 0 (s2op_save_user_cal), then the session's calibration of that slot read again from the controller whatever came of it, so the game and the
// tester use what the controller now holds (a failed restore may have left it blank). Refused at once when one is running, the link is gone or cal is no calibration. done after it returns
- (void)saveCal:(s2_cal)cal slot:(int)slot done:(void (^)(S2CalOutcome, NSString *))done {
    NSString *why = slot != 0 ? @"only the first stick slot can be saved (where the second slot's user calibration lives is not settled)" : _closed || !_live ? @"the controller is no longer connected"
                  : _saving ? @"a save on it is still running" : !cal.valid ? @"that is not a calibration the controller would read" : nil;
    if (why) {
        LOG("[sw2] %s: nothing saved: %s", s2_kind(_pid), why.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{ if (done) done(S2CalNoBackup, nil); });
        return;
    }
    _saving = YES; g_saving++;
    LOG("[sw2] %s %s: saving centre %.0f, %.0f, travel %.0f, %.0f up and %.0f, %.0f down as its user calibration", s2_kind(_pid), _serial, cal.cx, cal.cy, cal.maxx, cal.maxy, cal.minx, cal.miny);
    if (g_changed) g_changed();
    S2Session *me = self;   // held until the save ends: the operation holds its commander, and a controller that went away still answers
    s2op_save_user_cal(self, _serial, cal, s2op_backup_dir(), [self opsLog], ^(S2CalOutcome o, NSString *path) {
        if (o == S2CalSaved) { me->_cal[slot] = cal; if (me->_pad) s2_dev_of(me->_pad, me->_pid)->cal[slot] = cal; }   // what a re-read that gets no answer leaves
        [me readCal:slot keep:YES done:^{
            me->_saving = NO; g_saving--;
            if (g_changed) g_changed();
            if (done) done(o, path);
        }];
    });
}
- (void)setLEDs:(void (^)(void))done {
    int p = s2_player(_pad); uint8_t pl[4] = {s2_led_pattern[p >= 0 && p < 8 ? p : 0], 0, 0, 0};
    [self send:S2_CMD_LEDS sub:S2_SUB_LEDS_SET payload:pl len:4 done:^(NSData *r) { (void)r; if (done) done(); }];
}
// After the front end's read(): it may have given the pad a player at its first button press (DM_PAD_PRESS_TO_JOIN), and the pad's Joy-Con styles and player lights follow
- (void)playerSync {
    int player = s2_player(_pad); if (player == _player) return;
    _player = player;
    if (player >= 0) { s2_sync_sides(_pad); [self setLEDs:nil]; }
    if (g_changed) g_changed();   // the Controller menu names each controller's player
}
- (void)handshake {
    __weak S2Session *w = self;
    [self readMemory:0x40 at:S2_ADDR_INFO done:^(NSData *info) {
        S2Session *s = w; if (!s) return;
        uint16_t vid, pid; char serial[15];
        if (!info || !s2_info_parse(info.bytes, info.length, &vid, &pid, serial) || vid != S2_VENDOR_ID) { [s fail:@"the controller did not answer the identity read"]; return; }
        if (pid != s->_pid) LOG("[sw2] advertised 0x%04x but identifies as 0x%04x", s->_pid, pid);
        s->_pid = pid; memcpy(s->_serial, serial, sizeof s->_serial);
        void (^calibrate0)(void) = ^{ S2Session *a = w; if (!a) return;
            [a readCal:0 keep:NO done:^{ S2Session *b = w; if (!b) return;
                void (^next)(void) = ^{ S2Session *c = w; if (!c) return; [c finish]; };
                if (b->_pid == S2_PID_PRO) [b readCal:1 keep:NO done:next]; else next();
            }];
        };
        void (^calibrate)(void) = ^{ S2Session *a = w; if (!a) return; [a seedRest:calibrate0]; };
        if (s->_use == S2_ADV_PAIR) [s bond:calibrate]; else calibrate();
    }];
}
// The gyro offset before the controller has lain still for a second (s2_rest): the one learned for this controller in an earlier session, kept under its serial number,
// else its factory offset where that matches the rest offset (s2_gyro_cal_usable), else none until it rests. Either way s2_rest keeps refining it
- (void)seedRest:(void (^)(void))done {
    NSString *key = [NSString stringWithFormat:@"sw2.gyroOffset.%s", _serial];
    NSArray *kept = _serial[0] ? [NSUserDefaults.standardUserDefaults arrayForKey:key] : nil;
    if (kept.count == 3) {
        for (int i = 0; i < 3; i++) _rest.bias[i] = [kept[i] floatValue];
        _rest.have = 1;
        LOG("[sw2] %s: gyro offset %.1f, %.1f, %.1f counts, as learned in an earlier session", s2_kind(_pid), _rest.bias[0], _rest.bias[1], _rest.bias[2]);
        done(); return;
    }
    __weak S2Session *w = self;
    [self readMemory:0x10 at:S2_ADDR_GYRO_CAL done:^(NSData *d) {
        S2Session *s = w; if (!s) return;
        float off[3];
        if (d && s2_gyro_cal_parse(d.bytes, d.length, off)) {
            if (s2_gyro_cal_usable(s->_pid)) {
                memcpy(s->_rest.bias, off, sizeof off); s->_rest.have = 1;
                LOG("[sw2] %s: gyro offset %.1f, %.1f, %.1f counts, from its factory calibration", s2_kind(s->_pid), off[0], off[1], off[2]);
            } else LOG("[sw2] %s: factory gyro offset %.1f, %.1f, %.1f counts not used for this controller; it is learned once the controller rests", s2_kind(s->_pid), off[0], off[1], off[2]);
        } else LOG("[sw2] %s: no usable factory gyro offset; it is learned once the controller rests", s2_kind(s->_pid));
        done();
    }];
}
- (void)saveRest {
    if (!_serial[0] || !_rest.have) return;
    [NSUserDefaults.standardUserDefaults setObject:@[@(_rest.bias[0]), @(_rest.bias[1]), @(_rest.bias[2])] forKey:[NSString stringWithFormat:@"sw2.gyroOffset.%s", _serial]];
}
// Sync is held on this controller (its advertisement named no host): give it this host's address and fresh keys, as the console does (s2op_bond). It plays either way
- (void)bond:(void (^)(void))done {
    __weak S2Session *w = self;
    s2op_bond(self, g_host, [self opsLog], ^(BOOL ok) { (void)ok; S2Session *s = w; if (!s || s->_closed) return; done(); });
}
// The features this controller reports: a Joy-Con 2's mouse sensor too once the game reads the sensors (dm_joy_mouse_wanted; the Pro Controller 2 has none), as the console
// turns it on only for a game that uses it. Once on, it stays on while the controller is connected
- (uint8_t)features {
    if (_pid != S2_PID_PRO && dm_joy_mouse_wanted()) _mouseOn = YES;
    return _mouseOn ? S2_FEATURES | S2_FEATURE_MOUSE : S2_FEATURES;
}
// The game set up the mouse sensors after this controller connected: the handshake's two feature commands again, with the sensor's bit
- (void)mouseOn {
    __weak S2Session *w = self;
    uint8_t fe[4] = {S2_FEATURES | S2_FEATURE_MOUSE, 0, 0, 0};
    [self send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_SET payload:fe len:4 done:^(NSData *r1) {
        (void)r1; S2Session *s = w; if (!s) return;
        uint8_t on[4] = {S2_FEATURES | S2_FEATURE_MOUSE, 0, 0, 0};
        [s send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_ENABLE payload:on len:4 done:^(NSData *r2) { S2Session *t = w; if (t) LOG("[sw2] %s: mouse sensor %s", s2_kind(t->_pid), r2 ? "on" : "not turned on (no answer)"); }];
    }];
}
- (void)finish {
    __weak S2Session *w = self;
    uint8_t feat = [self features];
    uint8_t pl[4] = {feat, 0, 0, 0};
    [self send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_SET payload:pl len:4 done:^(NSData *r1) {
        S2Session *s = w; if (!s) return; (void)r1;
        uint8_t on[4] = {feat, 0, 0, 0};
        [s send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_ENABLE payload:on len:4 done:^(NSData *r2) {
            S2Session *t = w; if (!t) return;
            if (!r2) { [t fail:@"enabling the features was refused"]; return; }
            [t attachPad];
            [t logUserSlots];
        }];
    }];
}
// Read only, once per connection: which of the user calibration sector's stick slots carry the marker (B2 A1), and their bytes. Where the second stick's user calibration lives
// (0x1FC060 as this host reads it, or 0x1FC080 as SDL and Linux do) is not settled; a controller whose right stick was calibrated on a console answers it
- (void)logUserSlots {
    s2op_read(self, S2_ADDR_USER_CAL_SECTOR + 0x40, 0x60, S2_CAL_TRIES, [self opsLog], ^(NSData *d) {
        void (^log)(NSString *) = [self opsLog];
        if (!d) { log(@"user calibration slots: the read of 0x1FC040-0x1FC09F got no answer"); return; }
        const uint8_t *b = d.bytes;
        log([NSString stringWithFormat:@"user calibration slots: marker at 0x1FC040 %s, 0x1FC060 %s, 0x1FC080 %s", s2_user_cal_marked(b) ? "yes" : "no", s2_user_cal_marked(b + 0x20) ? "yes" : "no",
             s2_user_cal_marked(b + 0x40) ? "yes" : "no"]);
        for (int row = 0; row < 3; row++) {
            NSMutableString *h = [NSMutableString string];
            for (int i = 0; i < 0x20; i++) [h appendFormat:@"%02x%s", b[row * 0x20 + i], i % 8 == 7 ? "  " : " "];
            log([NSString stringWithFormat:@"user calibration 0x%06x: %@", S2_ADDR_USER_CAL_SECTOR + 0x40 + row * 0x20, h]);
        }
    });
}
- (void)attachPad {
    s2_pad *p = s2_pad_for(_pid);
    if (!p) { [self fail:@"out of memory"]; return; }
    BOOL fresh = s2_pad_empty(p);
    s2_dev *d = s2_dev_of(p, _pid); memset(d, 0, sizeof *d);
    d->have = 1; d->pid = _pid; d->cal[0] = _cal[0]; d->cal[1] = _cal[1];
    _pad = p; _live = YES; _handshaken = YES;
    s2_rumble_timer();
    if (fresh) {
        if (!g_front.attach || !g_front.attach(p)) { LOG("[sw2] %s: no free player, not used", s2_kind(_pid)); [self fail:@"all players are taken"]; return; }
    } else if (g_front.read) g_front.read(p);
    s2_sync_sides(p);
    _player = s2_player(p);
    if (_player < 0) LOG("[sw2] %s connected: a player once a button is pressed%s", s2_kind(_pid), fresh ? "" : " (joined the Joy-Con already there)");
    else LOG("[sw2] %s connected: player %d%s", s2_kind(_pid), _player + 1, fresh ? "" : " (joined the Joy-Con already there)");
    if (g_changed) g_changed();
    [self setLEDs:nil];
    [_peripheral setNotifyValue:YES forCharacteristic:_inp];
    __weak S2Session *w = self;   // the link drops after about ten seconds without a write from the host, so the LEDs are written once a second
    _tick = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        S2Session *s = w; if (!s) { [t invalidate]; return; }
        int pl = s2_player(s->_pad); uint8_t l[4] = {s2_led_pattern[pl >= 0 && pl < 8 ? pl : 0], 0, 0, 0}; uint8_t f[12];
        size_t n = s2_command(f, sizeof f, S2_CMD_LEDS, S2_SUB_LEDS_SET, l, 4);
        if (s->_wr && n) [s->_peripheral writeValue:[NSData dataWithBytes:f length:n] forCharacteristic:s->_wr type:CBCharacteristicWriteWithoutResponse];
        if (!s->_mouseOn && [s features] != S2_FEATURES) [s mouseOn];
    }];
    if (g_calibrate) [self startCalibration];
}
// Every 15 ms: this controller's share of the player's motors (s2_rumble_pick) over its next three 5 ms slots (switch2_proto.h, Rumble timing).
// A buzz ends with three packets of silence, since one write without response can be lost
- (void)rumbleTick {
    if (!_live || !_pad || !_vib) return;
    int pl = s2_player(_pad); if (pl < 0 || pl >= DM_PLAYERS) return;
    s2_haptic a[3], b[3]; int on = 0;
    const s2_dev *dev = s2_dev_of(_pad, _pid); unsigned motors = s2_rumble_motors(&_pad->st, dev);
    s2_rumble_resync(&_rcur, now_ns(), S2_RUMBLE_LAG_NS);
    for (int k = 0; k < 3; k++) {
        os_unfair_lock_lock(&g_rlog_lock); s2_rumble_slot(&g_rlog[pl], &_rcur, motors); os_unfair_lock_unlock(&g_rlog_lock);
        s2_rumble_pick(&_pad->st, dev, _rcur.now, &a[k], &b[k]);
        s2_rumble_limit(&a[k]); s2_rumble_limit(&b[k]);
        on |= s2_haptic_on(&a[k]) || s2_haptic_on(&b[k]);
    }
    if (on) _offLeft = 3;
    else if (_offLeft > 0) _offLeft--;
    else return;
    uint8_t pk[33]; size_t n = s2_rumble_packet3(pk, a, b, _vseq++);
    if (!_peripheral.canSendWriteWithoutResponse) g_rstat.busy++;
    g_rstat.sent++;
    [_peripheral writeValue:[NSData dataWithBytes:pk length:n] forCharacteristic:_vib type:CBCharacteristicWriteWithoutResponse];
}
- (void)report:(NSData *)d {
    s2_report r; if (!_pad || !s2_report_parse(d.bytes, d.length, &r)) return;
    s2_dev *dev = s2_dev_of(_pad, _pid); dev->r = r; _reports++;
    if (_calStep == 1) {   // a right Joy-Con's stick is in the second pair of the report, its calibration in the first slot
        float rest[2];
        if (s2_stick_rest_update(&_stickRest, _pid == S2_PID_JOYCON_R ? r.rx : r.lx, _pid == S2_PID_JOYCON_R ? r.ry : r.ly, r.clock_ms, rest)) [self calibrateAt:rest[0] y:rest[1]];
    }
    unsigned lost; uint64_t ts = s2_clock_sample(&_clk, r.clock_ms, now_ns(), &lost); _lost += lost;
    if (g_report) g_report(_cid);
    int had = _rest.have; float was[3] = {_rest.bias[0], _rest.bias[1], _rest.bias[2]};
    s2_rest_update(&_rest, r.raw);
    if (_rest.have && (!had || was[0] != _rest.bias[0] || was[1] != _rest.bias[1] || was[2] != _rest.bias[2]) && ts - _restSaved > 30000000000ull) { [self saveRest]; _restSaved = ts; }
    if (g_verbose && _reports % 33 == 1) {
        // The bytes the parser does not read (0x18..0x2F around the battery, and past the sensor at 0x3C), in hex: whether a report carries more than one IMU sample, or the
        // sensor's own timestamp, is not known yet (the report has room for one 12-byte sample at 0x30)
        char hex[3 * 0x50 + 1] = ""; const uint8_t *b = d.bytes; size_t len = d.length, o = 0;
        for (size_t i = 0x18; i < len && o + 4 < sizeof hex; i++) if (i < 0x30 || i >= 0x3C) o += (size_t)snprintf(hex + o, sizeof hex - o, "%s%02x", i == 0x3C ? " | " : "", b[i]);
        LOG("[sw2] %s: clock %u ms, %llu reports lost so far; buttons 0x%08x sticks %u,%u %u,%u accel %d,%d,%d gyro %d,%d,%d battery %u mV mouse %d,%d %u,%u; %zu bytes, 0x18..0x2f %s", s2_kind(_pid), r.clock_ms, (unsigned long long)_lost, r.buttons, r.lx, r.ly, r.rx, r.ry, r.raw[0], r.raw[1], r.raw[2], r.raw[3], r.raw[4], r.raw[5], r.battery_mv, r.mouse_x, r.mouse_y, r.mouse_q, r.mouse_s, len, hex);
    }
    if (g_front.read) g_front.read(_pad);
    int player = s2_player(_pad), blocked = s2_blocked();
    [self playerSync];
    if (g_motion && !blocked && player >= 0 && s2_pad_motion_dev(&_pad->st, g_motion_right) == dev) {
        const int8_t *map = g_map_set ? g_map : s2_pad_map(&_pad->st, dev);
        float acc[3], rot[3]; s2_motion(r.raw, map, _rest.have ? _rest.bias : NULL, g_gyro, acc, rot);
        dm_motion_set_hid(player, acc, rot, ts);
    }
    if (_pid == S2_PID_PRO || !g_joy || player < 0) return;
    int right = _pid == S2_PID_JOYCON_R;
    if (!blocked) {   // the Joy-Con's own buttons, stick and sensor, in its own frame (upright: the pair's map)
        int32_t st[2]; s2_joy_stick(dev, right, g_dead, st);
        dm_joy_set(player, right, s2_joy_buttons(r.buttons, right), st[0], st[1]);
        if (g_motion) { float acc[3], rot[3]; s2_motion(r.raw, g_map_set ? g_map : s2_map_pair, _rest.have ? _rest.bias : NULL, g_gyro, acc, rot); dm_motion_set_joy(player, right, acc, rot, ts); }
        // The mouse sensor, once it is on and reporting: the movement since the last report, none while lifted (the counters drift with no surface under them)
        if (_mouseOn && s2_mouse_present(&r)) {
            int on = s2_mouse_on_surface(&r);
            int32_t dx = _mouseSeen && on ? s2_mouse_delta(r.mouse_x, _mouseX) : 0, dy = _mouseSeen && on ? s2_mouse_delta(r.mouse_y, _mouseY) : 0;
            dm_joy_mouse(player, right, dx, dy, DM_JOYMOUSE_CONNECTED | (on ? DM_JOYMOUSE_ON_SURFACE : 0));
        }
    }
    _mouseSeen = _mouseOn && s2_mouse_present(&r); _mouseX = r.mouse_x; _mouseY = r.mouse_y;
    [self gesture:r.buttons right:right];
}
// SL + SR held on a Joy-Con of a pair: it plays alone. L on a left one alone and R on a right one alone, held together: they pair (switch2_proto.h, s2_gesture)
- (void)gesture:(uint32_t)b right:(int)right {
    s2_pad *p = _pad; int paired = p->st.left.have && p->st.right.have;
    if (!s2_hold_update(&_hold, s2_gesture(b, right, paired), now_ns()) || _hold.done) return;
    if (_hold.kind == S2_GESTURE_SPLIT) { _hold.done = 1; [self leavePair]; return; }
    for (S2Session *o in g_sessions.allValues) {   // the partner: the other side, alone, holding its own L or R as long
        if (o == self || !o->_live || !o->_pad || o->_pid == _pid || o->_pid == S2_PID_PRO || o->_pad->st.left.have == o->_pad->st.right.have) continue;
        if (o->_hold.kind != S2_GESTURE_JOIN || o->_hold.done || now_ns() - o->_hold.since < S2_GESTURE_NS) continue;
        _hold.done = o->_hold.done = 1;
        S2Session *mover = s2_player(_pad) > s2_player(o->_pad) ? self : o, *stay = mover == self ? o : self;
        [mover joinPadOf:stay];
        return;
    }
}
- (void)leavePair {
    s2_pad *old = _pad; int was = s2_player(old), side = _pid == S2_PID_JOYCON_R;
    s2_pad *p = calloc(1, sizeof *p); if (!p) return;
    snprintf(p->name, sizeof p->name, "%s", "Joy-Con 2");
    s2_dev *d = s2_dev_of(old, _pid), *nd = s2_dev_of(p, _pid); *nd = *d; memset(d, 0, sizeof *d);
    s2_joy_release(was, side); s2_sync_sides(old); if (g_front.read) g_front.read(old);
    p->next = g_pads; g_pads = p; _pad = p;
    if (!g_front.attach || !g_front.attach(p)) {   // no free player: back into the pair
        LOG("[sw2] %s: no free player to play alone, it stays in the pair", s2_kind(_pid));
        s2_pad_unlink(p); *d = *nd; _pad = old; free(p); s2_sync_sides(old); return;
    }
    s2_sync_sides(p); _player = s2_player(p);
    if (g_front.read) g_front.read(p);
    [self playerSync];   // waiting for a press, the held SL + SR are that press: read() made it a player just now
    if (_player < 0) LOG("[sw2] %s plays alone once a button is pressed (SL + SR held); the other Joy-Con stays player %d", s2_kind(_pid), was + 1);
    else LOG("[sw2] %s plays alone now: player %d (SL + SR held); the other Joy-Con stays player %d", s2_kind(_pid), _player + 1, was + 1);
    [self setLEDs:nil];
}
- (void)joinPadOf:(S2Session *)stay {
    s2_pad *old = _pad, *into = stay->_pad; int was = s2_player(old), side = _pid == S2_PID_JOYCON_R;
    s2_dev *d = s2_dev_of(old, _pid), *nd = s2_dev_of(into, _pid); *nd = *d; memset(d, 0, sizeof *d);
    s2_joy_release(was, side);
    if (was >= 0) { dm_motion_clear_hid(was); if (g_joy) dm_joy_attach(was, 0); }
    s2_pad_unlink(old); if (g_front.detach) g_front.detach(old); free(old);
    _pad = into; s2_sync_sides(into); if (g_front.read) g_front.read(into);
    LOG("[sw2] %s and %s play as one pair now: player %d (L + R held)", s2_kind(stay->_pid), s2_kind(_pid), s2_player(into) + 1);
    [self setLEDs:nil];
}
// ---- CBPeripheralDelegate ----
- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)e {
    if (e) { [self fail:[NSString stringWithFormat:@"service discovery: %@", e.localizedDescription]]; return; }
    for (CBService *s in p.services) [p discoverCharacteristics:nil forService:s];
}
- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)s error:(NSError *)e {
    if (e) { [self fail:[NSString stringWithFormat:@"characteristic discovery: %@", e.localizedDescription]]; return; }
    CBUUID *wr = [CBUUID UUIDWithString:@S2_UUID_CMD_WR], *rs = [CBUUID UUIDWithString:@S2_UUID_CMD_RSP], *in = [CBUUID UUIDWithString:@S2_UUID_INPUT];
    CBUUID *vr = [CBUUID UUIDWithString:@S2_UUID_VIB_JOYCON_R], *vl = [CBUUID UUIDWithString:@S2_UUID_VIB_JOYCON_L], *vp = [CBUUID UUIDWithString:@S2_UUID_VIB_PRO];
    for (CBCharacteristic *c in s.characteristics) {
        if ([c.UUID isEqual:wr]) _wr = c; else if ([c.UUID isEqual:rs]) _rsp = c; else if ([c.UUID isEqual:in]) _inp = c;
        else if ([c.UUID isEqual:vr] || [c.UUID isEqual:vl] || [c.UUID isEqual:vp]) _vib = c;
    }
    if (_wr && _rsp && _inp && !_handshaken && !_live) [p setNotifyValue:YES forCharacteristic:_rsp];
}
- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)e {
    if (e) { [self fail:[NSString stringWithFormat:@"notifications: %@", e.localizedDescription]]; return; }
    if (c == _rsp && !_live) [self handshake];
}
- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)e {
    if (e || !c.value.length) return;
    if (c == _inp) { [self report:c.value]; return; }
    if (c == _rsp && _pending && s2_reply_is(c.value.bytes, c.value.length, _pendingCmd)) {
        _pending = NO; void (^d)(NSData *) = _cb; _cb = nil;
        if (d) d(c.value);
        [self pump];
    }
}
- (void)peripheral:(CBPeripheral *)p didModifyServices:(NSArray<CBService *> *)invalidated { (void)invalidated; [self fail:@"the controller reset the link"]; }
@end

static void s2_session_closed(S2Session *s) {
    BOOL listed = s.pad != NULL;   // only a controller with a pad was ever in the tester's list
    [s teardown]; [g_sessions removeObjectForKey:s.peripheral.identifier]; s2_rumble_timer();
    if (listed && g_changed) g_changed();   // a controller the tester may have listed is gone
}

@interface S2Central : NSObject <CBCentralManagerDelegate>
@end
@implementation S2Central {
    NSMutableDictionary<NSUUID *, NSDate *> *_tried;   // a controller whose connection failed is not tried again at every advertisement
    NSMutableSet<NSUUID *> *_told;                       // a controller in pairing mode that cannot be paired is logged once
}
- (void)scan:(CBCentralManager *)c {
    [c scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];   // a controller's wake is advertised for a short while: every advertisement counts
    LOG("[sw2] Switch 2 controllers: press a button on a paired one; to pair one, hold its Sync button until the player lights sweep");
}
- (void)centralManagerDidUpdateState:(CBCentralManager *)c {
    if (c.state == CBManagerStatePoweredOn) {
#if TARGET_OS_OSX
        static BOOL asked;
        if (!g_host_known && !asked) {   // this Mac's address, from the Bluetooth host controller once the app may use Bluetooth (off the main queue: the first call can take seconds); the scan starts after it
            asked = YES; __weak CBCentralManager *wc = c;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                const char *addr = IOBluetoothHostController.defaultController.addressAsString.UTF8String;   // "aa-bb-cc-dd-ee-ff"
                unsigned a[6]; BOOL ok = addr && sscanf(addr, "%x%*c%x%*c%x%*c%x%*c%x%*c%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6 && (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]);
                uint8_t le[6]; for (int i = 0; i < 6; i++) le[i] = ok ? (uint8_t)a[5 - i] : 0;
                NSData *leData = [NSData dataWithBytes:le length:6];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (ok) { memcpy(g_host, leData.bytes, 6); g_host_known = 1; }
                    else LOG("[sw2] this Mac's Bluetooth address could not be read: controllers are used without pairing (DM_SW2_HOST=aa:bb:cc:dd:ee:ff names it)");
                    CBCentralManager *cm = wc; if (cm && cm.state == CBManagerStatePoweredOn) [self scan:cm];
                });
            });
            return;
        }
#endif
        [self scan:c];
    } else if (c.state == CBManagerStateUnauthorized) LOG("[sw2] Bluetooth is not allowed for this app (System Settings > Privacy & Security > Bluetooth)");
    else if (c.state == CBManagerStatePoweredOff) {
        LOG("[sw2] Bluetooth is off");
        for (S2Session *s in [g_sessions.allValues copy]) s2_session_closed(s);   // no link is left: a save on one ends at once (not after every command's timeout) and the controller leaves the list
    }
}
- (void)centralManager:(CBCentralManager *)c didDiscoverPeripheral:(CBPeripheral *)p advertisementData:(NSDictionary<NSString *, id> *)ad RSSI:(NSNumber *)rssi {
    NSData *d = ad[CBAdvertisementDataManufacturerDataKey];
    if (d.length < 18 || s2_u16(d.bytes) != S2_COMPANY_ID || g_sessions[p.identifier]) return;
    s2_advert a; if (!s2_advert_parse((const uint8_t *)d.bytes + 2, d.length - 2, &a)) return;
    int use = s2_advert_use(&a, g_host_known ? g_host : NULL);
    if (!_tried) { _tried = [NSMutableDictionary new]; _told = [NSMutableSet new]; }
    if (use == S2_ADV_IGNORE) {   // waking toward another host (a Switch 2 nearby): not touched, logged once
        if (![_told containsObject:p.identifier]) { [_told addObject:p.identifier]; LOG("[sw2] a %s woke for another host: left alone (hold its Sync button to pair it with this Mac)", s2_kind(a.pid)); }
        return;
    }
    if (_tried[p.identifier] && -[_tried[p.identifier] timeIntervalSinceNow] < 3) return;
    _tried[p.identifier] = [NSDate date];
    if (use == S2_ADV_TAKE && ![_told containsObject:p.identifier]) {
        [_told addObject:p.identifier];
        LOG("[sw2] this host's Bluetooth address is not known: the %s is used without pairing, and needs Sync again next time", s2_kind(a.pid));
    }
    S2Session *s = [[S2Session alloc] initWith:p pid:a.pid use:use];
    g_sessions[p.identifier] = s;
    LOG("[sw2] %s %s (rssi %d), connecting", use == S2_ADV_WAKE ? "woke:" : "pairing:", s2_kind(a.pid), rssi.intValue);
    [c connectPeripheral:p options:nil];
    NSUUID *id = p.identifier; __weak CBCentralManager *wc = c;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        S2Session *t = g_sessions[id];
        if (t && t.peripheral.state != CBPeripheralStateConnected) { LOG("[sw2] connecting timed out"); [wc cancelPeripheralConnection:t.peripheral]; s2_session_closed(t); }
    });
}
- (void)centralManager:(CBCentralManager *)c didConnectPeripheral:(CBPeripheral *)p { [g_sessions[p.identifier] begin]; }
- (void)centralManager:(CBCentralManager *)c didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
    LOG("[sw2] connect failed: %s", e.localizedDescription.UTF8String ?: "unknown");
    S2Session *s = g_sessions[p.identifier]; if (s) s2_session_closed(s);
}
- (void)centralManager:(CBCentralManager *)c didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)e {
    S2Session *s = g_sessions[p.identifier]; if (!s) return;
    LOG("[sw2] %s disconnected%s%s", s2_kind(s.pid), e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    s2_session_closed(s);
}
@end

void dm_switch2_active(int on) {
    g_active = on;
    if (!on) for (s2_pad *p = g_pads; p; p = p->next) { int pl = s2_player(p); if (pl >= 0) { dm_motion_clear_hid(pl); s2_joy_release(pl, 0); s2_joy_release(pl, 1); } }
}

static S2Central *g_delegate;
static void s2_central_start(void) { g_central = [[CBCentralManager alloc] initWithDelegate:g_delegate queue:nil options:nil]; }
// The rumble tick runs while a controller is connected (attachPad starts it, s2_session_closed stops it with the last one), not for the whole game
static void s2_rumble_timer(void) {
    BOOL any = NO; for (S2Session *s in g_sessions.allValues) if (s.live) { any = YES; break; }
    if (!any) { [g_rumble_timer invalidate]; g_rumble_timer = nil; return; }
    if (g_rumble_timer) return;
    memset(&g_rstat, 0, sizeof g_rstat);
    g_rumble_timer = [NSTimer scheduledTimerWithTimeInterval:S2_RUMBLE_TICK_S repeats:YES block:^(NSTimer *t) {
        (void)t; uint64_t now = now_ns();
        if (g_rstat.last && now - g_rstat.last > g_rstat.maxgap) g_rstat.maxgap = now - g_rstat.last;
        g_rstat.last = now; g_rstat.ticks++;
        for (S2Session *s in g_sessions.allValues) [s rumbleTick];
        if (!g_rstat.t0) g_rstat.t0 = now;
        if (now - g_rstat.t0 >= 1000000000ull) {
            if (g_verbose && g_rstat.sent) LOG("[sw2] rumble: %llu ticks (longest gap %.1f ms), %llu packets, %llu while Bluetooth was not ready", (unsigned long long)g_rstat.ticks, g_rstat.maxgap / 1e6, (unsigned long long)g_rstat.sent, (unsigned long long)g_rstat.busy);
            g_rstat.ticks = g_rstat.sent = g_rstat.busy = g_rstat.maxgap = 0; g_rstat.t0 = now;
        }
    }];
}

float dm_switch2_dead_zone(void) { return g_dead; }
void dm_switch2_set_dead_zone(float counts) { g_dead = counts > 0 ? counts : 0; }

// ---- the stick tester's view (pad_switch2_apple.h): connected controllers whose handshake finished, in the order they connected ----
int dm_switch2_on(void) { return g_on; }
void dm_switch2_observe(void (^changed)(void), void (^report)(uint32_t)) { g_changed = changed; g_report = report; }
int dm_switch2_saving(void) { return g_saving > 0; }
static S2Session *s2_session_by_id(uint32_t id) {
    for (S2Session *s in g_sessions.allValues) if (s.cid == id && s.live && s.pad) return s;
    return nil;
}
int dm_switch2_controllers(dm_sw2_info *out, int max) {
    NSMutableArray<S2Session *> *l = [NSMutableArray new];
    for (S2Session *s in g_sessions.allValues) if (s.live && s.pad) [l addObject:s];   // a torn-down session has no pad
    [l sortUsingComparator:^NSComparisonResult(S2Session *a, S2Session *b) { return a.cid < b.cid ? NSOrderedAscending : a.cid > b.cid ? NSOrderedDescending : NSOrderedSame; }];
    int n = 0;
    for (S2Session *s in l) {
        if (!out || n >= max) break;
        [s fill:&out[n++]];
    }
    return n;
}
void dm_switch2_save(uint32_t id, int slot, s2_cal cal, void (^done)(S2CalOutcome, NSString *)) {
    S2Session *s = s2_session_by_id(id);
    if (!s) {
        LOG("[sw2] nothing saved: controller %u is not connected", id);
        dispatch_async(dispatch_get_main_queue(), ^{ if (done) done(S2CalNoBackup, nil); });
        return;
    }
    [s saveCal:cal slot:slot done:done];
}

int dm_switch2_start(const dm_switch2_front *front) {
    const char *e = getenv("DM_SW2"); if (!e || !atoi(e)) return 0;   // off by default: no Bluetooth client, so no permission prompt
    if (![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"]) {
        LOG("[sw2] Switch 2 controllers are off: this app's Info.plist has no NSBluetoothAlwaysUsageDescription (an app packed with a newer pack_app.py has it)");
        return 0;
    }
    g_front = *front; g_on = 1;
    e = getenv("DM_SW2_VERBOSE"); g_verbose = e && atoi(e);
    e = getenv("DM_SW2_CALIBRATE"); g_calibrate = e && atoi(e); g_calibrated = [NSMutableSet new];
    e = getenv("DM_PAD_MERGE"); g_joy = !(e && atoi(e));   // merged, every pad is player 1: no pad holds a game's Joy-Cons apart
    e = getenv("DM_SW2_MOTION"); g_motion = !e || atoi(e);
    e = getenv("DM_SW2_MOTION_SIDE"); g_motion_right = !(e && (e[0] == 'L' || e[0] == 'l'));
    e = getenv("DM_SW2_MAP"); if (e && !(g_map_set = s2_map_parse(e, g_map))) LOG("[sw2] DM_SW2_MAP is not six numbers of 1..6 (signs allowed), like -1,-2,-3,4,5,6: using the built-in maps");
    e = getenv("DM_SW2_GYRO_DPS"); if (e && atof(e) > 0) g_gyro = (float)atof(e) * 3.14159265358979323846f / 180.f;
    e = getenv("DM_SW2_DEADZONE"); g_dead = s2_dead_counts(e); if (e && *e) LOG("[sw2] dead zone %.0f counts (DM_SW2_DEADZONE)", g_dead);
    g_sessions = [NSMutableDictionary new];
    g_delegate = [S2Central new];
    unsigned v[6]; char sep; e = getenv("DM_SW2_HOST");
    if (e && sscanf(e, "%x%c%x%c%x%c%x%c%x%c%x", &v[0], &sep, &v[1], &sep, &v[2], &sep, &v[3], &sep, &v[4], &sep, &v[5]) == 11) { for (int i = 0; i < 6; i++) g_host[i] = (uint8_t)v[5 - i]; g_host_known = 1; }
    s2_central_start();   // the main queue: the pad callbacks run where the front end's do. On the Mac this Mac's address is read once Bluetooth is allowed (centralManagerDidUpdateState)
    return 1;
}
