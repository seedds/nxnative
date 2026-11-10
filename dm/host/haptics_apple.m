// Core Haptics adapts the game's bands; it cannot reproduce the Switch's exact waveforms.
// All engines and players belong to one serial queue. Guest threads enqueue ordered snapshots only.
#import "haptics_apple.h"
#import <CoreHaptics/CoreHaptics.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#else
#import <AppKit/AppKit.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#endif
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

@interface DmRumbleChannel : NSObject
@property(nonatomic) CHHapticEngine *engine;
@property(nonatomic) id<CHHapticPatternPlayer> pattern;
@property(nonatomic, copy) CHHapticEngine *(^createEngine)(NSError **);
@property(nonatomic, copy) NSString *target;
@property(nonatomic, copy) void (^fallback)(NSTimeInterval);
@property(nonatomic) int player;
@property(nonatomic) int motor;   // the game's motor it plays (0 left, 1 right), or -1 for both mixed
@property(nonatomic, copy) void (^merge)(void);   // replaces a split route by one engine for both motors
@property(nonatomic) uint64_t generation;
@property(nonatomic) uint64_t silenceGeneration;
@property(nonatomic) NSTimeInterval retryAfter;
@property(nonatomic) unsigned failures;
@property(nonatomic) BOOL simple;   // the engine's server offers no advanced players: plain players play the effect in pulses
@property(nonatomic) id<CHHapticPatternPlayer> previous;   // the pulse before the scheduled one, still playing
@property(nonatomic) NSTimeInterval pulseEnd;   // engine time at which the last scheduled pulse ends
@property(nonatomic) dm_haptic_level level;   // the level the next pulse starts at
- (void)apply:(dm_haptic_level)level;
- (void)stop;
@end

static dispatch_queue_t queue;
static NSMutableArray<DmRumbleChannel *> *channels[DM_PLAYERS];
static GCController *controllers[DM_PLAYERS];
#if TARGET_OS_IPHONE
static BOOL phoneSupported;
#endif
static float gain = 1;
static pthread_mutex_t routeLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned errors;
static NSTimeInterval now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
// A plain player cannot loop, and one stopped in the middle of an event leaves an Xbox controller's motors running at its level after the engine and the
// process are gone (gamecontrollerd, macOS 27.0.1; only power-cycling the controller stopped them). So plain players play pulses that end on their own:
// each one PULSE long, the next scheduled where the last ends while the effect lasts; a stop silences them and waits for the last one to end
#define PULSE 0.5
#define PULSE_LEAD 0.15   // the next pulse is made this long before the last one ends
static CHHapticPattern *simple_pattern(dm_haptic_level level, NSError **error) {
    CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]
    ] relativeTime:0 duration:PULSE];
    // The level is the pattern's own initial control: a plain player has no mute to hide its first instant at full intensity
    return [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level.intensity relativeTime:0],
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:level.sharpness - 0.5f relativeTime:0]
    ] error:error];
}
// DM_RUMBLE_MIN (0 to 0.9, default 0): controller levels below it stay silent and the rest is stretched over the full range. On a Joy-Con's
// resonant actuator a game's quiet texture is felt as texture; on rotating motors it is rumble that never lets up (Mario Kart 8 Deluxe holds its
// low band at 0.1-0.4 all race long: 0.45 leaves crashes and drifts). The mapping stays linear by default: the opposite need, low levels that
// rotating motors do not render at all (Breath of the Wild's charged attack on an Xbox controller), is open (docs/reference/services.md)
static float rumble_min;
static float controller_level(float x) {
    x = isfinite(x) ? fmaxf(0, fminf(1, x)) : 0;
    return x <= rumble_min ? 0 : (x - rumble_min) / (1 - rumble_min);
}
static void report(NSError *e, NSString *target, const char *stage) {
    if (++errors <= 8 || errors % 64 == 0) {
        fprintf(stderr, "[RED] haptics: %s %s: %s (%s %ld)\n", target.UTF8String, stage,
                e.localizedDescription.UTF8String ?: "engine unavailable", e.domain.UTF8String ?: "no error", (long)e.code);
        NSError *underlying = e.userInfo[NSUnderlyingErrorKey];
        if (underlying) fprintf(stderr, "[RED] haptics: underlying %s %ld: %s\n", underlying.domain.UTF8String,
                                (long)underlying.code, underlying.userInfo[NSDebugDescriptionErrorKey] ?
                                [underlying.userInfo[NSDebugDescriptionErrorKey] UTF8String] : underlying.localizedDescription.UTF8String);
    }
}

