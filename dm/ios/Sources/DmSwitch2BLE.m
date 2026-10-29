// Native Nintendo Switch 2 Pro Controller support for the iOS front end, over CoreBluetooth.
//
// iOS has no system-level support for a Switch 2 controller: it is not a HID device and GameController.framework never sees it. The controller
// speaks Nintendo's proprietary BLE GATT protocol instead, decoded by the Mac bridge "Finally the Controller Works" by Peter Sharma
// (https://github.com/Peterksharma/switch2mac): input notifications AB7DE9BE…, commands on 649D4AC9…, replies on C765A961…, rumble on
// CC483F51…. The GATT identifiers, command framing, report layout, calibration and rumble packing are a port of that project's published
// notes and Swift implementation, which credits the wider Switch 2 reverse-engineering community (ndeadly, trevlars' MIT Linux bridge,
// Nadeflore/Switch2Connect and others). The installed 0.2.0 binary was checked in Ghidra for the same constants. DmSwitch2BLE is a CoreBluetooth
// central that performs the handshake, decodes input reports and feeds the same pad/motion/haptics paths as a GameController controller
// (the slot table in DmGamepad.m, hle_hid.c). The game never learns the difference.
//
// Scope and limits, all observed or explicitly marked as inferred:
//  * Pro Controller 2 (product id 0x2069) only. Joy-Con 2 halves and the NSO GameCube pad share the protocol but need other stick/trigger rules;
//    they are logged once and ignored rather than half-supported.
//  * No SMP and no application-layer bonding: the controller drops links that attempt SMP, and iOS exposes no public host Bluetooth address for
//    command 0x15. So the controller is not taught to wake toward the iPhone. Hold Sync to advertise in pairing mode, or press a button while the
//    game is open and scanning. A controller bonded to a Mac/Switch does not wake for the iPhone. DM_SW2_BOND_MAC=aa:bb:cc:dd:ee:ff sends the four
//    0x15 commands for a host address the caller knows (off by default; there is no public way to read the iPhone's own address).
//  * A 1 Hz player-LED refresh is the keep-alive: on Apple hosts the link is dropped after ~10-17 s without a host write even while reports arrive.
//  * Motion is decoded but published only with DM_SW2_MOTION=1: the Switch 2 IMU's raw counts and axis order are not verified on hardware, and a
//    wrong guess would fight a game's camera. DM_SW2_RUMBLE=0 turns HD rumble off; DM_SW2_DEADZONE=<counts> overrides the 40-count centre deadzone.
//  * iOS Bluetooth permission: NSBluetoothAlwaysUsageDescription in project.yml. Without it the system kills the app on the first scan.
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CoreBluetooth/CoreBluetooth.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#include "dm_input.h"
#include "hle_haptics.h"
#include "dm_switch2.h"
#include "dm_switch2_proto.h"

#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// Implemented in DmGamepad.m: the shared player-slot table, so a Switch 2 controller competes for players with GameController controllers.
extern int dm_pad_external_claim(const void *owner, const char *name);
extern void dm_pad_external_release(const void *owner);

// Implemented in haptics_apple.m: hand one player's game vibration to a controller that is not a GameController device.
extern void dm_haptics_apple_set_ble(int player, void (*sink)(const dm_haptic_state *));

static uint64_t ds2_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

typedef NS_ENUM(NSInteger, DS2Stage) {
    DS2StageIdle = 0, DS2StageServices, DS2StageInfo, DS2StageCalibration, DS2StageFeatures, DS2StageInput, DS2StageReady
};

@class DmSwitch2Session;

@interface DmSwitch2Session : NSObject <CBPeripheralDelegate>
@property (nonatomic, readonly) CBPeripheral *peripheral;
@property (nonatomic) DS2Stage stage;
@property (nonatomic) int slot;                       // 0-based; the player is slot + 1
@property (nonatomic) BOOL done;                      // handshake finished: reports drive the pad
@property (nonatomic, copy) NSString *serial;
@property (nonatomic) NSUInteger reports;
- (instancetype)initWithPeripheral:(CBPeripheral *)p queue:(dispatch_queue_t)q;
- (void)beginHandshake;
- (void)teardown;
- (void)rumbleFrom:(const dm_haptic_state *)s;         // any thread; stores targets only
@end

