// CoreBluetooth transport for Switch 2 controllers, shared by the Mac and iOS front ends (pad_switch2_apple.h says what it does; switch2_proto.h is the protocol).
// Everything runs on the main queue, where the front ends' own pad callbacks run.
#import "pad_switch2_apple.h"
#import <CoreBluetooth/CoreBluetooth.h>
#import <TargetConditionals.h>
#include "dm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <os/lock.h>

static dm_switch2_front g_front;
static s2_pad *g_pads;
static int g_verbose, g_motion_right = 1, g_motion = 1, g_active = 1;
static float g_gyro = S2_GYRO_RAD;
static int8_t g_map[6];
static int g_map_set;               // DM_SW2_MAP overrides the per-layout maps
static uint8_t g_host[6];           // this host's Bluetooth address, little endian, as a bonded controller's wake advertisement carries it
static int g_host_known;

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

@interface S2Session : NSObject <CBPeripheralDelegate>
@property (nonatomic, strong) CBPeripheral *peripheral;
@property (nonatomic) uint16_t pid;
@property (nonatomic) int use;      // S2_ADV_PAIR, S2_ADV_TAKE or S2_ADV_WAKE: why it was connected
@property (nonatomic) s2_pad *pad;
@property (nonatomic) BOOL live;
@end

static NSMutableDictionary<NSUUID *, S2Session *> *g_sessions;
static CBCentralManager *g_central;
static void s2_session_closed(S2Session *s);

