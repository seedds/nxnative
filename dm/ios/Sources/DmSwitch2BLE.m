// Native Nintendo Switch 2 controller support for the iOS front end, over CoreBluetooth: the Pro Controller 2, a left / right Joy-Con 2 (held
// on its own or together as one pad), and the NSO GameCube pad.
//
// iOS has no system-level support for a Switch 2 controller: it is not a HID device and GameController.framework never sees it. The controller
// speaks Nintendo's proprietary BLE GATT protocol instead, decoded by the Mac bridge "Finally the Controller Works" by Peter Sharma
// (https://github.com/Peterksharma/switch2mac): input notifications AB7DE9BE…, commands on 649D4AC9…, replies on C765A961…, rumble on
// CC483F51… (each Joy-Con 2 half has its own vibration characteristic). The GATT identifiers, command framing, report layout, calibration and
// rumble packing are a port of that project's published notes and Swift implementation, which credits the wider Switch 2 reverse-engineering
// community (ndeadly, trevlars' MIT Linux bridge, Nadeflore/Switch2Connect and others). The Joy-Con 2 pad rules are JakePre's hardware-verified
// PR 1346; the recorded reports DmSwitch2BLE's own tests keep proving them. The installed 0.2.0 binary was checked in Ghidra for the constants.
// DmSwitch2BLE is a CoreBluetooth central that performs the handshake, decodes input reports and feeds the same pad/motion/haptics paths as a
// GameController controller (the slot table in DmGamepad.m, hle_hid.c). The game never learns the difference.
//
// Scope and limits, all observed or explicitly marked as inferred:
//  * Pro Controller 2 (0x2069), Joy-Con 2 (0x2067 left, 0x2066 right; one alone is used sideways and a left and right together are one pad) and
//    the NSO GameCube pad (0x2073). Joy-Con motion and the pair's stick split are measured on hardware (PR 1346); the GameCube's input is from
//    switch2mac's published protocol and is not yet confirmed on our own hardware, and its analog triggers act as digital holds above a quarter
//    travel (the Npad interface has no trigger axes; the console itself maps them onto digital ZL / ZR).
//  * The NSO GameCube pad has no HD-rumble actuator: writing its motor characteristic powers the pad off, so its lane is never written and it
//    stays silent. Its built-in rumble presets (command 0x0A/0x02) are not used: their payloads are not published yet.
//  * No SMP and no application-layer bonding: the controller drops links that attempt SMP, and iOS exposes no public host Bluetooth address for
//    command 0x15. So the controller is not taught to wake toward the iPhone. Hold Sync to advertise in pairing mode, or press a button while the
//    game is open and scanning. A controller bonded to a Mac/Switch does not wake for the iPhone. DM_SW2_BOND_MAC=aa:bb:cc:dd:ee:ff sends the four
//    0x15 commands for a host address the caller knows (off by default; there is no public way to read the iPhone's own address).
//  * A 1 Hz player-LED refresh is the keep-alive: on Apple hosts the link is dropped after ~10-17 s without a host write even while reports arrive.
//  * Motion is decoded but published only with DM_SW2_MOTION=1: the Pro Controller 2's own IMU frame is not measured on hardware (the maps of
//    JakePre's PR 1346, measured on a right Joy-Con 2, are used; they make the pair and half forms right and the Pro's change remains possible).
//    DM_SW2_RUMBLE=0 turns HD rumble off; DM_SW2_DEADZONE=<counts> overrides the 40-count centre deadzone.
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
@class DmSwitch2BLE;

// A player's haptics target: one session (a full pad member) or a DS2Pad driving a grip of two Joy-Con halves.
@protocol DS2HapticTarget
- (void)rumbleFrom:(const dm_haptic_state *)s;
@end