static NSLock *gRumbleLock;
static DmSwitch2Session *gRumbleSlot[DM_PLAYERS];

// The haptics hook is a plain C function pointer with no context, so the session is found through the per-player registry.
static void ds2_rumble_dispatch(const dm_haptic_state *s) {
    if (!s || s->player < 0 || s->player >= DM_PLAYERS) return;
    [gRumbleLock lock];
    DmSwitch2Session *session = gRumbleSlot[s->player];
    [gRumbleLock unlock];
    [session rumbleFrom:s];
}

@interface DmSwitch2BLE : NSObject <CBCentralManagerDelegate>
+ (instancetype)shared;
- (void)start;
- (void)sessionEnded:(DmSwitch2Session *)s;
- (void)sessionReady:(DmSwitch2Session *)s;
@end

// ---- One controller ----

@interface DS2Command : NSObject
@property (nonatomic) uint8_t cmd, sub;
@property (nonatomic, copy) NSData *payload;
@property (nonatomic, copy) void (^completion)(NSData *);
@end
@implementation DS2Command
@end

@implementation DmSwitch2Session {
    dispatch_queue_t _q;
    NSMutableDictionary<NSString *, CBCharacteristic *> *_chars;
    NSMutableArray<DS2Command *> *_commands;    // one in flight, the rest wait (the controller has one reply channel)
    uint8_t _pendingID;
    BOOL _pending;
    void (^_completion)(NSData *);
    uint64_t _generation;
    dispatch_source_t _tick;
    NSData *_bondMAC;
    ds2_cal _cal[2];
    float _deadzone;
    BOOL _motion, _rumble, _verbose;
    uint8_t _rumbleSeq;
    uint64_t _lastWrite, _lastReport;
    _Atomic int _lfAmp, _lfHz, _hfAmp, _hfHz;       // written by the haptics thread
    uint64_t _rumbleHoldUntil;
}

- (instancetype)initWithPeripheral:(CBPeripheral *)p queue:(dispatch_queue_t)q {
    if ((self = [super init])) {
        _peripheral = p; p.delegate = self;
        _q = q;
        _slot = -1; _stage = DS2StageIdle;
        _chars = [NSMutableDictionary new];
        _commands = [NSMutableArray new];
        for (int i = 0; i < 2; i++) ds2_cal_default(&_cal[i]);
        const char *e = getenv("DM_SW2_MOTION"); _motion = e && atoi(e);
        e = getenv("DM_SW2_RUMBLE"); _rumble = !e || atoi(e);
        e = getenv("DM_SW2_DEADZONE"); _deadzone = e ? (float)atof(e) : 40.f;
        e = getenv("DM_SW2_VERBOSE"); _verbose = e && atoi(e);
        e = getenv("DM_SW2_BOND_MAC");
        if (e && *e) {
            unsigned v[6]; char sep;
            if (sscanf(e, "%x%c%x%c%x%c%x%c%x%c%x", &v[0], &sep, &v[1], &sep, &v[2], &sep, &v[3], &sep, &v[4], &sep, &v[5]) == 11) {
                uint8_t mac[6]; for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[5 - i];   // the protocol carries it little-endian
                _bondMAC = [NSData dataWithBytes:mac length:6];
            } else LOG("switch2: DM_SW2_BOND_MAC is not aa:bb:cc:dd:ee:ff, bonding skipped");
        }
    }
    return self;
}

- (CBCharacteristic *)ch:(const char *)uuid { return _chars[[NSString stringWithUTF8String:uuid]]; }

