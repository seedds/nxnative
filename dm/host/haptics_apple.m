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
#import <GameController/GCSyntheticDeviceKeys.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#include "pad_jc1.h"
#endif
#include <math.h>
#include <pthread.h>
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
static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * NSEC_PER_SEC + (uint64_t)t.tv_nsec;
}
static NSTimeInterval now(void) { return (double)now_ns() / 1e9; }
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
    // A handle engine that cannot be made or started may fall back to the default locality.
    // Other failures, and Joy-Cons whose localities must stay separate, retry the same actuator.
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
                // adding up: with both playing, the handle motors were not felt, while one engine for both handles was. A route with no merge
                // (one engine, a DualSense's handles, or separate Joy-Cons) keeps its channels
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
// A DualShock 4, or a controller that presents itself as one (a GameSir Cyclone over Bluetooth: Sony 054C / 05C4, whose Core Haptics helper
// failed on every locality on macOS 27.0.1), is driven through its own effects report, as SDL's hidapi driver does: 0x11 over Bluetooth
// (78 bytes, CRC32 over 0xA2 and the first 74 bytes, rumble only, so the light bar is left alone), 0x05 over USB (32 bytes). Unlike the
// handle engines of a server without advanced players, the report keeps the strong (left) and weak (right) motor apart.
typedef struct { IOHIDDeviceRef dev; uint64_t id; BOOL bluetooth; uint8_t motor[2], sent[2]; } ds4_out;   // id: its registry entry
static ds4_out ds4[DM_PLAYERS];
static uint32_t ds4_crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}
static void ds4_send(int p) {
    ds4_out *o = &ds4[p]; uint8_t r[78] = {0}; size_t n;
    if (o->bluetooth) {
        r[0] = 0x11; r[1] = 0xC4; r[3] = 0x01; r[6] = o->motor[1]; r[7] = o->motor[0]; n = 78;
        uint8_t c[75] = {0xA2}; memcpy(c + 1, r, 74);
        uint32_t crc = ds4_crc(c, sizeof c); r[74] = crc; r[75] = crc >> 8; r[76] = crc >> 16; r[77] = crc >> 24;
    } else { r[0] = 0x05; r[1] = 0x01; r[4] = o->motor[1]; r[5] = o->motor[0]; n = 32; }
    IOReturn e = IOHIDDeviceSetReport(o->dev, kIOHIDReportTypeOutput, r[0], r, n);
    if (e != kIOReturnSuccess) { if (++errors <= 8 || errors % 64 == 0) fprintf(stderr, "[RED] haptics: DualShock 4 effects report: IOReturn 0x%x\n", e); return; }
    o->sent[0] = o->motor[0]; o->sent[1] = o->motor[1];
}
// As the emulators feed SDL: the strong motor follows the low band and the weak one the high band, mostly from the left and the right motor
// respectively, linearly. The combined level of the Core Haptics route (dm_haptics_level) would run both motors on either band.
static void erm_mix(const dm_haptic_state *s, uint8_t out[2]) {
    const dm_vibration *l = &s->motor[0], *r = &s->motor[1];
    out[0] = (uint8_t)lrintf(controller_level((0.85f * l->amplitude_low + 0.15f * r->amplitude_low) * gain) * 255);
    out[1] = (uint8_t)lrintf(controller_level((0.15f * l->amplitude_high + 0.85f * r->amplitude_high) * gain) * 255);
}
static void ds4_apply(dm_haptic_state s) {
    ds4_out *o = &ds4[s.player];
    if (!o->dev) return;
    erm_mix(&s, o->motor);
    if (o->motor[0] == o->sent[0] && o->motor[1] == o->sent[1]) return;
    ds4_send(s.player);
}
static void ds4_release(int p) {
    ds4_out *o = &ds4[p];
    if (o->dev) { o->motor[0] = o->motor[1] = 0; if (o->sent[0] || o->sent[1]) ds4_send(p); IOHIDDeviceClose(o->dev, kIOHIDOptionsTypeNone); CFRelease(o->dev); }
    *o = (ds4_out){0};
}
@interface DmDs4Channel : DmRumbleChannel   // one per motor; the route takes both motors' values at once (ds4_apply)
@end
@implementation DmDs4Channel
- (void)apply:(dm_haptic_level)level { (void)level; }
- (void)stop {
    ds4_out *o = &ds4[self.player];
    o->motor[self.motor] = 0;
    if (o->dev && (o->sent[0] || o->sent[1]) && !o->motor[0] && !o->motor[1]) ds4_send(self.player);
}
@end
// A Switch Pro Controller, or a controller that presents itself as one (Nintendo 057E / 2009: genuine Pro Controller, 8BitDo, GameSir Nova, XTRE041), is driven
// through its own HD rumble report 0x10 (pad_jc1.h), as SDL's hidapi driver and pad_jc1_mac.h / pad_hid_mac.h send it. Core Haptics without advanced players merges
// both handles onto one engine and collapses both frequency bands into one intensity and sharpness, and on Switch-mode clones gamecontrollerd's output stays silent
// (PR #2607, issue #2804, PR #2787). Output reports are padded to 49 bytes over Bluetooth (JC1_REPORT_LEN) and 64 bytes over USB, after subcommand 0x48 enables
// vibration on connect. Changes are paced to PRO_MIN_NS with peak-hold (jc1_rumble_hold) so 5 ms pulses are not dropped and Bluetooth is not flooded at 200 Hz, and
// an active level is refreshed every PRO_REFRESH_NS.
#define PRO_MIN_NS     15000000ull
#define PRO_REFRESH_NS 30000000ull
typedef struct {
    IOHIDDeviceRef dev;
    uint64_t id, gen, last, due;   // id: its registry entry; gen: cancels stale dispatch_after ticks; last/due: CLOCK_MONOTONIC ns
    size_t len;                    // 49 over Bluetooth (JC1_REPORT_LEN), 64 over USB
    jc1_rumble_hold hold;          // peak-hold across the pacing interval (pad_jc1.h)
    uint8_t sent[8], counter;
    BOOL armed, bluetooth;
} pro_out;
static pro_out pro[DM_PLAYERS];
static void pro_encode_side(const dm_vibration *v, uint8_t out[4]) {
    if (!jc1_vibration_on(v)) { memcpy(out, jc1_neutral_rumble, 4); return; }
    jc1_rumble_encode(v, out);
}
static void pro_want(const pro_out *o, dm_vibration out[2]) {
    for (int k = 0; k < 2; k++) out[k] = jc1_vibration_on(&o->hold.peak[k]) ? o->hold.peak[k] : o->hold.cur[k];
}
static BOOL pro_send(int p, const uint8_t data[8]) {
    pro_out *o = &pro[p]; uint8_t r[64] = {0};
    jc1_rumble_report(r, o->counter++, data, data + 4);
    IOReturn e = IOHIDDeviceSetReport(o->dev, kIOHIDReportTypeOutput, r[0], r, (CFIndex)o->len);
    o->last = now_ns();
    if (e != kIOReturnSuccess) { if (++errors <= 8 || errors % 64 == 0) fprintf(stderr, "[RED] haptics: Pro Controller rumble report: IOReturn 0x%x\n", e); return NO; }
    memcpy(o->sent, data, 8);
    return YES;
}
static void pro_step(int p);
static void pro_arm(int p, uint64_t t, uint64_t target) {
    pro_out *o = &pro[p];
    if (o->armed && o->due <= target) return;
    uint64_t gen = ++o->gen; o->armed = YES; o->due = target;
    int64_t wait = target > t ? (int64_t)(target - t) : 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, wait), queue, ^{
        if (pro[p].gen != gen) return;
        pro[p].armed = NO;
        pro_step(p);
    });
}
static void pro_step(int p) {
    pro_out *o = &pro[p];
    if (!o->dev) return;
    dm_vibration m[2]; uint8_t data[8];
    pro_want(o, m);
    pro_encode_side(&m[0], data); pro_encode_side(&m[1], data + 4);
    BOOL on = jc1_vibration_on(&m[0]) || jc1_vibration_on(&m[1]);
    BOOL dirty = memcmp(data, o->sent, 8) != 0;
    if (!on && !dirty) return;
    uint64_t t = now_ns(), need = dirty ? PRO_MIN_NS : PRO_REFRESH_NS;
    if (!o->last || t >= o->last + need) {
        BOOL ok = pro_send(p, data);
        t = o->last;
        o->hold.peak[0] = o->hold.peak[1] = (dm_vibration){0};
        if (!ok && !on) return;
        pro_want(o, m);
        pro_encode_side(&m[0], data); pro_encode_side(&m[1], data + 4);
        on = jc1_vibration_on(&m[0]) || jc1_vibration_on(&m[1]);
        dirty = memcmp(data, o->sent, 8) != 0;
        if (!on && !dirty) return;
        need = dirty ? PRO_MIN_NS : PRO_REFRESH_NS;
    }
    pro_arm(p, t, o->last + need);
}
static void pro_apply(dm_haptic_state s) {
    pro_out *o = &pro[s.player];
    if (!o->dev) return;
    dm_vibration m[2];
    for (int i = 0; i < 2; i++) {
        m[i] = s.motor[i];
        m[i].amplitude_low = controller_level(m[i].amplitude_low * gain);
        m[i].amplitude_high = controller_level(m[i].amplitude_high * gain);
    }
    jc1_rumble_set(&o->hold, m);
    pro_step(s.player);
}
static void pro_release(int p) {
    pro_out *o = &pro[p];
    uint64_t gen = o->gen + 1;
    o->gen = gen; o->armed = NO;
    if (o->dev) {
        if (memcmp(o->sent, jc1_neutral_rumble, 8)) pro_send(p, jc1_neutral_rumble);
        IOHIDDeviceClose(o->dev, kIOHIDOptionsTypeNone); CFRelease(o->dev);
    }
    *o = (pro_out){0};
    o->gen = gen;
}
@interface DmProChannel : DmRumbleChannel   // one per motor; the route takes both motors' values at once (pro_apply)
@end
@implementation DmProChannel
- (void)apply:(dm_haptic_level)level { (void)level; }
- (void)stop {
    pro_out *o = &pro[self.player];
    o->hold.cur[self.motor] = o->hold.peak[self.motor] = (dm_vibration){0};
    if (jc1_vibration_on(&o->hold.cur[0]) || jc1_vibration_on(&o->hold.cur[1]) ||
        jc1_vibration_on(&o->hold.peak[0]) || jc1_vibration_on(&o->hold.peak[1])) return;
    o->gen++; o->armed = NO;
    if (o->dev && memcmp(o->sent, jc1_neutral_rumble, 8)) pro_send(self.player, jc1_neutral_rumble);
}
@end
// An 8BitDo Ultimate 2 Wireless in its own mode (2DC8:6012, firmware 1.03 and later): GameController lists it with no haptics localities ("localities none,
// 0 channels"), so Core Haptics cannot drive it. Its two rotating motors take output report 0x05 ({0x05, low motor, high motor, 0, 0}), as SDL's hidapi
// 8BitDo driver sends it. Changes are paced to ABITDO_MIN_NS with peak-hold across the pacing interval so 5 ms pulses are not dropped and Bluetooth LE is
// not flooded at 200 Hz, an active level is refreshed every ABITDO_REFRESH_NS, and a normal stop sends ABITDO_STOPS zero reports so a lost BLE stop does not
// leave the motors running. Uses the same linear erm_mix (DM_RUMBLE_GAIN, DM_RUMBLE_MIN) as the DualShock 4 route; only controllers with no localities of
// their own take it.
#define ABITDO_MIN_NS     20000000ull
#define ABITDO_REFRESH_NS 50000000ull
enum { ABITDO_STOPS = 3 };
typedef struct {
    IOHIDDeviceRef dev;
    uint64_t id, gen, last, due;   // id: its registry entry; gen: cancels stale dispatch_after ticks; last/due: CLOCK_MONOTONIC ns
    uint8_t cur[2], peak[2], sent[2];
    int stops;                     // zero reports still to send on a normal transition to silence
    BOOL armed;
} abitdo_out;
static abitdo_out abitdo[DM_PLAYERS];
static void abitdo_want(const abitdo_out *o, uint8_t out[2]) {
    for (int i = 0; i < 2; i++) out[i] = o->peak[i] ? o->peak[i] : o->cur[i];
}
static BOOL abitdo_send(int p, const uint8_t m[2]) {
    abitdo_out *o = &abitdo[p]; const uint8_t r[5] = {0x05, m[0], m[1], 0, 0};
    IOReturn e = IOHIDDeviceSetReport(o->dev, kIOHIDReportTypeOutput, r[0], r, sizeof r);
    o->last = now_ns();
    if (e != kIOReturnSuccess) { if (++errors <= 8 || errors % 64 == 0) fprintf(stderr, "[RED] haptics: 8BitDo output report 0x05: IOReturn 0x%x\n", e); return NO; }
    o->sent[0] = m[0]; o->sent[1] = m[1];
    return YES;
}
static void abitdo_step(int p);
static void abitdo_arm(int p, uint64_t t, uint64_t target) {
    abitdo_out *o = &abitdo[p];
    if (o->armed && o->due <= target) return;
    uint64_t gen = ++o->gen; o->armed = YES; o->due = target;
    int64_t wait = target > t ? (int64_t)(target - t) : 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, wait), queue, ^{
        if (abitdo[p].gen != gen) return;
        abitdo[p].armed = NO;
        abitdo_step(p);
    });
}
static void abitdo_step(int p) {
    abitdo_out *o = &abitdo[p];
    if (!o->dev) return;
    uint8_t m[2];
    abitdo_want(o, m);
    BOOL on = m[0] || m[1], dirty = m[0] != o->sent[0] || m[1] != o->sent[1], stopping = !on && (dirty || o->stops > 0);
    if (!on && !stopping) return;
    uint64_t t = now_ns(), need = (dirty || stopping) ? ABITDO_MIN_NS : ABITDO_REFRESH_NS;
    if (!o->last || t >= o->last + need) {
        BOOL ok = abitdo_send(p, m);
        t = o->last;
        o->peak[0] = o->peak[1] = 0;
        if (ok) { if (on) o->stops = ABITDO_STOPS; else if (o->stops > 0) o->stops--; }
        else if (!on) { o->stops = 0; return; }
        abitdo_want(o, m);
        on = m[0] || m[1]; dirty = m[0] != o->sent[0] || m[1] != o->sent[1]; stopping = !on && (dirty || o->stops > 0);
        if (!on && !stopping) return;
        need = (dirty || stopping) ? ABITDO_MIN_NS : ABITDO_REFRESH_NS;
    }
    abitdo_arm(p, t, o->last + need);
}
static void abitdo_apply(dm_haptic_state s) {
    abitdo_out *o = &abitdo[s.player];
    if (!o->dev) return;
    uint8_t m[2];
    erm_mix(&s, m);
    for (int i = 0; i < 2; i++) {
        o->cur[i] = m[i];
        if (m[i] > o->peak[i]) o->peak[i] = m[i];
    }
    abitdo_step(s.player);
}
static void abitdo_release(int p) {
    abitdo_out *o = &abitdo[p];
    uint64_t gen = o->gen + 1;
    o->gen = gen; o->armed = NO;
    if (o->dev) {
        if (o->sent[0] || o->sent[1]) { const uint8_t z[2] = {0, 0}; abitdo_send(p, z); }
        IOHIDDeviceClose(o->dev, kIOHIDOptionsTypeNone); CFRelease(o->dev);
    }
    *o = (abitdo_out){0};
    o->gen = gen;
}
@interface DmAbitdoChannel : DmRumbleChannel   // one per motor; the route takes both motors' values at once (abitdo_apply)
@end
@implementation DmAbitdoChannel
- (void)apply:(dm_haptic_level)level { (void)level; }
- (void)stop {
    abitdo_out *o = &abitdo[self.player];
    o->cur[self.motor] = o->peak[self.motor] = 0;
    if (o->cur[0] || o->cur[1] || o->peak[0] || o->peak[1]) return;
    o->gen++; o->armed = NO; o->stops = 0;
    if (o->dev && (o->sent[0] || o->sent[1])) { const uint8_t z[2] = {0, 0}; abitdo_send(self.player, z); }
}
@end
// The first non-synthetic HID device of the vendor, with one of the products, that no other player drives (ds4[] / pro[]: taken), opened for player p. The registry
// is asked at each connect: an IOHIDManager that is never scheduled keeps the devices of the moment its matching was set, so a controller turned on later found none
// and stayed on Core Haptics, and one that came back got its dead device. A scheduled one would depend on its arrival callback running before GameController's connect;
// the registry has the device by then. accept may refuse an opened device (it is then closed and the next one tried)
static IOHIDDeviceRef hid_take(int vendor, const int *pids, int npids, BOOL (^taken)(uint64_t id), BOOL (^accept)(IOHIDDeviceRef dev), uint64_t *taken_id) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey); io_iterator_t it = 0;
    if (!match) return NULL;
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), (__bridge CFNumberRef)@(vendor));
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return NULL;   // takes match
    IOHIDDeviceRef found = NULL;
    for (io_service_t s; !found && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        uint64_t id = 0;   // GameController does not name a controller's HID device: two connecting together may get each other's
        if (IORegistryEntryGetRegistryEntryID(s, &id) != KERN_SUCCESS || taken(id)) continue;
        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, s);
        if (!dev) continue;
        BOOL synthetic = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue;
        int pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey)) intValue]; BOOL want = NO;
        for (int i = 0; i < npids; i++) want |= pid == pids[i];
        if (!synthetic && want && IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
            if (!accept || accept(dev)) { found = dev; *taken_id = id; continue; }
            IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
        }
        CFRelease(dev);
    }
    IOObjectRelease(it);
    return found;
}
// Only a controller GameController calls a Pro Controller, and only while a live non-synthetic 057E:2009 HID device accepts subcommand 0x48 (enable vibration):
// after a long Bluetooth disconnect IOKit can keep a second entry of the pad whose output fails (MACH_SEND_INVALID_DEST, #2237), so this picks the live one
static BOOL pro_route(int p, GCController *controller) {
    if (![controller.productCategory isEqualToString:@"Switch Pro Controller"]) return NO;
    static const int pids[] = {0x2009};
    uint64_t id = 0;
    IOHIDDeviceRef dev = hid_take(0x057E, pids, 1, ^BOOL(uint64_t i) { for (int q = 0; q < DM_PLAYERS; q++) if (q != p && pro[q].dev && pro[q].id == i) return YES; return NO; },
                                  ^BOOL(IOHIDDeviceRef d) {
                                      BOOL bt = [(__bridge NSString *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDTransportKey)) isEqualToString:@"Bluetooth"];
                                      uint8_t r[64] = {0}, on = 1;
                                      jc1_output_report(r, 0, JC1_SUB_VIBRATION, &on, 1);
                                      return IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, r[0], r, (CFIndex)(bt ? JC1_REPORT_LEN : 64)) == kIOReturnSuccess;
                                  }, &id);
    if (!dev) return NO;
    pro_out *o = &pro[p];
    o->dev = dev; o->id = id; o->gen++; o->counter = 1;
    o->bluetooth = [(__bridge NSString *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)) isEqualToString:@"Bluetooth"];
    o->len = o->bluetooth ? JC1_REPORT_LEN : 64;
    memcpy(o->sent, jc1_neutral_rumble, 8);
    for (int m = 0; m < 2; m++) { DmProChannel *c = [DmProChannel new]; c.player = p; c.motor = m; c.target = @"Pro Controller report"; [channels[p] addObject:c]; }
    fprintf(stderr, "haptics: player %d controller %s, rumble through the Pro Controller rumble report (%s)\n", p + 1, controller.vendorName.UTF8String ?: "unknown",
            [(__bridge NSString *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)) UTF8String] ?: "unknown transport");
    return YES;
}
// The first free DualShock 4 device, opened for player p
static BOOL ds4_take(int p) {
    static const int pids[] = {0x05C4, 0x09CC, 0x0BA0};   // DualShock 4 v1, v2, the USB wireless adapter
    uint64_t id = 0;
    IOHIDDeviceRef dev = hid_take(0x054C, pids, 3, ^BOOL(uint64_t i) { for (int q = 0; q < DM_PLAYERS; q++) if (q != p && ds4[q].dev && ds4[q].id == i) return YES; return NO; }, nil, &id);
    if (!dev) return NO;
    ds4[p].dev = dev; ds4[p].id = id;
    ds4[p].bluetooth = [(__bridge NSString *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)) isEqualToString:@"Bluetooth"];
    return YES;
}
// Only a controller GameController calls a DualShock 4, and only while such a HID device is free
static BOOL ds4_route(int p, GCController *controller) {
    if (![controller.productCategory isEqualToString:GCProductCategoryDualShock4] || !ds4_take(p)) return NO;
    for (int m = 0; m < 2; m++) { DmDs4Channel *c = [DmDs4Channel new]; c.player = p; c.motor = m; c.target = @"DualShock 4 report"; [channels[p] addObject:c]; }
    fprintf(stderr, "haptics: player %d controller %s, rumble through the DualShock 4 effects report (%s, product 0x%04x)\n", p + 1, controller.vendorName.UTF8String ?: "unknown",
            ds4[p].bluetooth ? "Bluetooth" : "USB", [(__bridge NSNumber *)IOHIDDeviceGetProperty(ds4[p].dev, CFSTR(kIOHIDProductIDKey)) intValue]);
    return YES;
}
// Only a controller with no haptics of its own whose name says 8BitDo, while a free live non-synthetic 2DC8:6012 gamepad/joystick HID device accepts output
// report 0x05: probing a zero report skips dead Bluetooth entries (#2237) and firmware that lacks report 0x05
static BOOL abitdo_route(int p, GCController *controller) {
    if (!controller.vendorName || controller.haptics.supportedLocalities.count ||
        [controller.vendorName rangeOfString:@"8BitDo" options:NSCaseInsensitiveSearch].location == NSNotFound) return NO;
    static const int pids[] = {0x6012};
    uint64_t id = 0;
    IOHIDDeviceRef dev = hid_take(0x2DC8, pids, 1, ^BOOL(uint64_t i) { for (int q = 0; q < DM_PLAYERS; q++) if (q != p && abitdo[q].dev && abitdo[q].id == i) return YES; return NO; },
                                  ^BOOL(IOHIDDeviceRef d) {
                                      int page = [(__bridge NSNumber *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDPrimaryUsagePageKey)) intValue];
                                      int usage = [(__bridge NSNumber *)IOHIDDeviceGetProperty(d, CFSTR(kIOHIDPrimaryUsageKey)) intValue];
                                      if (page != 1 || (usage != 4 && usage != 5)) return NO;
                                      const uint8_t z[5] = {0x05, 0, 0, 0, 0};
                                      return IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, z[0], z, sizeof z) == kIOReturnSuccess;
                                  }, &id);
    if (!dev) return NO;
    abitdo_out *o = &abitdo[p];
    o->dev = dev; o->id = id; o->gen++;
    for (int m = 0; m < 2; m++) { DmAbitdoChannel *c = [DmAbitdoChannel new]; c.player = p; c.motor = m; c.target = @"8BitDo report"; [channels[p] addObject:c]; }
    fprintf(stderr, "haptics: player %d controller %s, rumble through the 8BitDo output report 0x05 (%s)\n", p + 1, controller.vendorName.UTF8String,
            [(__bridge NSString *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey)) UTF8String] ?: "unknown transport");
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
        if ([out.firstObject isKindOfClass:DmDs4Channel.class]) { ds4_apply(s); return; }
        if ([out.firstObject isKindOfClass:DmProChannel.class]) { pro_apply(s); return; }
        if ([out.firstObject isKindOfClass:DmAbitdoChannel.class]) { abitdo_apply(s); return; }
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
static GCHapticsLocality const kJoyConLeftLocality = (GCHapticsLocality)@"JoyCon (L)";
static GCHapticsLocality const kJoyConRightLocality = (GCHapticsLocality)@"JoyCon (R)";