static const char *s2_kind(uint16_t pid) { return pid == S2_PID_PRO ? "Pro Controller 2" : pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)"; }
static s2_dev *s2_dev_of(s2_pad *p, uint16_t pid) { return pid == S2_PID_PRO ? &p->st.pro : pid == S2_PID_JOYCON_L ? &p->st.left : &p->st.right; }
static int s2_pad_empty(const s2_pad *p) { return !p->st.pro.have && !p->st.left.have && !p->st.right.have; }
static int s2_player(s2_pad *p) { return p && g_front.player ? g_front.player(p) : -1; }
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
    s2_cal _cal[2];
    NSTimer *_tick; uint64_t _reports; BOOL _handshaken; s2_rest _rest;
    CBCharacteristic *_vib; uint8_t _vseq; int _offLeft; s2_rumble_cursor _rcur;   // _rcur: where this controller is in its player's rumble log
}
- (instancetype)initWith:(CBPeripheral *)p pid:(uint16_t)pid use:(int)use {
    if ((self = [super init])) { _peripheral = p; _pid = pid; _use = use; p.delegate = self; _queue = [NSMutableArray new]; s2_cal_default(&_cal[0]); s2_cal_default(&_cal[1]); }
    return self;
}
- (void)begin {
    [_peripheral discoverServices:nil];
    __weak S2Session *w = self;   // a controller that connects but never finishes the handshake is dropped
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ S2Session *s = w; if (s && !s->_handshaken) [s fail:@"handshake timed out"]; });
}
- (void)fail:(NSString *)why {
    LOG("[sw2] %s: %s", s2_kind(_pid), why.UTF8String);
    [g_central cancelPeripheralConnection:_peripheral];   // didDisconnect cleans up; a session that never connected is dropped there too
    if (!_live) s2_session_closed(self);
}
- (void)teardown {
    [_tick invalidate]; _tick = nil;
    if (_pad) {
        s2_pad *p = _pad; _pad = NULL;
        int player = s2_player(p);
        s2_dev *d = s2_dev_of(p, _pid); memset(d, 0, sizeof *d);
        if (player >= 0) dm_motion_clear_hid(player);
        if (s2_pad_empty(p)) {
            s2_pad **link = &g_pads; while (*link && *link != p) link = &(*link)->next;
            if (*link) *link = p->next;
            if (g_front.detach) g_front.detach(p);
            free(p);
        } else if (g_front.read) g_front.read(p);
    }
}
// ---- commands: one in flight (the controller has one reply channel) ----
- (void)send:(uint8_t)cmd sub:(uint8_t)sub payload:(const uint8_t *)pl len:(size_t)n done:(void (^)(NSData *))done {
    S2Command *c = [S2Command new]; c.cmd = cmd; c.sub = sub; c.payload = [NSData dataWithBytes:pl length:n]; c.done = done;
    [_queue addObject:c]; [self pump];
}
- (void)pump {
    if (_pending || !_queue.count || !_wr) return;
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
- (void)readCal:(int)slot done:(void (^)(void))done {   // the user's calibration when set, else the factory's
    __weak S2Session *w = self;
    uint32_t user = slot ? S2_ADDR_USER_STICK2 : S2_ADDR_USER_STICK1, factory = slot ? S2_ADDR_FACTORY_STICK2 : S2_ADDR_FACTORY_STICK1;
    [self readMemory:0x0B at:user done:^(NSData *u) {
        S2Session *s = w; if (!s) return;
        if (u && s2_cal_parse(u.bytes, u.length, &s->_cal[slot])) { done(); return; }
        [s readMemory:0x0B at:factory done:^(NSData *f) { S2Session *s2 = w; if (!s2) return; if (f) s2_cal_parse(f.bytes, f.length, &s2->_cal[slot]); done(); }];
    }];
}
- (void)setLEDs:(void (^)(void))done {
    int p = s2_player(_pad); uint8_t pl[4] = {s2_led_pattern[p >= 0 && p < 8 ? p : 0], 0, 0, 0};
    [self send:S2_CMD_LEDS sub:S2_SUB_LEDS_SET payload:pl len:4 done:^(NSData *r) { (void)r; if (done) done(); }];
}
- (void)handshake {
    __weak S2Session *w = self;
    [self readMemory:0x40 at:S2_ADDR_INFO done:^(NSData *info) {
        S2Session *s = w; if (!s) return;
        uint16_t vid, pid; char serial[15];
        if (!info || !s2_info_parse(info.bytes, info.length, &vid, &pid, serial) || vid != S2_VENDOR_ID) { [s fail:@"the controller did not answer the identity read"]; return; }
        if (pid != s->_pid) LOG("[sw2] advertised 0x%04x but identifies as 0x%04x", s->_pid, pid);
        s->_pid = pid;
        void (^calibrate)(void) = ^{ S2Session *a = w; if (!a) return;
            [a readCal:0 done:^{ S2Session *b = w; if (!b) return;
                void (^next)(void) = ^{ S2Session *c = w; if (!c) return; [c finish]; };
                if (b->_pid == S2_PID_PRO) [b readCal:1 done:next]; else next();
            }];
        };
        if (s->_use == S2_ADV_PAIR) [s bond:calibrate]; else calibrate();
    }];
}
// Sync is held on this controller (its advertisement named no host): give it this host's address and fresh keys, as the console does (switch2_proto.h, Bonding)
- (void)bond:(void (^)(void))done {
    __weak S2Session *w = self;
    uint8_t host[S2_PAIR_HOST_LEN], k1[16], k2[16], key1[S2_PAIR_KEY_LEN], key2[S2_PAIR_KEY_LEN], last = 0;
    arc4random_buf(k1, sizeof k1); arc4random_buf(k2, sizeof k2);
    s2_pair_host_payload(host, g_host); s2_pair_key_payload(key1, k1); s2_pair_key_payload(key2, k2);
    NSData *z = [NSData dataWithBytes:&last length:1], *h = [NSData dataWithBytes:host length:sizeof host], *a = [NSData dataWithBytes:key1 length:sizeof key1], *b = [NSData dataWithBytes:key2 length:sizeof key2];
    void (^step)(NSData *, const char *, void (^)(void)) = ^(NSData *r, const char *what, void (^next)(void)) {
        S2Session *s = w; if (!s) return;
        if (!r) { LOG("[sw2] %s: pairing stopped at %s (no answer): it plays now, but needs Sync again next time", s2_kind(s->_pid), what); done(); return; }
        next();
    };
    [self send:S2_CMD_PAIR sub:S2_SUB_PAIR_HOST payload:h.bytes len:h.length done:^(NSData *r1) { step(r1, "the host address", ^{
        S2Session *s = w; if (!s) return;
        [s send:S2_CMD_PAIR sub:S2_SUB_PAIR_KEY1 payload:a.bytes len:a.length done:^(NSData *r2) { step(r2, "the first key", ^{
            S2Session *t = w; if (!t) return;
            [t send:S2_CMD_PAIR sub:S2_SUB_PAIR_KEY2 payload:b.bytes len:b.length done:^(NSData *r3) { step(r3, "the second key", ^{
                S2Session *u = w; if (!u) return;
                [u send:S2_CMD_PAIR sub:S2_SUB_PAIR_DONE payload:z.bytes len:z.length done:^(NSData *r4) { step(r4, "the last step", ^{
                    S2Session *v = w; if (!v) return;
                    LOG("[sw2] %s paired with this Mac: from now on a press of any of its buttons connects it, no Sync needed", s2_kind(v->_pid));
                    done();
                }); }];
            }); }];
        }); }];
    }); }];
}
- (void)finish {
    __weak S2Session *w = self;
    uint8_t pl[4] = {S2_FEATURES, 0, 0, 0};
    [self send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_SET payload:pl len:4 done:^(NSData *r1) {
        S2Session *s = w; if (!s) return; (void)r1;
        uint8_t on[4] = {S2_FEATURES, 0, 0, 0};
        [s send:S2_CMD_FEATURE sub:S2_SUB_FEATURE_ENABLE payload:on len:4 done:^(NSData *r2) {
            S2Session *t = w; if (!t) return;
            if (!r2) { [t fail:@"enabling the features was refused"]; return; }
            [t attachPad];
        }];
    }];
}
- (void)attachPad {
    s2_pad *p = s2_pad_for(_pid);
    if (!p) { [self fail:@"out of memory"]; return; }
    BOOL fresh = s2_pad_empty(p);
    s2_dev *d = s2_dev_of(p, _pid); memset(d, 0, sizeof *d);
    d->have = 1; d->pid = _pid; d->cal[0] = _cal[0]; d->cal[1] = _cal[1];
    _pad = p; _live = YES; _handshaken = YES;
    if (fresh) {
        if (g_front.attach) g_front.attach(p);
        if (s2_player(p) < 0) { LOG("[sw2] %s: no free player, not used", s2_kind(_pid)); [self fail:@"all players are taken"]; return; }
    } else if (g_front.read) g_front.read(p);
    LOG("[sw2] %s connected: player %d%s", s2_kind(_pid), s2_player(p) + 1, fresh ? "" : " (joined the Joy-Con already there)");
    [self setLEDs:nil];
    [_peripheral setNotifyValue:YES forCharacteristic:_inp];
    __weak S2Session *w = self;   // the link drops after about ten seconds without a write from the host, so the LEDs are written once a second
    _tick = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        S2Session *s = w; if (!s) { [t invalidate]; return; }
        int pl = s2_player(s->_pad); uint8_t l[4] = {s2_led_pattern[pl >= 0 && pl < 8 ? pl : 0], 0, 0, 0}; uint8_t f[12];
        size_t n = s2_command(f, sizeof f, S2_CMD_LEDS, S2_SUB_LEDS_SET, l, 4);
        if (s->_wr && n) [s->_peripheral writeValue:[NSData dataWithBytes:f length:n] forCharacteristic:s->_wr type:CBCharacteristicWriteWithoutResponse];
    }];
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
    s2_rest_update(&_rest, r.raw);
    if (g_verbose && _reports % 33 == 1)
        LOG("[sw2] %s: buttons 0x%08x sticks %u,%u %u,%u accel %d,%d,%d gyro %d,%d,%d battery %u mV", s2_kind(_pid), r.buttons, r.lx, r.ly, r.rx, r.ry, r.raw[0], r.raw[1], r.raw[2], r.raw[3], r.raw[4], r.raw[5], r.battery_mv);
    if (g_front.read) g_front.read(_pad);
    int player = s2_player(_pad);
    if (g_motion && g_active && player >= 0 && s2_pad_motion_dev(&_pad->st, g_motion_right) == dev) {
        const int8_t *map = g_map_set ? g_map : s2_pad_map(&_pad->st, dev);
        float acc[3], rot[3]; s2_motion(r.raw, map, _rest.have ? _rest.bias : NULL, g_gyro, acc, rot);
        dm_motion_set_hid(player, acc, rot, now_ns());
    }
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