// The pad a player holds: one slot in the gamepad table. A full member (a Pro Controller 2 or an NSO GameCube pad) is a pad on its own; of the
// Joy-Con 2 halves, a left and a right together share one pad (the pair is a Pro Controller: the left's stick the left stick, the right's the
// right, motion from the right half), and one half on its own is a sideways pad. The pad owns the slot claim and drives dm_pad_set_player as
// each member's report arrives, because a member's report carries only its own half.
@interface DS2Pad : NSObject <DS2HapticTarget>
@property (nonatomic) int slot;         // 0-based; the player is slot + 1
- (BOOL)canAccept:(uint16_t)pid;
- (BOOL)addSession:(DmSwitch2Session *)s;
- (void)reportFrom:(DmSwitch2Session *)s;
- (void)sessionLeft:(DmSwitch2Session *)s;
- (BOOL)isEmpty;
@property (nonatomic, readonly) NSUInteger memberCount;
- (void)endAllSessions:(void (^)(DmSwitch2Session *))block;
@end

@interface DmSwitch2Session : NSObject <CBPeripheralDelegate, DS2HapticTarget>
@property (nonatomic, readonly) CBPeripheral *peripheral;
@property (nonatomic) uint16_t pid;
@property (nonatomic) DS2Stage stage;
@property (nonatomic) int slot;                       // 0-based; the player is slot + 1 (a grip half shares its pad's slot)
@property (nonatomic, weak) DS2Pad *pad;              // set once the pad is joined
@property (nonatomic) BOOL done;                      // handshake finished: reports drive the pad
@property (nonatomic, copy) NSString *serial;
@property (nonatomic) NSUInteger reports;
@property (nonatomic) uint16_t battery_mv;
@property (nonatomic) int battery_percent;
@property (nonatomic) int charging;
@property (nonatomic) int rssi;
- (instancetype)initWithPeripheral:(CBPeripheral *)p queue:(dispatch_queue_t)q;
- (void)beginHandshake;
- (void)teardown;
- (void)rumbleLow:(float)lfA lfHz:(float)lfHz high:(float)hfA hfHz:(float)hfHz;  // store rumble targets; see rumbleFrom
- (void)rumbleFrom:(const dm_haptic_state *)s;         // any thread; stores targets only
- (void)testRumble;
- (NSString *)name;                                    // the model's name for the launcher's list
- (ds2_dev *)devSnapshot;                              // the handshake's state (calibration included) for the pad to copy
- (ds2_dev *)devSlot;                                  // the pad's member this session feeds; NULL before joining
- (void)setDevSlot:(ds2_dev *)dev;
@end

// The haptics hook is a plain C function pointer with no context, so the target is found through the per-player registry.
static id<DS2HapticTarget> gRumbleSlot[DM_PLAYERS];
static NSLock *gRumbleLock;

static void ds2_rumble_dispatch(const dm_haptic_state *s) {
    if (!s || s->player < 0 || s->player >= DM_PLAYERS) return;
    [gRumbleLock lock];
    id<DS2HapticTarget> target = gRumbleSlot[s->player];
    [gRumbleLock unlock];
    [target rumbleFrom:s];
}

@interface DmSwitch2BLE : NSObject <CBCentralManagerDelegate>
+ (instancetype)shared;
- (void)start;
- (void)sessionEnded:(DmSwitch2Session *)s;
- (void)sessionReady:(DmSwitch2Session *)s;
- (DS2Pad *)padForSession:(DmSwitch2Session *)s;
- (void)padEnded:(DS2Pad *)p;
- (void)startScan;
- (void)stopScan;
- (BOOL)isScanning;
- (int)getControllers:(dm_switch2_controller_info *)out maxCount:(int)maxCount;
- (void)disconnectPlayer:(int)player;
- (void)testRumblePlayer:(int)player;
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
    ds2_dev _dev;                               // this controller's member slot in its pad: report, calibration, gyro rest learner
    ds2_dev *_devSlot;                          // the pad's member this session feeds once joined; NULL before the handshake finishes
    BOOL _rumble, _verbose;
    uint8_t _rumbleSeq;
    uint64_t _lastWrite, _lastReport, _lastFindLED;
    uint64_t _findUntil;                            // while set (ms of monotonic time), the 50 ms tick serves the find chase
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
        memset(&_dev, 0, sizeof _dev);
        _dev.have = 1;
        for (int i = 0; i < 2; i++) ds2_cal_default(&_dev.cal[i]);
        const char *e = getenv("DM_SW2_RUMBLE"); _rumble = !e || atoi(e);
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