static void configure(int p, GCController *controller) {
    for (DmRumbleChannel *c in channels[p]) [c stop];
    channels[p] = [NSMutableArray new];
#if !TARGET_OS_IPHONE
    ds4_release(p); pro_release(p); abitdo_release(p);
#endif
    if (sinks[p]) { sink_silence(p); dm_haptics_route(p, 1); return; }
#if !TARGET_OS_IPHONE
    if (controller && (ds4_route(p, controller) || pro_route(p, controller) || abitdo_route(p, controller))) { dm_haptics_route(p, 1); return; }   // the report first, as SDL drives it
#endif
    if (controller) {
        GCDeviceHaptics *h = controller.haptics;
        NSSet *localities = h.supportedLocalities;
        BOOL handles = [localities containsObject:GCHapticsLocalityLeftHandle] && [localities containsObject:GCHapticsLocalityRightHandle];
        BOOL joycons = !handles && [localities containsObject:kJoyConLeftLocality] && [localities containsObject:kJoyConRightLocality];
        if (handles || joycons) {
            GCHapticsLocality leftLoc = handles ? GCHapticsLocalityLeftHandle : kJoyConLeftLocality;
            GCHapticsLocality rightLoc = handles ? GCHapticsLocalityRightHandle : kJoyConRightLocality;
            add_engine(p, 0, handles ? @"controller left handle" : @"controller left Joy-Con", ^CHHapticEngine *(NSError **e) { (void)e; return [h createEngineWithLocality:leftLoc]; });
            add_engine(p, 1, handles ? @"controller right handle" : @"controller right Joy-Con", ^CHHapticEngine *(NSError **e) { (void)e; return [h createEngineWithLocality:rightLoc]; });
            // Advertised handle localities can still fail to start. Fall back to the standard controller engine once.
            // Joy-Cons can belong to different npads: Default mixes their motors, so a failed channel must retry its own locality.
            __weak NSArray *route = channels[p];
            if (handles) for (DmRumbleChannel *c in channels[p]) c.fallback = ^(NSTimeInterval retryAfter) {
                if (!route || channels[p] != route) return;
                for (DmRumbleChannel *old in channels[p]) [old stop];
                channels[p] = [NSMutableArray new];
                default_controller(p, h, retryAfter);
                dm_haptics_route(p, 1);
                fprintf(stderr, "haptics: player %d retrying with the default controller locality\n", p + 1);
            };
            // A server without advanced players gets one engine for both handles (see applyLevel), except for a DualSense or paired Joy-Cons:
            // plain players on the DualSense's handle engines are felt apart (PR #986), and JoyCon (L) / JoyCon (R) are two separate Bluetooth
            // controllers whose motors are routed per npad when split into JoyLeft and JoyRight
            if (handles && ![controller.productCategory isEqualToString:GCProductCategoryDualSense]) for (DmRumbleChannel *c in channels[p]) c.merge = ^{
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
    if (queue) suspend(DM_HAPTICS_INACTIVE, YES);
}
void dm_haptics_apple_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("dm.rumble", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(queue, &queue, &queue, NULL);
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