@implementation DmRumbleChannel
// Core Haptics raises an NSException, not an NSError, when a player call reaches an engine that is not running ("-[CHHapticEngine(CHHapticEngineInternal)
// checkEngineRunning:]: self.running"), and it offers no running state to ask first: the engine stops on its own at any time (an audio interruption,
// the haptics helper's exit) while its stopped handler is only queued here, and a stop is asynchronous. So every entry into a channel (apply:, stop,
// the queued renewals and stops) runs its Core Haptics calls through guarded:work:, and a raise drops the engine as a failed one. Uncaught, it ended
// Mario Kart World on an iPhone (2026-11-07); which call raised there is not known. The host is ARC without -fobjc-arc-exceptions, so a raise leaks
// what ARC held in the frames it unwinds: the player whose call raised (and so perhaps its dead engine) and the parameters sent to it, once per engine
// failure (t_haptics_apple under leaks: about 300 bytes a raise, none with the flag). The flag would cover the whole host, or need a per-file rule in
// both the Makefile and the iOS project; not worth it for this
- (void)guarded:(const char *)stage work:(void (^)(void))work {
    @try { work(); }
    @catch (NSException *x) { [self stoppedEngine:x stage:stage]; }
}
// Like a failed start (failed:), except that neither the engine nor its players are called again: a stop sends the players zero, which raises once more.
// Nothing plays from an engine that is not running. The next effect makes a new engine after the backoff
- (void)stoppedEngine:(NSException *)x stage:(const char *)stage {
    if (++errors <= 8 || errors % 64 == 0)
        fprintf(stderr, "[RED] haptics: %s %s on a stopped engine (%s: %s)\n", self.target.UTF8String, stage,
                x.name.UTF8String ?: "exception", x.reason.UTF8String ?: "no reason");
    self.generation++; self.silenceGeneration++;   // its renewals, idle release and callbacks are stale
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; }; self.engine.resetHandler = ^{};
    self.engine = nil; self.pattern = nil; self.previous = nil; self.level = (dm_haptic_level){0, 0};
    self.failures = MIN(self.failures + 1, 4u);
    self.retryAfter = now() + (1u << (self.failures - 1));
    dm_haptics_clear(self.player);
}
- (void)stop { [self guarded:"stop" work:^{ [self halt]; }]; }
// stop's work, for callers already inside guarded:work:
- (void)halt {
    self.generation++; self.silenceGeneration++;
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; }; self.engine.resetHandler = ^{};
    if (self.simple) {
        // Never cut a pulse: silence the playing ones and stop the engine once the last has ended
        NSArray *zero = @[[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:0 relativeTime:0]];
        [self.previous sendParameters:zero atTime:CHHapticTimeImmediate error:nil];
        [self.pattern sendParameters:zero atTime:CHHapticTimeImmediate error:nil];
        self.pattern = nil; self.previous = nil; self.level = (dm_haptic_level){0, 0};
        NSTimeInterval left = self.engine ? self.pulseEnd - self.engine.currentTime : 0;
        if (left > 0) {
            __weak DmRumbleChannel *weak = self; CHHapticEngine *engine = self.engine;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((left + 0.05) * NSEC_PER_SEC)), queue, ^{
                DmRumbleChannel *c = weak; if (c && c.engine == engine && c.pattern) return;   // a new effect took the engine meanwhile
                @try { [engine stopWithCompletionHandler:nil]; }
                @catch (NSException *x) { if (c && c.engine == engine) [c stoppedEngine:x stage:"stop engine"]; }   // one no channel holds is gone anyway
            });
            return;
        }
    } else {
        [self.pattern stopAtTime:CHHapticTimeImmediate error:nil]; self.pattern = nil;
    }
    [self.engine stopWithCompletionHandler:nil];   // the engine stays: the next effect restarts it
}
// The next pulse at the current level, from where the last one ends, and the renewal of the one after while the effect lasts
- (BOOL)pulse:(NSError **)error {
    id<CHHapticPatternPlayer> next = [self.engine createPlayerWithPattern:simple_pattern(self.level, error) error:error];
    NSTimeInterval at = MAX(self.engine.currentTime, self.pulseEnd);
    if (!next || ![next startAtTime:at error:error]) return NO;
    self.previous = self.pattern; self.pattern = next; self.pulseEnd = at + PULSE;
    __weak DmRumbleChannel *weak = self; uint64_t generation = self.generation;
    NSTimeInterval wait = MAX(0, self.pulseEnd - PULSE_LEAD - self.engine.currentTime);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_SEC)), queue, ^{
        DmRumbleChannel *c = weak;
        if (!c || c.generation != generation || c.pattern != next || c.level.intensity == 0) return;   // stopped, replaced or silent
        [c guarded:"pulse" work:^{ NSError *e = nil; if (![c pulse:&e]) [c failed:e stage:"pulse"]; }];
    });
    return YES;
}
- (void)failed:(NSError *)error stage:(const char *)stage {   // inside guarded:work: (apply: or a renewal), which handles a raise from halt
    report(error, self.target, stage);
    [self halt]; self.engine = nil;   // A failed helper connection must not be reused for every later vibration.
    self.failures = MIN(self.failures + 1, 4u);
    self.retryAfter = now() + (1u << (self.failures - 1));
    dm_haptics_clear(self.player);
}
- (void)engineFailed:(NSError *)error stage:(const char *)stage {
    [self failed:error stage:stage];
    // Only an engine that cannot be made or started moves a split route to the default locality;
    // a player or parameter failure (a service reset, say) is retried on the same handle.
    if (self.fallback) self.fallback(self.retryAfter);
}
- (BOOL)startEngine {
    NSError *error = nil;
    if (!self.engine) {
        self.engine = self.createEngine(&error);
        if (!self.engine) { [self engineFailed:error stage:"create engine"]; return NO; }
        self.engine.playsHapticsOnly = YES; self.engine.autoShutdownEnabled = NO;
    }
    __weak DmRumbleChannel *weak = self;
    uint64_t generation = ++self.generation;
    void (^reset)(void) = ^{
        dispatch_async(queue, ^{
            DmRumbleChannel *c = weak; if (!c || c.generation != generation) return;
            [c stop];
            dm_haptics_clear(c.player);   // queued effects from before an interruption cannot restart
        });
    };
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; reset(); };
    self.engine.resetHandler = reset;
    // Startup can itself fail or reset the service: install handlers before contacting it.
    if (![self.engine startAndReturnError:&error]) { [self engineFailed:error stage:"start engine"]; return NO; }
    return YES;
}
- (void)apply:(dm_haptic_level)level { [self guarded:"apply" work:^{ [self applyLevel:level]; }]; }
- (void)applyLevel:(dm_haptic_level)level {
    if ([self.target hasPrefix:@"controller"]) level.intensity = controller_level(level.intensity);
    uint64_t silence = ++self.silenceGeneration;
    NSError *error = nil;
    self.level = level;
    if (!self.pattern) {
        if (level.intensity == 0) return;
        if (now() < self.retryAfter) {
            // Discard this effect, so an identical fresh send after the backoff is not deduplicated away.
            dm_haptics_clear(self.player); return;
        }
        if (![self startEngine]) return;
        if (!self.simple) {
            CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[
                [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
                [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]
            ] relativeTime:0 duration:30];
            CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
            id<CHHapticAdvancedPatternPlayer> advanced = pattern ? [self.engine createAdvancedPlayerWithPattern:pattern error:&error] : nil;
            if (advanced) { advanced.loopEnabled = YES; advanced.isMuted = YES; self.pattern = advanced; }
            else if (pattern) {
                // Game controllers' haptic server (gamecontrollerd, macOS 27) reports no advanced players to CoreHaptics ("adv players: 0" in its log), and
                // creating one fails over XPC (NSCocoaErrorDomain 4097): the Xbox controllers never vibrated. The refusal also leaves that engine unusable
                // (a plain player or a restart on it fails with -4810), so plain players play from a new engine
                fprintf(stderr, "haptics: player %d %s has no advanced players (%s %ld): plain players\n", self.player + 1, self.target.UTF8String,
                        error.domain.UTF8String ?: "no error", (long)error.code);
                error = nil;
                [self halt]; self.engine = nil;
                // Without advanced players, the engines of the left and right handles override each other on an Xbox Wireless Controller instead of
                // adding up: with both playing, the handle motors were not felt, while one engine for both handles was
                if (self.merge) { dm_haptics_clear(self.player); self.merge(); return; }
                self.simple = YES;
                if (![self startEngine]) return;
            }
        }
        if (self.simple) {
            self.pulseEnd = 0;
            if (![self pulse:&error]) { [self failed:error stage:"start pulse"]; return; }
        }
        if (!self.pattern) { [self failed:error stage:"create player"]; return; }
        if (!self.simple && ![self.pattern startAtTime:CHHapticTimeImmediate error:&error]) { [self failed:error stage:"start player"]; return; }
    } else if (self.simple && level.intensity > 0 && self.pulseEnd <= self.engine.currentTime) {
        if (![self pulse:&error]) { [self failed:error stage:"start pulse"]; return; }   // the pulses ran out while the effect was silent
    }
    // Controls live outside the loop: the loop boundary never replays an initial intensity.
    NSArray *parameters = @[
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level.intensity relativeTime:0],
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:level.sharpness - 0.5f relativeTime:0]
    ];
    [self.previous sendParameters:parameters atTime:CHHapticTimeImmediate error:nil];   // still playing until the scheduled pulse starts
    if (![self.pattern sendParameters:parameters atTime:CHHapticTimeImmediate error:&error]) {
        [self failed:error stage:"send parameters"]; return;
    }
    self.failures = 0;
    if (!self.simple) ((id<CHHapticAdvancedPatternPlayer>)self.pattern).isMuted = NO;
    if (level.intensity == 0) {
        // Reuse a warm player for nearby pulses, then release idle hardware. This never times out an active effect.
        __weak DmRumbleChannel *weak = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), queue, ^{
            DmRumbleChannel *c = weak; if (c && c.silenceGeneration == silence) [c stop];
        });
    }
}
@end