// The vibration characteristic of this model: the Pro Controller 2 and the NSO GameCube share one lane (the GameCube's is never written: it
// powers the pad off), and each Joy-Con 2 half has its own.
- (const char *)rumbleUUID {
    switch (self.pid) {
        case DS2_PID_JOYCON2_L: return DS2_UUID_RUMBLE_JC_L;
        case DS2_PID_JOYCON2_R: return DS2_UUID_RUMBLE_JC_R;
        default: return DS2_UUID_RUMBLE;
    }
}

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
    if (self.pad) {
        [self.pad sessionLeft:self];
        self.pad = nil;
    }
    self.slot = -1;
    self.done = NO;
}

// The controller's name for the launcher's list, by model.
- (NSString *)name {
    switch (self.pid) {
        case DS2_PID_JOYCON2_L: return @"Joy-Con 2 (L)";
        case DS2_PID_JOYCON2_R: return @"Joy-Con 2 (R)";
        case DS2_PID_NSO_GAMECUBE: return @"NSO GameCube pad";
        default: return @"Pro Controller 2";
    }
}

- (ds2_dev *)devSnapshot { return &_dev; }
- (ds2_dev *)devSlot { return _devSlot; }
- (void)setDevSlot:(ds2_dev *)dev { _devSlot = dev; }

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

- (void)ledPattern:(uint8_t)pattern completion:(void (^)(BOOL))done {
    uint8_t p[4] = {pattern, 0, 0, 0};
    [self command:DS2_CMD_LEDS sub:DS2_SUB_LEDS_SET payload:[NSData dataWithBytes:p length:4] completion:^(NSData *r) { if (done) done(r != nil); }];
}

- (void)setPlayerLEDs:(void (^)(BOOL))done {
    int player = self.slot >= 0 ? self.slot + 1 : 1;
    [self ledPattern:player >= 1 && player <= 8 ? ds2_led_pattern[player - 1] : 0x01 completion:done];
}