- (void)beginHandshake {
    LOG("switch2: %s connected, discovering services", (self.peripheral.name ?: @"controller").UTF8String);
    self.stage = DS2StageServices;
    [self.peripheral discoverServices:nil];
    // A controller that accepts the link but never answers must not hold the only scan slot: give up and rescan.
    __weak DmSwitch2Session *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20.0 * NSEC_PER_SEC)), _q, ^{
        DmSwitch2Session *s = weak;
        if (s && !s.done) [s fail:@"handshake timed out"];
    });
}

- (void)teardown {
    if (_tick) { dispatch_source_cancel(_tick); _tick = nil; }
    if (self.slot >= 0) {
        if (_motion) dm_motion_clear_hid(self.slot);
        [gRumbleLock lock]; gRumbleSlot[self.slot] = nil; [gRumbleLock unlock];
        dm_haptics_apple_set_ble(self.slot, NULL);
        dm_pad_external_release((__bridge const void *)self);
        self.slot = -1;
    }
    self.done = NO;
}

// ---- Commands ----

- (void)command:(uint8_t)cmd sub:(uint8_t)sub payload:(NSData *)payload completion:(void (^)(NSData *))completion {
    DS2Command *c = [DS2Command new];
    c.cmd = cmd; c.sub = sub; c.payload = payload ?: [NSData data]; c.completion = completion;
    [_commands addObject:c];
    [self pump];
}

- (void)pump {
    if (_pending || !_commands.count) return;
    CBCharacteristic *w = [self ch:DS2_UUID_CMD_WR];
    if (!w) {
        while (_commands.count) { DS2Command *c = _commands.firstObject; [_commands removeObjectAtIndex:0]; if (c.completion) c.completion(nil); }
        return;
    }
    DS2Command *c = _commands.firstObject; [_commands removeObjectAtIndex:0];
    uint8_t frame[8 + 255];
    size_t n = ds2_command(frame, sizeof frame, c.cmd, c.sub, c.payload.bytes, c.payload.length);
    if (!n) { if (c.completion) c.completion(nil); [self pump]; return; }
    _pendingID = c.cmd; _pending = YES; _completion = c.completion;
    uint64_t gen = ++_generation;
    __weak DmSwitch2Session *weak = self;
    uint8_t id = c.cmd;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), _q, ^{
        DmSwitch2Session *s = weak;
        if (!s || !s->_pending || s->_generation != gen) return;
        s->_pending = NO;
        void (^done)(NSData *) = s->_completion; s->_completion = nil;
        LOG("switch2: command 0x%02x timed out", id);
        if (done) done(nil);
        [s pump];
    });
    [self.peripheral writeValue:[NSData dataWithBytes:frame length:n] forCharacteristic:w type:CBCharacteristicWriteWithoutResponse];
    _lastWrite = ds2_now();
}

- (void)readMemory:(uint8_t)len at:(uint32_t)addr completion:(void (^)(NSData *))completion {
    uint8_t p[8]; ds2_memory_payload(p, len, addr);
    [self command:DS2_CMD_MEMORY sub:DS2_SUB_MEMORY_READ payload:[NSData dataWithBytes:p length:8] completion:^(NSData *rsp) {
        // The reply payload is len, 0x7E, 0, 0, address(4), 0, 0, 0, data.
        if (!rsp || rsp.length < 8 + (NSUInteger)len || ((const uint8_t *)rsp.bytes)[0] != len) { completion(nil); return; }
        completion([rsp subdataWithRange:NSMakeRange(8, len)]);
    }];
}

- (void)setPlayerLEDs:(void (^)(BOOL))done {
    int player = self.slot + 1;
    uint8_t pattern = player >= 1 && player <= 8 ? ds2_led_pattern[player - 1] : 0x01;
    uint8_t p[4] = {pattern, 0, 0, 0};
    [self command:DS2_CMD_LEDS sub:DS2_SUB_LEDS_SET payload:[NSData dataWithBytes:p length:4] completion:^(NSData *r) { if (done) done(r != nil); }];
}