#if !TARGET_OS_IPHONE
// Controllers whose rumble Core Haptics does not reach, driven through their own HID output report as SDL's hidapi drivers do:
// - a DualShock 4, or a controller that presents itself as one (a GameSir Cyclone over Bluetooth: Sony 054C / 05C4, whose Core Haptics
//   helper failed on every locality on macOS 27.0.1): its effects report, 0x11 over Bluetooth (78 bytes, CRC32 over 0xA2 and the first
//   74 bytes, rumble only, so the light bar is left alone), 0x05 over USB (32 bytes), motors 0-255;
// - an Amazon Luna controller over Bluetooth LE (0171 / 0419), for which GameController offers no haptic locality: output report 0x03,
//   the PID Set Effect report its descriptor declares and Xbox One controllers take over Bluetooth, magnitudes 0-100 at bytes 4 (left,
//   strong) and 5 (right, weak), duration 0xFF and loop count 0xEB as SDL sends them; the motors keep the last magnitude until the next
//   report. macOS writes HID output reports to a third-party Bluetooth LE device as ATT Write Requests; firmware 305164020 never answered
//   them, so every report blocked 30 s and the link was dropped; 305165720 answers at once (docs/reference/services.md).
// Unlike the handle engines of a server without advanced players, the reports keep the strong (left) and weak (right) motor apart.
enum { PAD_DS4, PAD_LUNA };
static const struct { const char *name; uint16_t vendor, product[3]; float scale; } pads[] = {
    [PAD_DS4] = {"DualShock 4 effects report", 0x054C, {0x05C4, 0x09CC, 0x0BA0}, 255},   // DualShock 4 v1, v2, the USB wireless adapter
    [PAD_LUNA] = {"Luna output report", 0x0171, {0x0419}, 100},
};
static uint32_t ds4_crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}
static size_t pad_report(int kind, BOOL bluetooth, const uint8_t motor[2], uint8_t r[78]) {
    memset(r, 0, 78);
    if (kind == PAD_LUNA) {
        const uint8_t l[9] = {0x03, 0x0F, 0, 0, motor[0], motor[1], 0xFF, 0x00, 0xEB}; memcpy(r, l, sizeof l); return sizeof l;
    }
    if (!bluetooth) { r[0] = 0x05; r[1] = 0x01; r[4] = motor[1]; r[5] = motor[0]; return 32; }
    r[0] = 0x11; r[1] = 0xC4; r[3] = 0x01; r[6] = motor[1]; r[7] = motor[0];
    uint8_t c[75] = {0xA2}; memcpy(c + 1, r, 74);
    uint32_t crc = ds4_crc(c, sizeof c); r[74] = crc; r[75] = crc >> 8; r[76] = crc >> 16; r[77] = crc >> 24;
    return 78;
}
// The reports go out on a queue of their own: IOHIDDeviceSetReport waits for the device's answer, up to the HID device's RequestTimeout
// (30 s for a Bluetooth LE device), and the rumble queue, which controller changes wait for, must not wait with it. Only the newest
// motor levels are sent; levels that change while a report is out are merged into the next one.
static IOReturn (*pad_write)(IOHIDDeviceRef, IOHIDReportType, CFIndex, const uint8_t *, CFIndex) = IOHIDDeviceSetReport;
static dispatch_queue_t padQueue;
@interface DmPadOutput : NSObject {
@public
    IOHIDDeviceRef dev; uint64_t id; int kind, player; BOOL bluetooth;   // id: its registry entry; the device is open while the output lives
    _Atomic uint32_t want; _Atomic bool busy;   // the levels to send (left | right << 8) and whether a pump is queued; set from the rumble queue
    uint32_t sent; BOOL dead;   // padQueue only
}
@end
@implementation DmPadOutput
- (void)dealloc { if (dev) { IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone); CFRelease(dev); } }   // after the pump's last report
@end
static DmPadOutput *pads_out[DM_PLAYERS];   // the rumble queue's
static BOOL padLog;   // DM_RUMBLE_LOG: once a second, the reports sent, the time their writes took and the peak levels
static void pad_log(DmPadOutput *o, uint32_t v, double ms) {
    static double since, total, longest; static unsigned count; static uint8_t peak[2];
    double t = now();
    if (!since) since = t;
    count++; total += ms; longest = fmax(longest, ms);
    for (int m = 0; m < 2; m++) peak[m] = MAX(peak[m], (uint8_t)(v >> 8 * m));
    if (t - since < 1) return;
    fprintf(stderr, "haptics: player %d %s: %u reports in %.1f s, write %.1f ms average / %.1f ms longest, peak levels %u / %u of %.0f\n", o->player + 1,
            pads[o->kind].name, count, t - since, total / count, longest, peak[0], peak[1], pads[o->kind].scale);
    since = t; count = 0; total = longest = 0; peak[0] = peak[1] = 0;
}
static void pad_pump(DmPadOutput *o) {
    for (;;) {
        uint32_t v = atomic_load(&o->want);
        if (o->dead || v == o->sent) {
            atomic_store(&o->busy, false);
            // levels stored after the load found the pump busy and did not queue one: take them
            if (o->dead || atomic_load(&o->want) == o->sent || atomic_exchange(&o->busy, true)) return;
            continue;
        }
        uint8_t motor[2] = {v, v >> 8}, r[78];
        size_t n = pad_report(o->kind, o->bluetooth, motor, r);
        double start = now();
        IOReturn e = pad_write(o->dev, kIOHIDReportTypeOutput, r[0], r, n);
        if (padLog) pad_log(o, v, (now() - start) * 1000);
        o->sent = v;   // a failed report is not repeated: the next change sends again
        if (e == kIOReturnTimeout) {
            // The device took the report but never answered (each such report also drops a Bluetooth LE link): send no more
            o->dead = YES;
            fprintf(stderr, "[RED] haptics: player %d %s timed out: no more rumble on this controller until it reconnects%s\n", o->player + 1,
                    pads[o->kind].name, o->kind == PAD_LUNA ? " (Luna firmware 305164020 never answers; update it with the Luna Controller app)" : "");
        } else if (e != kIOReturnSuccess && (++errors <= 8 || errors % 64 == 0))
            fprintf(stderr, "[RED] haptics: player %d %s: IOReturn 0x%x\n", o->player + 1, pads[o->kind].name, e);
    }
}
static void pad_set(DmPadOutput *o, const uint8_t motor[2]) {
    atomic_store(&o->want, motor[0] | (uint32_t)motor[1] << 8);
    if (!atomic_exchange(&o->busy, true)) dispatch_async(padQueue, ^{ pad_pump(o); });
}
// As the emulators feed SDL: the strong motor follows the low band and the weak one the high band, mostly from the left and the right motor
// respectively, linearly. The combined level of the Core Haptics route (dm_haptics_level) would run both motors on either band.
static void pad_apply(dm_haptic_state s) {
    DmPadOutput *o = pads_out[s.player];
    if (!o) return;
    const dm_vibration *l = &s.motor[0], *r = &s.motor[1];
    float scale = pads[o->kind].scale;
    uint8_t motor[2] = {(uint8_t)lrintf(controller_level((0.85f * l->amplitude_low + 0.15f * r->amplitude_low) * gain) * scale),
                        (uint8_t)lrintf(controller_level((0.15f * l->amplitude_high + 0.85f * r->amplitude_high) * gain) * scale)};
    pad_set(o, motor);
}
static void pad_release(int p) {
    if (pads_out[p]) pad_set(pads_out[p], (const uint8_t[2]){0, 0});   // the pump keeps the output until the stop is out
    pads_out[p] = nil;
}
@interface DmPadChannel : DmRumbleChannel   // one per motor; the route takes both motors' values at once (pad_apply)
@end
@implementation DmPadChannel
- (void)apply:(dm_haptic_level)level { (void)level; }
- (void)stop { if (pads_out[self.player]) pad_set(pads_out[self.player], (const uint8_t[2]){0, 0}); }
@end
static int pad_kind(GCController *controller) {
    if ([controller.productCategory isEqualToString:GCProductCategoryDualShock4]) return PAD_DS4;
    if ([controller.vendorName hasPrefix:@"Luna"]) return PAD_LUNA;
    return -1;
}
// The first HID device of this kind no other player drives, opened for player p. The registry is asked at each connect: an IOHIDManager that is never
// scheduled keeps the devices of the moment its matching was set, so a controller turned on later found none and stayed on Core Haptics, and one that
// came back got its dead device. A scheduled one would depend on its arrival callback running before GameController's connect; the registry has the
// device by then
static DmPadOutput *pad_take(int p, int kind) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey); io_iterator_t it = 0;
    if (!match) return nil;
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), (__bridge CFNumberRef)@(pads[kind].vendor));
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return nil;   // takes match
    DmPadOutput *o = nil;
    for (io_service_t s; !o && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        uint64_t id = 0; BOOL taken = NO;   // GameController does not name a controller's HID device: two connecting together may get each other's
        if (IORegistryEntryGetRegistryEntryID(s, &id) != KERN_SUCCESS) continue;
        for (int q = 0; q < DM_PLAYERS; q++) taken |= q != p && pads_out[q] && pads_out[q]->id == id;
        IOHIDDeviceRef dev = taken ? NULL : IOHIDDeviceCreate(kCFAllocatorDefault, s);
        if (!dev) continue;
        int pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey)) intValue];
        BOOL known = NO;
        for (size_t i = 0; i < sizeof pads[kind].product / sizeof *pads[kind].product; i++) known |= pid && pads[kind].product[i] == pid;
        if (!known || IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) { CFRelease(dev); continue; }
        o = [DmPadOutput new];
        o->dev = dev; o->id = id; o->kind = kind; o->player = p;
        o->bluetooth = [(__bridge NSString *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)) hasPrefix:@"Bluetooth"];   // a Luna: "Bluetooth Low Energy"
    }
    IOObjectRelease(it);
    return o;
}
// Only a controller GameController names as one of these, and only while such a HID device is free
static BOOL pad_route(int p, GCController *controller) {
    int kind = pad_kind(controller);
    DmPadOutput *o = kind < 0 ? nil : pad_take(p, kind);
    if (!o) return NO;
    pads_out[p] = o;
    for (int m = 0; m < 2; m++) { DmPadChannel *c = [DmPadChannel new]; c.player = p; c.motor = m; c.target = @(pads[kind].name); [channels[p] addObject:c]; }
    fprintf(stderr, "haptics: player %d controller %s, rumble through the %s (%s, product 0x%04x)\n", p + 1, controller.vendorName.UTF8String ?: "unknown",
            pads[kind].name, [(__bridge NSString *)IOHIDDeviceGetProperty(o->dev, CFSTR(kIOHIDTransportKey)) UTF8String] ?: "unknown transport",
            [(__bridge NSNumber *)IOHIDDeviceGetProperty(o->dev, CFSTR(kIOHIDProductIDKey)) intValue]);
    return YES;
}
#endif