- (void)setFeature:(uint8_t)sub completion:(void (^)(BOOL))done {
    uint8_t p[4] = {DS2_FEATURES, 0, 0, 0};
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
        if (d && ds2_cal_parse(d.bytes, d.length, &s->_dev.cal[which])) { done(); return; }
        [s readMemory:0x0B at:factory completion:^(NSData *f) {
            DmSwitch2Session *s2 = weak; if (!s2) return;
            if (f) ds2_cal_parse(f.bytes, f.length, &s2->_dev.cal[which]);
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
        s.pid = pid; s->_dev.pid = pid;
        if (vid != DS2_VENDOR_ID)
            LOG("switch2: unexpected identity vid=0x%04x pid=0x%04x; using the Pro Controller 2 mapping", vid, pid);
        else if (!ds2_is_controller(pid))
            LOG("switch2: unknown Nintendo model 0x%04x: using the Pro Controller 2 mapping", pid);
        // A Joy-Con has no stick-2 calibration; its one stick's slots are stick 1 even when the right unit's data reports in the right field.
        if (pid == DS2_PID_JOYCON2_L || pid == DS2_PID_JOYCON2_R)
            LOG("switch2: %s connected: the Sideways hold maps its rail buttons to L / R, or a matching opposite half joins it into one pad",
                s.pid == DS2_PID_JOYCON2_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)");
        else if (pid == DS2_PID_NSO_GAMECUBE)
            LOG("switch2: NSO GameCube pad connected: analog triggers act as digital holds, rumble is silent (the pad has no actuator)");
        s.stage = DS2StageCalibration;
        [s readCalibration:0 done:^{
            DmSwitch2Session *s2 = weak; if (!s2) return;
            if (s2.pid == DS2_PID_JOYCON2_L || s2.pid == DS2_PID_JOYCON2_R) { [s2 finishHandshake]; return; }
            [s2 readCalibration:1 done:^{ [s2 finishHandshake]; }];
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
    self.battery_mv = r.battery_mv;
    if (r.battery_mv >= 3400) {
        int pct = (int)(((int)r.battery_mv - 3400) * 100 / (4200 - 3400));
        self.battery_percent = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    } else if (r.battery_mv > 0) {
        self.battery_percent = 0;
    }
    if (!self.done) return;
    // The member's report lands in its pad, which composes the whole pad from every member and drives the gamepad slot.
    ds2_dev *dev = _devSlot ?: &_dev;
    dev->r = r;
    self.charging = r.battery_current > 0;
    if (_verbose && self.reports % 90 == 1)
        LOG("switch2: slot %d reports=%lu buttons=0x%08x sticks=%u,%u %u,%u battery=%umV %dmA%s imu=%.1fC", self.slot + 1,
            (unsigned long)self.reports, r.buttons, r.lx, r.ly, r.rx, r.ry, r.battery_mv, r.battery_current,
            r.battery_current > 0 ? " (charging)" : "", 25.f + r.temperature_raw / 127.f);
    [self.pad reportFrom:self];
}

- (void)testRumble {
    dm_haptic_state s = {0};
    s.player = self.slot >= 0 ? self.slot : 0;
    s.motor[0].amplitude_low = 0.8f;
    s.motor[0].frequency_low = 160.0f;
    s.motor[0].amplitude_high = 0.8f;
    s.motor[0].frequency_high = 320.0f;
    s.motor[1] = s.motor[0];
    [self rumbleFrom:&s];
    __weak DmSwitch2Session *weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        dm_haptic_state off = {0};
        off.player = weak ? (weak.slot >= 0 ? weak.slot : 0) : 0;
        [weak rumbleFrom:&off];
    });
}

// ---- Rumble ----

// Store rumble targets (any thread): amplitudes 0..0x3FF with the frequencies as given. A band at amplitude 0 stays off until the next nonzero write.
- (void)rumbleLow:(float)lfA lfHz:(float)lfHz high:(float)hfA hfHz:(float)hfHz {
    atomic_store(&_lfAmp, (int)lrintf(fminf(1.f, fmaxf(0.f, lfA)) * 0x3FF));
    atomic_store(&_hfAmp, (int)lrintf(fminf(1.f, fmaxf(0.f, hfA)) * 0x3FF));
    atomic_store(&_lfHz, (int)fminf(511.f, fmaxf(1.f, lfHz)));
    atomic_store(&_hfHz, (int)fminf(511.f, fmaxf(1.f, hfHz)));
}

- (void)rumbleFrom:(const dm_haptic_state *)s {
    float lo = fmaxf(s->motor[0].amplitude_low, s->motor[1].amplitude_low);
    float hi = fmaxf(s->motor[0].amplitude_high, s->motor[1].amplitude_high);
    const dm_vibration *l = s->motor[0].amplitude_low >= s->motor[1].amplitude_low ? &s->motor[0] : &s->motor[1];
    const dm_vibration *h = s->motor[0].amplitude_high >= s->motor[1].amplitude_high ? &s->motor[0] : &s->motor[1];
    [self rumbleLow:lo lfHz:l->frequency_low high:hi hfHz:h->frequency_high];
}

// The motor tone the find mode pulses: a mid-strong two-band tone. The write goes straight to the characteristic and is skipped on a GameCube.
- (void)writeFindPulse {
    if (self.pid == DS2_PID_NSO_GAMECUBE) return;
    ds2_vibration v = {0x80, 0x280, 0x100, 0x280};
    uint8_t packet[40];
    size_t n = ds2_motor_packet(packet, &v, self->_rumbleSeq++, self.pid == DS2_PID_PRO_2 ? 2 : 1);
    CBCharacteristic *m = [self ch:[self rumbleUUID]];
    if (m) {
        [self.peripheral writeValue:[NSData dataWithBytes:packet length:n] forCharacteristic:m type:CBCharacteristicWriteWithoutResponse];
        _lastWrite = ds2_now();
    }
}

// Find mode: for a few seconds the player LEDs run one at a time and the motors pulse, so the controller is known among four dark ones.
- (void)startFind {
    if (!self.done) return;
    _findUntil = ds2_now() + 4000000000ull;
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
    // The find chase: LEDs one at a time at 10 Hz, a motor pulse every 0.5 s (not on a GameCube), and the keep-alive is covered by these writes.
    if (t < _findUntil) {
        if (t - _lastFindLED >= 100000000ull) {
            _lastFindLED = t;
            [self ledPattern:(uint8_t)(1 << ((t / 150000000ull) & 3)) completion:nil];
        }
        if (t % 500000000ull < 50000000ull) [self writeFindPulse];
        return;
    }
    int lfA = atomic_load(&_lfAmp), lfF = atomic_load(&_lfHz);
    int hfA = atomic_load(&_hfAmp), hfF = atomic_load(&_hfHz);
    BOOL active = lfA > 0 || hfA > 0 || t < _rumbleHoldUntil;
    // The NSO GameCube pad is never motor-written: its lane powers it off (see the header comment).
    if (_rumble && active && self.pid != DS2_PID_NSO_GAMECUBE) {
        ds2_vibration v = {(uint16_t)lfF, (uint16_t)lfA, (uint16_t)hfF, (uint16_t)hfA};
        uint8_t packet[40];
        size_t n = ds2_motor_packet(packet, &v, self->_rumbleSeq++, self.pid == DS2_PID_PRO_2 ? 2 : 1);
        CBCharacteristic *m = [self ch:[self rumbleUUID]];
        if (m) {
            [self.peripheral writeValue:[NSData dataWithBytes:packet length:n] forCharacteristic:m type:CBCharacteristicWriteWithoutResponse];
            _lastWrite = t;
        }
        if (lfA > 0 || hfA > 0) _rumbleHoldUntil = t + 120ull * NSEC_PER_MSEC;   // zero packets must not extend their own stop deadline
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
        DS2Pad *pad = [[DmSwitch2BLE shared] padForSession:self];
        if (!pad) { [self fail:@"all players are taken"]; return; }
        self.done = YES;
        self.stage = DS2StageReady;
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

// ---- The pad a player holds ----

@implementation DS2Pad {
    ds2_pad_state _state;
    NSMutableArray<DmSwitch2Session *> *_members;
    float _deadzone;
    BOOL _motion;
    uint64_t _glMask, _grMask;
    uint8_t _gcHeld;    // the NSO GameCube pad's trigger hold threshold, report counts (DM_SW2_GC_TRIGGER)
    int _slot;   // -1 until the pad claims a player
}

- (instancetype)init {
    if ((self = [super init])) {
        _members = [NSMutableArray new];
        _slot = -1;
        memset(&_state, 0, sizeof _state);
        const char *e = getenv("DM_SW2_DEADZONE"); _deadzone = e ? (float)atof(e) : 40.f;
        e = getenv("DM_SW2_MOTION"); _motion = e && atoi(e);
        e = getenv("DM_PAD_GL"); _glMask = ds2_btn_from_str(e);
        e = getenv("DM_PAD_GR"); _grMask = ds2_btn_from_str(e);
        if (_glMask || _grMask) LOG("switch2: back buttons GL=0x%llx GR=0x%llx (the Pro Controller 2's back buttons)", (unsigned long long)_glMask, (unsigned long long)_grMask);
        int held = DS2_TRIGGER_HELD;
        if ((e = getenv("DM_SW2_GC_TRIGGER")) && *e) held = atoi(e);
        _gcHeld = held < 0 ? 0 : held > 255 ? 255 : held;
    }
    return self;
}

// A full pad member joins a fresh pad only; a half joins a pad of its own side or one holding the opposite half alone.
- (BOOL)canAccept:(uint16_t)pid {
    switch (pid) {
        case DS2_PID_PRO_2: case DS2_PID_NSO_GAMECUBE: return !_state.pro.have;
        case DS2_PID_JOYCON2_L: return !_state.left.have && !_state.pro.have;
        case DS2_PID_JOYCON2_R: return !_state.right.have && !_state.pro.have;
        default: return NO;
    }
}

// Whether the pad holds exactly one half a matching opposite half could join (the pair is formed when the second half arrives).
- (BOOL)isLoneHalf:(uint16_t)pid {
    if (_state.pro.have || _members.count != 1) return NO;
    if (pid == DS2_PID_JOYCON2_L) return _state.right.have;
    if (pid == DS2_PID_JOYCON2_R) return _state.left.have;
    return NO;
}

- (ds2_dev *)devForSession:(DmSwitch2Session *)s {
    switch (s.pid) {
        case DS2_PID_JOYCON2_L: return &_state.left;
        case DS2_PID_JOYCON2_R: return &_state.right;
        default: return &_state.pro;
    }
}

- (BOOL)addSession:(DmSwitch2Session *)s {
    if (self.slot >= 0 && ![self canAccept:s.pid]) return NO;
    ds2_dev *dev = [self devForSession:s];
    *dev = *[s devSnapshot];        // copy the handshake's state (calibration included), then the session writes into this member
    if (![_members containsObject:s]) [_members addObject:s];
    if (_slot < 0) {
        _slot = dm_pad_external_claim((__bridge const void *)self, self.name.UTF8String);
        if (_slot < 0) {
            memset(dev, 0, sizeof *dev);
            [_members removeObject:s];
            return NO;
        }
        if (_motion) LOG("switch2: %s is player %d (motion on)", self.name.UTF8String, _slot + 1);
        else LOG("switch2: %s is player %d", self.name.UTF8String, _slot + 1);
    } else {
        LOG("switch2: %s joined %s as player %d: they are one pad now", s.name.UTF8String, self.name.UTF8String, _slot + 1);
    }
    s.slot = _slot;
    [s setDevSlot:dev];
    [gRumbleLock lock]; gRumbleSlot[_slot] = self; [gRumbleLock unlock];
    dm_haptics_apple_set_ble(_slot, ds2_rumble_dispatch);   // owns the player even with rumble off, so the phone does not buzz instead
    [self drive];                   // a surviving half's last report may hold buttons a departed member left set
    return YES;
}

- (NSString *)name {
    if (_state.pro.have) return _state.pro.pid == DS2_PID_NSO_GAMECUBE ? @"NSO GameCube pad" : @"Pro Controller 2";
    if (_state.left.have && _state.right.have) return @"Joy-Con 2 (L+R)";
    return _state.left.have ? @"Joy-Con 2 (L)" : @"Joy-Con 2 (R)";
}

- (BOOL)isEmpty { return _members.count == 0; }
- (NSUInteger)memberCount { return _members.count; }

// Run block for every member (a copy of the list: the block may remove members).
- (void)endAllSessions:(void (^)(DmSwitch2Session *))block {
    for (DmSwitch2Session *s in [_members copy]) block(s);
}

- (int)slot { return _slot; }
- (void)setSlot:(int)s { _slot = s; }

- (void)drive {
    if (_slot < 0 || !_members.count) return;
    uint64_t buttons; int32_t st[4];
    if (!ds2_pad_input(&_state, _deadzone, _gcHeld, _glMask, _grMask, &buttons, st)) return;
    dm_pad_set_player(_slot, buttons, st[0], st[1], st[2], st[3]);
    if (!_motion) return;
    ds2_dev *dev = ds2_pad_motion_dev(&_state);
    if (!dev) return;
    const int8_t *map = ds2_pad_map(&_state, dev);
    int16_t raw[6] = {dev->r.accel[0], dev->r.accel[1], dev->r.accel[2], dev->r.gyro[0], dev->r.gyro[1], dev->r.gyro[2]};
    float acc[3], rot[3];
    ds2_rest_update(&dev->rest, raw);       // a second at rest learns the gyro's own offset (ds2_rest)
    ds2_motion(raw, map, dev->rest.have ? dev->rest.bias : NULL, acc, rot);
    dm_motion_set_hid(_slot, acc, rot, ds2_now());
}

- (void)reportFrom:(DmSwitch2Session *)s {
    if (s.pad != self) return;
    [self drive];
}

- (void)sessionLeft:(DmSwitch2Session *)s {
    [_members removeObject:s];
    ds2_dev *dev = [self devForSession:s];
    if ([s devSlot] == dev) [s setDevSlot:NULL];
    memset(dev, 0, sizeof *dev);
    s.slot = -1;
    if (_members.count) return;   // a grip that lost one half keeps playing as the surviving half's sideways pad
    if (_slot >= 0) {
        dm_pad_set_player(_slot, 0, 0, 0, 0, 0);
        [gRumbleLock lock]; gRumbleSlot[_slot] = nil; [gRumbleLock unlock];
        dm_haptics_apple_set_ble(_slot, NULL);
        if (_motion) dm_motion_clear_hid(_slot);
        dm_pad_external_release((__bridge const void *)self);
        _slot = -1;
    }
    [[DmSwitch2BLE shared] padEnded:self];
}

// Find mode on every member: each half's LEDs run the same chase and their motors pulse (rumble only on the HD models).
- (void)find {
    for (DmSwitch2Session *m in _members) [m startFind];
}

// The pad's rumble: a full member takes both bands as they come; a pair follows the console's split, the left half the low band, the right half
// the high band (say the same tone to both and the actuator answers from its own half). A GameCube pad's member is never motor-written.
- (void)rumbleFrom:(const dm_haptic_state *)s {
    if (_state.pro.have || _members.count <= 1) {
        for (DmSwitch2Session *m in _members) [m rumbleFrom:s];
        return;
    }
    float lo = fmaxf(s->motor[0].amplitude_low, s->motor[1].amplitude_low);
    float hi = fmaxf(s->motor[0].amplitude_high, s->motor[1].amplitude_high);
    const dm_vibration *l = s->motor[0].amplitude_low >= s->motor[1].amplitude_low ? &s->motor[0] : &s->motor[1];
    const dm_vibration *h = s->motor[0].amplitude_high >= s->motor[1].amplitude_high ? &s->motor[0] : &s->motor[1];
    for (DmSwitch2Session *m in _members) {
        if ([m devSlot] == &_state.left) [m rumbleLow:lo lfHz:l->frequency_low high:0 hfHz:0];
        else if ([m devSlot] == &_state.right) [m rumbleLow:0 lfHz:0 high:hi hfHz:h->frequency_high];
    }
}

// The pad's own test rumble: on a pair, the console's split (the left half the low band, the right half the high band); a full pad member takes
// both; a GameCube pad stays silent (its member's own write is skipped in the session too).
- (void)testRumble {
    dm_haptic_state s = {0};
    s.player = _slot >= 0 ? _slot : 0;
    s.motor[0].amplitude_low = 0.8f;
    s.motor[0].frequency_low = 160.0f;
    s.motor[0].amplitude_high = 0.8f;
    s.motor[0].frequency_high = 320.0f;
    s.motor[1] = s.motor[0];
    [self rumbleFrom:&s];
    __weak DS2Pad *w = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        DS2Pad *p = w; if (!p) return;
        dm_haptic_state off = {0};
        off.player = p->_slot >= 0 ? p->_slot : 0;
        [p rumbleFrom:&off];
    });
}

@end

// ---- The central ----

@implementation DmSwitch2BLE {
    dispatch_queue_t _q;
    CBCentralManager *_central;
    NSMutableDictionary<NSUUID *, DmSwitch2Session *> *_sessions;
    NSMutableArray<DS2Pad *> *_pads;
    NSMutableDictionary<NSUUID *, DmSwitch2Session *> *_connecting;
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
        _pads = [NSMutableArray new];
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
    NSUInteger busy = _pads.count + _connecting.count;   // a grip's two halves share one pad, so count pads, not sessions
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
                    LOG("switch2: still no Switch 2 controller; hold Sync until its player LEDs sweep. If iOS Settings > Bluetooth shows it connected, forget it there first: the system link hides it from apps");
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
    if (_sessions.count + _connecting.count >= DM_PLAYERS) return;
    LOG("switch2: found %s rssi=%d%s", (p.name ?: @"a Switch 2 controller").UTF8String, rssi.intValue,
        a.host ? " (wake)" : " (pairing mode)");
    DmSwitch2Session *s = [[DmSwitch2Session alloc] initWithPeripheral:p queue:_q];
    s.rssi = rssi.intValue;
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

// Which pad a finished session joins: a half prefers the lone opposite half (they become one pair on the earlier half's slot); a full member makes
// a pad of its own (a reconnecting controller gets a fresh pad after its old one released the slot on disconnect).
- (DS2Pad *)padForSession:(DmSwitch2Session *)s {
    DS2Pad *join = nil;
    if (s.pid == DS2_PID_JOYCON2_L || s.pid == DS2_PID_JOYCON2_R) {
        for (DS2Pad *p in _pads)
            if ([p isLoneHalf:s.pid]) { join = p; break; }
    }
    if (!join) {
        join = [DS2Pad new];
        [_pads addObject:join];
    }
    if (![join addSession:s]) {
        if (join.memberCount == 0) [_pads removeObject:join];
        return nil;
    }
    return join;
}

- (void)padEnded:(DS2Pad *)p {
    [_pads removeObjectIdenticalTo:p];
    [self updateScanning];
}

- (void)sessionEnded:(DmSwitch2Session *)s {
    [_connecting removeObjectForKey:s.peripheral.identifier];
    [_sessions removeObjectForKey:s.peripheral.identifier];
    [_central cancelPeripheralConnection:s.peripheral];
    [self updateScanning];
}

- (void)startScan {
    [self start];
    dispatch_async(_q, ^{
        [self updateScanning];
    });
}

- (void)stopScan {
    dispatch_async(_q, ^{
        if (self->_central.isScanning) [self->_central stopScan];
    });
}

- (BOOL)isScanning {
    return _central && _central.isScanning;
}

- (int)getControllers:(dm_switch2_controller_info *)out maxCount:(int)maxCount {
    if (!out || maxCount <= 0) return 0;
    __block int count = 0;
    dispatch_sync(_q, ^{
        for (DmSwitch2Session *s in self->_sessions.allValues) {
            if (count >= maxCount) break;
            dm_switch2_controller_info *info = &out[count++];
            memset(info, 0, sizeof(*info));
            snprintf(info->name, sizeof(info->name), "%s", s.name.UTF8String);
            if (s.serial) snprintf(info->serial, sizeof(info->serial), "%s", s.serial.UTF8String);
            info->player = s.slot + 1;
            info->battery_percent = s.battery_percent;
            info->battery_mv = s.battery_mv;
            info->charging = s.charging;
            info->rssi = s.rssi;
            info->connected = s.done;
        }
    });
    return count;
}

- (void)disconnectPlayer:(int)player {
    dispatch_async(_q, ^{
        // Ending a player ends its whole pad: a grip of two halves is one controller to the game.
        for (DS2Pad *p in [self->_pads copy]) {
            if (p.slot + 1 != player) continue;
            [p endAllSessions:^(DmSwitch2Session *s) { [self sessionEnded:s]; }];
            return;
        }
    });
}

- (void)findPlayer:(int)player {
    dispatch_async(_q, ^{
        for (DS2Pad *p in self->_pads) {
            if (p.slot + 1 == player) {
                [p find];
                return;
            }
        }
    });
}

- (void)testRumblePlayer:(int)player {
    dispatch_async(_q, ^{
        for (DS2Pad *p in self->_pads) {
            if (p.slot + 1 == player) {
                [p testRumble];
                return;
            }
        }
    });
}

@end

void dm_switch2_start(void) {
    @autoreleasepool { [[DmSwitch2BLE shared] start]; }
}

void dm_switch2_scan_start(void) {
    @autoreleasepool { [[DmSwitch2BLE shared] startScan]; }
}

void dm_switch2_scan_stop(void) {
    @autoreleasepool { [[DmSwitch2BLE shared] stopScan]; }
}

bool dm_switch2_is_scanning(void) {
    @autoreleasepool { return [[DmSwitch2BLE shared] isScanning]; }
}

int dm_switch2_get_controllers(dm_switch2_controller_info *out, int max_count) {
    @autoreleasepool { return [[DmSwitch2BLE shared] getControllers:out maxCount:max_count]; }
}

void dm_switch2_disconnect(int player) {
    @autoreleasepool { [[DmSwitch2BLE shared] disconnectPlayer:player]; }
}

void dm_switch2_test_rumble(int player) {
    @autoreleasepool { [[DmSwitch2BLE shared] testRumblePlayer:player]; }
}

void dm_switch2_find(int player) {
    @autoreleasepool { [[DmSwitch2BLE shared] findPlayer:player]; }
}