- (void)setFeature:(uint8_t)sub completion:(void (^)(BOOL))done {
    uint8_t p[4] = {DS2_FEATURES_PRO, 0, 0, 0};
    [self command:DS2_CMD_FEATURE sub:sub payload:[NSData dataWithBytes:p length:4] completion:^(NSData *r) { if (done) done(r != nil); }];
}

- (void)bond {
    if (!_bondMAC) return;
    uint8_t set[14] = {0x00, 0x02};
    [_bondMAC getBytes:set + 2 length:6]; [_bondMAC getBytes:set + 8 length:6];
    static const uint8_t ltk1[17] = {0x00, 0xEA, 0xBD, 0x47, 0x13, 0x89, 0x35, 0x42, 0xC6, 0x79, 0xEE, 0x07, 0xF2, 0x53, 0x2C, 0x6C, 0x31};
    static const uint8_t ltk2[17] = {0x00, 0x40, 0xB0, 0x8A, 0x5F, 0xCD, 0x1F, 0x9B, 0x41, 0x12, 0x5C, 0xAC, 0xC6, 0x3F, 0x38, 0xA0, 0x73};
    [self command:DS2_CMD_PAIR sub:0x01 payload:[NSData dataWithBytes:set length:14] completion:^(NSData *r1) {
        if (!r1) { LOG("switch2: bond: host address refused"); return; }
        [self command:DS2_CMD_PAIR sub:0x04 payload:[NSData dataWithBytes:ltk1 length:17] completion:^(NSData *r2) {
            if (!r2) { LOG("switch2: bond: LTK1 refused"); return; }
            [self command:DS2_CMD_PAIR sub:0x02 payload:[NSData dataWithBytes:ltk2 length:17] completion:^(NSData *r3) {
                if (!r3) { LOG("switch2: bond: LTK2 refused"); return; }
                uint8_t fin = 0x00;
                [self command:DS2_CMD_PAIR sub:0x03 payload:[NSData dataWithBytes:&fin length:1] completion:^(NSData *r4) {
                    LOG("switch2: bond %s", r4 ? "accepted" : "finish refused");
                }];
            }];
        }];
    }];
}

// ---- Handshake ----

- (void)readCalibration:(int)which done:(void (^)(void))done {
    uint32_t user = which ? DS2_ADDR_USER_STICK2 : DS2_ADDR_USER_STICK1;
    uint32_t factory = which ? DS2_ADDR_FACTORY_STICK2 : DS2_ADDR_FACTORY_STICK1;
    __weak DmSwitch2Session *weak = self;
    [self readMemory:0x0B at:user completion:^(NSData *d) {
        DmSwitch2Session *s = weak; if (!s) return;
        if (d && !ds2_cal_blank(d.bytes, d.length)) { ds2_cal_parse(d.bytes, d.length, &s->_cal[which]); done(); return; }
        [s readMemory:0x0B at:factory completion:^(NSData *f) {
            DmSwitch2Session *s2 = weak; if (!s2) return;
            if (f) ds2_cal_parse(f.bytes, f.length, &s2->_cal[which]);
            done();
        }];
    }];
}

- (void)finishHandshake {
    self.stage = DS2StageFeatures;
    __weak DmSwitch2Session *w = self;
    // Serial: LEDs, feature mask, enable, then input notifications (the console's order).
    [w setPlayerLEDs:^(BOOL ok1) {
        [w setFeature:DS2_SUB_FEATURE_SET completion:^(BOOL ok2) {
            [w setFeature:DS2_SUB_FEATURE_ENABLE completion:^(BOOL ok3) {
                if (!ok3) { [w fail:@"feature enable refused"]; return; }
                w.stage = DS2StageInput;
                CBCharacteristic *in = [w ch:DS2_UUID_INPUT];
                if (!in) { [w fail:@"input characteristic missing"]; return; }
                [w.peripheral setNotifyValue:YES forCharacteristic:in];
            }];
        }];
    }];
}