static void s2_session_closed(S2Session *s) { [s teardown]; [g_sessions removeObjectForKey:s.peripheral.identifier]; }

@interface S2Central : NSObject <CBCentralManagerDelegate>
@end
@implementation S2Central {
    NSMutableDictionary<NSUUID *, NSDate *> *_tried;   // a controller whose connection failed is not tried again at every advertisement
    NSMutableSet<NSUUID *> *_told;                       // a controller in pairing mode that cannot be paired is logged once
}
- (void)centralManagerDidUpdateState:(CBCentralManager *)c {
    if (c.state == CBManagerStatePoweredOn) {
        [c scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];   // a controller's wake is advertised for a short while: every advertisement counts
        LOG("[sw2] Switch 2 controllers: press a button on a paired one; to pair one, hold its Sync button until the player lights sweep");
    } else if (c.state == CBManagerStateUnauthorized) LOG("[sw2] Bluetooth is not allowed for this app (System Settings > Privacy & Security > Bluetooth)");
    else if (c.state == CBManagerStatePoweredOff) LOG("[sw2] Bluetooth is off");
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
    if (!on) for (s2_pad *p = g_pads; p; p = p->next) { int pl = s2_player(p); if (pl >= 0) dm_motion_clear_hid(pl); }
}

static S2Central *g_delegate;
static void s2_central_start(void) {
    g_central = [[CBCentralManager alloc] initWithDelegate:g_delegate queue:nil options:nil];
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

int dm_switch2_start(const dm_switch2_front *front) {
    const char *e = getenv("DM_SW2"); if (!e || !atoi(e)) return 0;   // off by default: no Bluetooth client, so no permission prompt
    if (![NSBundle.mainBundle objectForInfoDictionaryKey:@"NSBluetoothAlwaysUsageDescription"]) {
        LOG("[sw2] Switch 2 controllers are off: this app's Info.plist has no NSBluetoothAlwaysUsageDescription (an app packed with a newer pack_app.py has it)");
        return 0;
    }
    g_front = *front;
    e = getenv("DM_SW2_VERBOSE"); g_verbose = e && atoi(e);
    e = getenv("DM_SW2_MOTION"); g_motion = !e || atoi(e);
    e = getenv("DM_SW2_MOTION_SIDE"); g_motion_right = !(e && (e[0] == 'L' || e[0] == 'l'));
    e = getenv("DM_SW2_MAP"); if (e && !(g_map_set = s2_map_parse(e, g_map))) LOG("[sw2] DM_SW2_MAP is not six numbers of 1..6 (signs allowed), like -1,-2,-3,4,5,6: using the built-in maps");
    e = getenv("DM_SW2_GYRO_DPS"); if (e && atof(e) > 0) g_gyro = (float)atof(e) * 3.14159265358979323846f / 180.f;
    g_sessions = [NSMutableDictionary new];
    g_delegate = [S2Central new];
    unsigned v[6]; char sep; e = getenv("DM_SW2_HOST");
    if (e && sscanf(e, "%x%c%x%c%x%c%x%c%x%c%x", &v[0], &sep, &v[1], &sep, &v[2], &sep, &v[3], &sep, &v[4], &sep, &v[5]) == 11) { for (int i = 0; i < 6; i++) g_host[i] = (uint8_t)v[5 - i]; g_host_known = 1; }
#if TARGET_OS_OSX
    if (!g_host_known) {   // the system's Bluetooth report names this Mac's address (a second or so: off the main queue; the scan starts once it is known)
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/system_profiler"]; t.arguments = @[@"SPBluetoothDataType"];
            NSPipe *pipe = [NSPipe pipe]; t.standardOutput = pipe; t.standardError = [NSFileHandle fileHandleWithNullDevice];
            NSData *out = [t launchAndReturnError:nil] ? [pipe.fileHandleForReading readDataToEndOfFile] : nil; [t waitUntilExit];
            NSString *txt = out ? [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding] : nil;
            NSRange r = txt ? [txt rangeOfString:@"Address: "] : NSMakeRange(NSNotFound, 0);
            const char *addr = r.location != NSNotFound && txt.length >= r.location + r.length + 17 ? [txt substringWithRange:NSMakeRange(r.location + r.length, 17)].UTF8String : NULL;
            unsigned a[6] = {0}; BOOL ok = addr && sscanf(addr, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6;
            uint8_t le[6]; for (int i = 0; i < 6; i++) le[i] = (uint8_t)a[5 - i];
            NSData *leData = [NSData dataWithBytes:le length:6];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (ok) { memcpy(g_host, leData.bytes, 6); g_host_known = 1; }
                else LOG("[sw2] this Mac's Bluetooth address could not be read: controllers are used without pairing (DM_SW2_HOST=aa:bb:cc:dd:ee:ff names it)");
                s2_central_start();
            });
        });
        return 1;
    }
#endif
    s2_central_start();   // the main queue: the pad callbacks run where the front end's do
    return 1;
}