static void (*sinks[DM_PLAYERS])(const dm_haptic_state *);   // a player's own output instead of engines (dm_haptics_apple_set_sink)
static void sink_apply(dm_haptic_state s) {   // on the queue: Rumble strength and cutoff, as the engines get them
    for (int m = 0; m < 2; m++) { s.motor[m].amplitude_low = controller_level(s.motor[m].amplitude_low * gain); s.motor[m].amplitude_high = controller_level(s.motor[m].amplitude_high * gain); }
    sinks[s.player](&s);
}
static void sink_silence(int p) { dm_haptic_state z = {0}; z.player = p; if (sinks[p]) sinks[p](&z); }
static void apply(dm_haptic_state s) {
    dispatch_async(queue, ^{ @autoreleasepool {
        if (s.epoch != dm_haptics_epoch(s.player)) return;
        if (sinks[s.player]) { sink_apply(s); return; }   // the player is in range, as channels[] below assumes (hle_haptics.c)
        NSArray<DmRumbleChannel *> *out = channels[s.player];
#if !TARGET_OS_IPHONE
        if ([out.firstObject isKindOfClass:DmPadChannel.class]) { pad_apply(s); return; }
#endif
        for (NSUInteger i = 0; i < out.count; i++) {
            if (s.epoch != dm_haptics_epoch(s.player)) break;   // a failed channel may replace this route
            int m = out[i].motor;
            dm_haptic_level level = m >= 0 ? dm_haptics_level(&s.motor[m], 1, gain) : dm_haptics_level(s.motor, 2, gain);
            [out[i] apply:level];
        }
    } });
}
static void add_engine(int p, int motor, NSString *target, CHHapticEngine *(^create)(NSError **)) {
    DmRumbleChannel *c = [DmRumbleChannel new]; c.createEngine = create; c.target = target; c.player = p; c.motor = motor;
    [channels[p] addObject:c];
}
static void default_controller(int p, GCDeviceHaptics *h, NSTimeInterval retryAfter) {
    add_engine(p, -1, @"controller default", ^CHHapticEngine *(NSError **e) { (void)e; return [h createEngineWithLocality:GCHapticsLocalityDefault]; });
    channels[p].lastObject.retryAfter = retryAfter;
}
static void configure(int p, GCController *controller) {
    for (DmRumbleChannel *c in channels[p]) [c stop];
    channels[p] = [NSMutableArray new];
    if (sinks[p]) { sink_silence(p); dm_haptics_route(p, 1); return; }
#if !TARGET_OS_IPHONE
    pad_release(p);
    if (controller && pad_route(p, controller)) { dm_haptics_route(p, 1); return; }   // the report first, as SDL drives it
#endif
    if (controller) {
        GCDeviceHaptics *h = controller.haptics;
        NSSet *localities = h.supportedLocalities;
        if ([localities containsObject:GCHapticsLocalityLeftHandle] && [localities containsObject:GCHapticsLocalityRightHandle]) {
            add_engine(p, 0, @"controller left handle", ^CHHapticEngine *(NSError **e) { (void)e; return [h createEngineWithLocality:GCHapticsLocalityLeftHandle]; });
            add_engine(p, 1, @"controller right handle", ^CHHapticEngine *(NSError **e) { (void)e; return [h createEngineWithLocality:GCHapticsLocalityRightHandle]; });
            // Advertised handle localities can still fail to start. Fall back to the standard
            // controller engine, once, rather than keeping two unusable helper connections.
            __weak NSArray *route = channels[p];
            for (DmRumbleChannel *c in channels[p]) c.fallback = ^(NSTimeInterval retryAfter) {
                if (!route || channels[p] != route) return;
                for (DmRumbleChannel *old in channels[p]) [old stop];
                channels[p] = [NSMutableArray new];
                default_controller(p, h, retryAfter);
                dm_haptics_route(p, 1);
                fprintf(stderr, "haptics: player %d retrying with the default controller locality\n", p + 1);
            };
            // A server without advanced players gets one engine for both handles (see apply)
            for (DmRumbleChannel *c in channels[p]) c.merge = ^{
                if (!route || channels[p] != route) return;
                for (DmRumbleChannel *old in channels[p]) [old stop];
                channels[p] = [NSMutableArray new];
                GCHapticsLocality both = [localities containsObject:GCHapticsLocalityHandles] ? GCHapticsLocalityHandles : GCHapticsLocalityDefault;
                add_engine(p, -1, @"controller handles", ^CHHapticEngine *(NSError **e) { (void)e; return [h createEngineWithLocality:both]; });
                channels[p].lastObject.simple = YES;
                dm_haptics_route(p, 1);
                fprintf(stderr, "haptics: player %d plays both motors on one engine (%s)\n", p + 1, both.UTF8String);
            };
        }
        else if (localities.count) default_controller(p, h, 0);
        fprintf(stderr, "haptics: player %d controller %s, localities %s, %lu channels\n", p + 1,
                controller.vendorName.UTF8String ?: "unknown", [[localities.allObjects sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@", "].UTF8String ?: "none", (unsigned long)channels[p].count);
    }
#if TARGET_OS_IPHONE
    else if (!p && phoneSupported) {
        // Do not initialize the phone's engine before the game's audio session is active.
        add_engine(p, -1, @"phone", ^CHHapticEngine *(NSError **e) { return [[CHHapticEngine alloc] initWithAudioSession:AVAudioSession.sharedInstance error:e]; });
    }
#endif
    dm_haptics_route(p, channels[p].count != 0);
}
void dm_haptics_apple_set_sink(int p, void (*sink)(const dm_haptic_state *)) {
    if (!queue || p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&routeLock);
    dm_haptics_route(p, 0);   // invalidate queued work before changing the output
    dispatch_sync(queue, ^{ sink_silence(p); sinks[p] = sink; configure(p, controllers[p]); });
    pthread_mutex_unlock(&routeLock);
}
void dm_haptics_apple_controller(int p, GCController *controller) {
    if (!queue || p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&routeLock);
    if (controllers[p] != controller) {
        controllers[p] = controller;
        dm_haptics_route(p, 0);   // invalidate queued work before changing hardware
        dispatch_sync(queue, ^{ configure(p, controller); });
    }
    pthread_mutex_unlock(&routeLock);
}
static void suspend(unsigned reason, BOOL on) {
    dm_haptics_suspend(reason, on);
    void (^stop)(void) = ^{
        for (int p = 0; p < DM_PLAYERS; p++) { for (DmRumbleChannel *c in channels[p]) [c stop]; sink_silence(p); }
    };
    if (on) { if (dispatch_get_specific(&queue)) stop(); else dispatch_sync(queue, stop); }
}
void dm_haptics_apple_shutdown(void) {
    if (!queue) return;
    suspend(DM_HAPTICS_INACTIVE, YES);
#if !TARGET_OS_IPHONE
    // The HID reports' motors run until the next report: let the stops just queued go out before the process ends (a device that does
    // not answer cannot hold the exit longer than this)
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(padQueue, ^{ dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, dispatch_walltime(NULL, NSEC_PER_SEC));
#endif
}
void dm_haptics_apple_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("dm.rumble", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(queue, &queue, &queue, NULL);
#if !TARGET_OS_IPHONE
        padQueue = dispatch_queue_create("dm.rumble.hid", DISPATCH_QUEUE_SERIAL);
        padLog = getenv("DM_RUMBLE_LOG") != NULL;
#endif
        const char *e = getenv("DM_RUMBLE_GAIN");
        if (e) { float v = strtof(e, NULL); if (isfinite(v)) gain = fmaxf(0, fminf(3, v)); }
        e = getenv("DM_RUMBLE_MIN");
        if (e) { float v = strtof(e, NULL); if (isfinite(v)) rumble_min = fmaxf(0, fminf(0.9f, v)); }
        e = getenv("DM_RUMBLE");
        dm_haptics_init(apply, !e || atoi(e) != 0);
#if TARGET_OS_IPHONE
        phoneSupported = CHHapticEngine.capabilitiesForHardware.supportsHaptics;
#endif
        dispatch_sync(queue, ^{ configure(0, nil); });
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
#if TARGET_OS_IPHONE
        [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, YES); }];
        [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, NO); }];
        [nc addObserverForName:UIApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_haptics_apple_shutdown(); }];
        [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            suspend(DM_HAPTICS_INTERRUPTED, [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan);
        }];
        [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            (void)n; dm_haptics_clear(0);
        }];
#else
        [nc addObserverForName:NSApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, YES); }];
        [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, NO); }];
        [nc addObserverForName:NSApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_haptics_apple_shutdown(); }];
#endif
        atexit(dm_haptics_apple_shutdown);
    });
}