- (void)handshakeInfo {
    self.stage = DS2StageInfo;
    __weak DmSwitch2Session *weak = self;
    [self readMemory:0x40 at:DS2_ADDR_INFO completion:^(NSData *d) {
        DmSwitch2Session *s = weak; if (!s) return;
        if (!d || d.length < 0x25) { [s fail:@"controller info read failed"]; return; }
        const uint8_t *b = d.bytes;
        uint16_t vid = ds2_u16(b + 18), pid = ds2_u16(b + 20);
        char serial[15] = {0}; memcpy(serial, b + 2, 14);
        s.serial = [NSString stringWithUTF8String:serial] ?: @"?";
        if (vid != DS2_VENDOR_ID || pid != DS2_PID_PRO_CONTROLLER_2)
            LOG("switch2: unexpected identity vid=0x%04x pid=0x%04x; using the Pro Controller 2 mapping", vid, pid);
        s.stage = DS2StageCalibration;
        [s readCalibration:0 done:^{
            [s readCalibration:1 done:^{ [s finishHandshake]; }];
        }];
    }];
}

- (void)fail:(NSString *)why {
    LOG("switch2: %s", why.UTF8String);
    [self teardown];
    [[DmSwitch2BLE shared] sessionEnded:self];
}

// ---- Reports ----

- (void)handleReport:(NSData *)d {
    ds2_report r;
    if (!ds2_report_parse(d.bytes, d.length, &r)) return;
    self.reports++;
    _lastReport = ds2_now();
    if (!self.done) return;
    float lx, ly, rx, ry;
    ds2_cal_apply(&_cal[0], r.lx, r.ly, _deadzone, &lx, &ly);
    ds2_cal_apply(&_cal[1], r.rx, r.ry, _deadzone, &rx, &ry);
    dm_pad_set_player(self.slot, ds2_buttons(r.buttons),
                      (int32_t)lrintf(lx * 32767.f), (int32_t)lrintf(ly * 32767.f),
                      (int32_t)lrintf(rx * 32767.f), (int32_t)lrintf(ry * 32767.f));
    if (_motion) {
        // The Switch Pro IMU map (motion_pro.h): raw x toward the shoulders, y left, z out of the face; Switch x right, y forward, gravity down.
        // Inferred for the Switch 2 IMU until it is measured on hardware; that is why it is opt-in.
        const float g = DS2_ACCEL_G_PER_COUNT, rad = DS2_GYRO_RAD_PER_COUNT;
        float acc[3] = {r.accel[1] * g, -r.accel[0] * g, -r.accel[2] * g};
        float rot[3] = {-r.gyro[1] * rad, r.gyro[0] * rad, r.gyro[2] * rad};
        dm_motion_set_hid(self.slot, acc, rot, ds2_now());
    }
    if (_verbose && self.reports % 90 == 1)
        LOG("switch2: slot %d reports=%lu buttons=0x%08x sticks=%u,%u %u,%u battery=%umV", self.slot + 1,
            (unsigned long)self.reports, r.buttons, r.lx, r.ly, r.rx, r.ry, r.battery_mv);
}

// ---- Rumble ----

- (void)rumbleFrom:(const dm_haptic_state *)s {
    float lo = fmaxf(s->motor[0].amplitude_low, s->motor[1].amplitude_low);
    float hi = fmaxf(s->motor[0].amplitude_high, s->motor[1].amplitude_high);
    const dm_vibration *l = s->motor[0].amplitude_low >= s->motor[1].amplitude_low ? &s->motor[0] : &s->motor[1];
    const dm_vibration *h = s->motor[0].amplitude_high >= s->motor[1].amplitude_high ? &s->motor[0] : &s->motor[1];
    int lfA = (int)lrintf(fminf(1.f, fmaxf(0.f, lo)) * 0x3FF);
    int hfA = (int)lrintf(fminf(1.f, fmaxf(0.f, hi)) * 0x3FF);
    atomic_store(&_lfAmp, lfA);
    atomic_store(&_hfAmp, hfA);
    atomic_store(&_lfHz, (int)fminf(511.f, fmaxf(1.f, l->frequency_low)));
    atomic_store(&_hfHz, (int)fminf(511.f, fmaxf(1.f, h->frequency_high)));
}

// ---- Maintenance ----

- (void)startMaintenance {
    if (_tick) return;
    _tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _q);
    dispatch_source_set_timer(_tick, dispatch_time(DISPATCH_TIME_NOW, 0), 50ull * NSEC_PER_MSEC, 5ull * NSEC_PER_MSEC);
    __weak DmSwitch2Session *weak = self;
    dispatch_source_set_event_handler(_tick, ^{ [weak maintain]; });
    dispatch_resume(_tick);
}

- (void)maintain {
    if (!self.done) return;
    uint64_t t = ds2_now();
    int lfA = atomic_load(&_lfAmp), lfF = atomic_load(&_lfHz);
    int hfA = atomic_load(&_hfAmp), hfF = atomic_load(&_hfHz);
    BOOL active = lfA > 0 || hfA > 0 || t < _rumbleHoldUntil;
    if (_rumble && active) {
        ds2_vibration v = {(uint16_t)lfF, (uint16_t)lfA, (uint16_t)hfF, (uint16_t)hfA};
        uint8_t packet[40];
        size_t n = ds2_motor_packet(packet, &v, self->_rumbleSeq++);
        CBCharacteristic *m = [self ch:DS2_UUID_RUMBLE];
        if (m) {
            [self.peripheral writeValue:[NSData dataWithBytes:packet length:n] forCharacteristic:m type:CBCharacteristicWriteWithoutResponse];
            _lastWrite = t;
        }
        _rumbleHoldUntil = t + 120ull * NSEC_PER_MSEC;   // keep sending zeros briefly so the actuator stops instead of holding
    }
    // 1 Hz keep-alive: the last host write is what keeps the link alive on Apple hosts.
    if (t - _lastWrite >= 1000000000ull && !_pending) [self setPlayerLEDs:nil];
}

// ---- CBPeripheralDelegate ----

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error {
    if (error) { [self fail:[NSString stringWithFormat:@"service discovery: %@", error.localizedDescription]]; return; }
    for (CBService *s in p.services ?: @[]) [p discoverCharacteristics:nil forService:s];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error {
    if (error) { [self fail:[NSString stringWithFormat:@"characteristic discovery: %@", error.localizedDescription]]; return; }
    for (CBCharacteristic *c in service.characteristics ?: @[]) _chars[c.UUID.UUIDString.uppercaseString] = c;
    if ([self ch:DS2_UUID_CMD_WR] && [self ch:DS2_UUID_CMD_RSP] && [self ch:DS2_UUID_INPUT] && self.stage == DS2StageServices) {
        self.stage = DS2StageIdle;
        [p setNotifyValue:YES forCharacteristic:[self ch:DS2_UUID_CMD_RSP]];
    }
}

- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    NSString *u = c.UUID.UUIDString.uppercaseString;
    if (error) {
        if ([u isEqualToString:@DS2_UUID_CMD_RSP] || [u isEqualToString:@DS2_UUID_INPUT])
            [self fail:[NSString stringWithFormat:@"notify %@: %@", c.UUID.UUIDString, error.localizedDescription]];
        else if (_verbose) LOG("switch2: notify refused on %s: %s", c.UUID.UUIDString.UTF8String, error.localizedDescription.UTF8String);
        return;
    }
    if ([u isEqualToString:@DS2_UUID_CMD_RSP] && self.stage == DS2StageIdle) {
        [self handshakeInfo];
    } else if ([u isEqualToString:@DS2_UUID_INPUT]) {
        if (self.slot < 0) {
            int slot = dm_pad_external_claim((__bridge const void *)self, "Pro Controller 2");
            if (slot < 0) { [self fail:@"all players are taken"]; return; }
            self.slot = slot;
            LOG("switch2: Pro Controller 2 ready: player %d%s", slot + 1, _motion ? " (motion on)" : "");
        }
        self.done = YES;
        self.stage = DS2StageReady;
        [gRumbleLock lock]; gRumbleSlot[self.slot] = self; [gRumbleLock unlock];
        dm_haptics_apple_set_ble(self.slot, ds2_rumble_dispatch);   // owns the player even with rumble off, so the phone does not buzz instead
        [self startMaintenance];
        [self bond];
        [[DmSwitch2BLE shared] sessionReady:self];
    }
}

- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    if (error) return;
    NSData *d = c.value; if (!d.length) return;
    NSString *u = c.UUID.UUIDString.uppercaseString;
    if ([u isEqualToString:@DS2_UUID_INPUT]) { [self handleReport:d]; return; }
    if ([u isEqualToString:@DS2_UUID_CMD_RSP]) {
        if (!_pending || d.length < 8) return;
        const uint8_t *b = d.bytes;
        if (b[0] != _pendingID || (b[1] != 0x01 && b[1] != 0x02)) return;   // 0x01 success; 0x02 a status reply with the same shape
        _pending = NO;
        void (^done)(NSData *) = _completion; _completion = nil;
        if (done) done([d subdataWithRange:NSMakeRange(8, d.length - 8)]);
        [self pump];
    }
}

- (void)peripheral:(CBPeripheral *)p didModifyServices:(NSArray<CBService *> *)invalidated {
    [self fail:@"services changed (the controller reset the link)"];
}

@end

// ---- The central ----

@implementation DmSwitch2BLE {
    dispatch_queue_t _q;
    CBCentralManager *_central;
    NSMutableDictionary<NSUUID *, DmSwitch2Session *> *_sessions;
    NSMutableDictionary<NSUUID *, DmSwitch2Session *> *_connecting;
    NSMutableSet<NSUUID *> *_ignored;
    BOOL _started;
}

+ (instancetype)shared {
    static DmSwitch2BLE *s; static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [DmSwitch2BLE new]; });
    return s;
}

- (instancetype)init {
    if ((self = [super init])) {
        _q = dispatch_queue_create("com.amesvt.nxnative.switch2", DISPATCH_QUEUE_SERIAL);
        _sessions = [NSMutableDictionary new];
        _connecting = [NSMutableDictionary new];
        _ignored = [NSMutableSet new];
        gRumbleLock = [NSLock new];
    }
    return self;
}

- (void)start {
    if (_started) return;
    _started = YES;
    _central = [[CBCentralManager alloc] initWithDelegate:self queue:_q options:@{CBCentralManagerOptionShowPowerAlertKey: @YES}];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        (void)n; dispatch_async(self->_q, ^{ [self updateScanning]; });
    }];
    [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        (void)n; dispatch_async(self->_q, ^{ if (self->_central.isScanning) [self->_central stopScan]; });
    }];
}

- (void)updateScanning {
    if (_central.state != CBManagerStatePoweredOn) return;
    NSUInteger busy = _sessions.count + _connecting.count;
    if (busy < DM_PLAYERS) {
        if (!_central.isScanning) {
            [_central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
            LOG("switch2: scanning for a Switch 2 Pro Controller (hold Sync until the player LEDs sweep; if iOS Settings holds it, forget it there first)");
        }
    } else if (_central.isScanning) {
        [_central stopScan];
    }
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    switch (central.state) {
        case CBManagerStatePoweredOn: {
            LOG("switch2: Bluetooth ready");
            [self updateScanning];
            __weak DmSwitch2BLE *weak = self;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(20.0 * NSEC_PER_SEC)), _q, ^{
                DmSwitch2BLE *m = weak; if (!m) return;
                if (m->_sessions.count + m->_connecting.count == 0)
                    LOG("switch2: still no Switch 2 Pro Controller; hold Sync until its player LEDs sweep. If iOS Settings > Bluetooth shows it connected, forget it there first: the system link hides it from apps");
            });
            break;
        }
        case CBManagerStateUnauthorized: LOG("switch2: Bluetooth permission denied; allow it in Settings > Privacy & Security > Bluetooth"); break;
        case CBManagerStatePoweredOff: LOG("switch2: Bluetooth is off"); break;
        case CBManagerStateUnsupported: LOG("switch2: Bluetooth LE is not available on this device"); break;
        default: break;
    }
}

- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)p advertisementData:(NSDictionary<NSString *, id> *)adv rssi:(NSNumber *)rssi {
    NSData *manu = adv[CBAdvertisementDataManufacturerDataKey];
    if (!manu || manu.length < 18) return;
    const uint8_t *b = manu.bytes;
    if (ds2_u16(b) != DS2_COMPANY_ID) return;
    ds2_advert a;
    if (!ds2_advert_parse(b + 2, manu.length - 2, &a)) return;
    if (_connecting[p.identifier] || _sessions[p.identifier]) return;
    if (a.pid != DS2_PID_PRO_CONTROLLER_2) {
        if (![_ignored containsObject:p.identifier]) {
            [_ignored addObject:p.identifier];
            LOG("switch2: %s (product 0x%04x) is not a Pro Controller 2: ignored", (p.name ?: @"controller").UTF8String, a.pid);
        }
        return;
    }
    if (_sessions.count + _connecting.count >= DM_PLAYERS) return;
    LOG("switch2: found %s rssi=%d%s", (p.name ?: @"Pro Controller 2").UTF8String, rssi.intValue,
        a.host ? " (wake)" : " (pairing mode)");
    DmSwitch2Session *s = [[DmSwitch2Session alloc] initWithPeripheral:p queue:_q];
    _connecting[p.identifier] = s;
    [central stopScan];   // scanning during a connection hurts the report rate; resumed after the handshake or a failure
    [central connectPeripheral:p options:nil];
    __weak DmSwitch2BLE *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(10.0 * NSEC_PER_SEC)), _q, ^{
        DmSwitch2BLE *m = weak; if (!m) return;
        if (m->_connecting[p.identifier] != s) return;
        LOG("switch2: connect timed out; rescanning");
        [m->_central cancelPeripheralConnection:p];
        [m->_connecting removeObjectForKey:p.identifier];
        [m updateScanning];
    });
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)p {
    DmSwitch2Session *s = _connecting[p.identifier];
    if (!s) { [central cancelPeripheralConnection:p]; return; }
    [_connecting removeObjectForKey:p.identifier];
    _sessions[p.identifier] = s;
    [s beginHandshake];
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    LOG("switch2: connect failed: %s", (error.localizedDescription ?: @"unknown error").UTF8String);
    [_connecting removeObjectForKey:p.identifier];
    [self updateScanning];
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    DmSwitch2Session *s = _sessions[p.identifier];
    if (s) {
        LOG("switch2: %s disconnected%s", (p.name ?: @"controller").UTF8String, error ? [[NSString stringWithFormat:@": %@", error.localizedDescription] UTF8String] : "");
        [s teardown];
        [_sessions removeObjectForKey:p.identifier];
    } else {
        [_connecting removeObjectForKey:p.identifier];
    }
    [self updateScanning];
}

- (void)sessionReady:(DmSwitch2Session *)s { (void)s; [self updateScanning]; }

- (void)sessionEnded:(DmSwitch2Session *)s {
    [_connecting removeObjectForKey:s.peripheral.identifier];
    [_sessions removeObjectForKey:s.peripheral.identifier];
    [_central cancelPeripheralConnection:s.peripheral];
    [self updateScanning];
}

@end

void dm_switch2_start(void) {
    @autoreleasepool { [[DmSwitch2BLE shared] start]; }
}
