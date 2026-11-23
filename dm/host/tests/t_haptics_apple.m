// Run the actual serial-queue adapter with fake engines: no controller, actuator or app is opened.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#include <assert.h>
#include <time.h>

static int test_clock_gettime(clockid_t clock, struct timespec *t);
static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta);
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t target, dispatch_block_t block);
static kern_return_t test_matching_services(mach_port_t port, CFDictionaryRef match CF_RELEASES_ARGUMENT, io_iterator_t *it);
static io_object_t test_iterator_next(io_iterator_t it);
static kern_return_t test_object_release(io_object_t o);
static kern_return_t test_entry_id(io_registry_entry_t s, uint64_t *id);
static IOHIDDeviceRef test_device_create(CFAllocatorRef a, io_service_t s);
static CFTypeRef test_device_property(IOHIDDeviceRef d, CFStringRef key);
static IOReturn test_device_open(IOHIDDeviceRef d, IOOptionBits o);
static IOReturn test_device_close(IOHIDDeviceRef d, IOOptionBits o);
static IOReturn test_device_report(IOHIDDeviceRef d, IOHIDReportType t, CFIndex id, const uint8_t *r, CFIndex n);
// dispatch_after need not submit its block before a sleep followed by dispatch_sync ends.
// Keep the real serial queue, but advance the adapter's clock and deliver its timers explicitly.
#define clock_gettime test_clock_gettime
#define dispatch_time test_dispatch_time
#define dispatch_after test_dispatch_after
// The HID devices are fakes too: the registry is the list the test keeps
#define IOServiceGetMatchingServices test_matching_services
#define IOIteratorNext test_iterator_next
#define IOObjectRelease test_object_release
#define IORegistryEntryGetRegistryEntryID test_entry_id
#define IOHIDDeviceCreate test_device_create
#define IOHIDDeviceGetProperty test_device_property
#define IOHIDDeviceOpen test_device_open
#define IOHIDDeviceClose test_device_close
#define IOHIDDeviceSetReport test_device_report
#import "../haptics_apple.m"
#undef IOServiceGetMatchingServices
#undef IOIteratorNext
#undef IOObjectRelease
#undef IORegistryEntryGetRegistryEntryID
#undef IOHIDDeviceCreate
#undef IOHIDDeviceGetProperty
#undef IOHIDDeviceOpen
#undef IOHIDDeviceClose
#undef IOHIDDeviceSetReport
#undef clock_gettime
#undef dispatch_time
#undef dispatch_after

@interface TestTimer : NSObject
@property dispatch_time_t deadline;
@property(copy) dispatch_block_t block;
@end
@implementation TestTimer
@end
static uint64_t testTime = 100 * NSEC_PER_SEC;
static NSMutableArray<TestTimer *> *timers;   // owned by the adapter's serial queue, like its engines
static int test_clock_gettime(clockid_t clock, struct timespec *t) {
    assert(clock == CLOCK_MONOTONIC);
    t->tv_sec = testTime / NSEC_PER_SEC; t->tv_nsec = testTime % NSEC_PER_SEC;
    return 0;
}
static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta) {
    assert(when == DISPATCH_TIME_NOW && delta >= 0);
    return testTime + delta;
}
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t target, dispatch_block_t block) {
    assert(target == queue && dispatch_get_specific(&queue));
    TestTimer *timer = [TestTimer new]; timer.deadline = when; timer.block = block;
    [timers addObject:timer];
}

@interface TestPlayer : NSObject
@property BOOL loopEnabled, isMuted;
@property int starts, stops;
@property NSMutableArray<NSNumber *> *levels;
@property BOOL failStart, failSend, raiseSend;   // raiseSend: the engine stopped under the player, as Core Haptics reports it, by raising
@end
@implementation TestPlayer
- (instancetype)init { if ((self = [super init])) _levels = [NSMutableArray new]; return self; }
- (BOOL)startAtTime:(NSTimeInterval)t error:(NSError **)e { (void)t; (void)e; self.starts++; return !self.failStart; }
- (BOOL)stopAtTime:(NSTimeInterval)t error:(NSError **)e { (void)t; (void)e; self.stops++; return YES; }
- (BOOL)sendParameters:(NSArray<CHHapticDynamicParameter *> *)p atTime:(NSTimeInterval)t error:(NSError **)e {
    (void)t; (void)e;
    if (self.raiseSend) [NSException raise:@"com.apple.CoreHaptics" format:@"-[CHHapticEngine checkEngineRunning:]: self.running"];
    if (self.failSend) return NO;
    for (CHHapticDynamicParameter *v in p) if ([v.parameterID isEqual:CHHapticDynamicParameterIDHapticIntensityControl]) [self.levels addObject:@(v.value)];
    return YES;
}
@end
@interface TestEngine : NSObject
@property BOOL playsHapticsOnly, autoShutdownEnabled;
@property(copy) void (^resetHandler)(void);
@property(copy) void (^stoppedHandler)(CHHapticEngineStoppedReason);
@property TestPlayer *testPlayer;
@property int starts, stops;
@property BOOL fail;
@property BOOL failPattern, failPlayer, failSend, handlersAtStart, resetDuringStart, noAdvanced, broken;
@property int plainPlayers;
@property(readonly) NSTimeInterval currentTime;
@property(copy) void (^startingReset)(void);
@end
@implementation TestEngine
- (NSTimeInterval)currentTime { return now(); }   // an engine's clock runs like the host's
- (BOOL)startAndReturnError:(NSError **)e {
    self.starts++; self.handlersAtStart = self.resetHandler && self.stoppedHandler;
    self.startingReset = self.resetHandler;
    if (self.resetDuringStart && self.resetHandler) self.resetHandler();
    if ((self.fail || self.broken) && e) *e = [NSError errorWithDomain:CoreHapticsErrorDomain code:CHHapticErrorCodeServerInitFailed userInfo:nil];
    return !self.fail && !self.broken;
}
- (void)stopWithCompletionHandler:(CHHapticCompletionHandler)handler { self.stops++; if (handler) handler(nil); }
- (id<CHHapticAdvancedPatternPlayer>)createAdvancedPlayerWithPattern:(CHHapticPattern *)p error:(NSError **)e {
    (void)p;
    if (self.failPattern || self.noAdvanced) {   // gamecontrollerd's answer when it offers no advanced players, after which the engine is unusable
        if (e) *e = [NSError errorWithDomain:NSCocoaErrorDomain code:NSXPCConnectionInterrupted userInfo:nil];
        self.broken = YES; return nil;
    }
    self.testPlayer = [TestPlayer new]; self.testPlayer.failStart = self.failPlayer; self.testPlayer.failSend = self.failSend;
    return (id)self.testPlayer;
}
- (id<CHHapticPatternPlayer>)createPlayerWithPattern:(CHHapticPattern *)p error:(NSError **)e {
    assert(p);
    if (self.failPattern || self.broken) {
        if (e) *e = [NSError errorWithDomain:CoreHapticsErrorDomain code:CHHapticErrorCodeServerInitFailed userInfo:nil];
        return nil;
    }
    self.plainPlayers++;
    self.testPlayer = [TestPlayer new]; self.testPlayer.failStart = self.failPlayer; self.testPlayer.failSend = self.failSend;
    return (id)self.testPlayer;
}
@end
@interface TestHaptics : NSObject
@property NSSet *supportedLocalities;
@property NSMutableArray<TestEngine *> *engines;
@property NSMutableArray<GCHapticsLocality> *requestedLocalities;
@property int attempts, failureStage, failureAttempt;
@property BOOL noAdvanced;
@end
@implementation TestHaptics
- (instancetype)init {
    if ((self = [super init])) {
        _supportedLocalities = [NSSet setWithObject:GCHapticsLocalityDefault];
        _engines = [NSMutableArray new]; _requestedLocalities = [NSMutableArray new];
    }
    return self;
}
- (CHHapticEngine *)createEngineWithLocality:(GCHapticsLocality)locality {
    assert([self.supportedLocalities containsObject:locality]);
    self.attempts++;
    [self.requestedLocalities addObject:locality];
    BOOL failing = self.attempts == (self.failureAttempt ?: 1);
    if (failing && self.failureStage == 1) return nil;
    TestEngine *e = [TestEngine new]; [self.engines addObject:e]; e.noAdvanced = self.noAdvanced;
    if (failing) {
        e.fail = self.failureStage == 2; e.failPattern = self.failureStage == 3;
        e.failPlayer = self.failureStage == 4; e.failSend = self.failureStage == 5;
        e.resetDuringStart = self.failureStage == 2;
    }
    return (id)e;
}
@end
@interface TestController : NSObject
@property TestHaptics *haptics;
@property NSString *vendorName, *productCategory;
@end
@implementation TestController
@end
// A HID device in the fake registry: its entry (one per connection), and the handles made on it
@interface TestHid : NSObject
@property io_service_t service;
@property int vendor, product, page, usage, opens, closes, reportAttempts;
@property BOOL bluetooth, synthetic, failOpen, failReport;
@property NSData *report;
@property NSMutableArray<NSData *> *reports;
@end
@implementation TestHid
- (instancetype)init { if ((self = [super init])) _reports = [NSMutableArray new]; return self; }
@end
@interface TestHandle : NSObject
@property TestHid *hid;
@end
@implementation TestHandle
@end
#define HID_ITERATOR 77
static NSMutableArray<TestHid *> *registry;
static NSArray<TestHid *> *listed;   // what the open iterator walks
static NSUInteger listedNext;
static int objects;   // iterators and services not released yet
static TestHid *hid_add_vp(int vendor, int product) {
    static io_service_t next = 1000;
    TestHid *h = [TestHid new]; h.service = next++; h.vendor = vendor; h.product = product; h.page = 1; h.usage = 5; h.bluetooth = YES;
    [registry addObject:h]; return h;
}
static TestHid *hid_add(int product) { return hid_add_vp(0x054C, product); }
static TestHid *hid_of(int player) {
    __block IOHIDDeviceRef d; dispatch_sync(queue, ^{ d = ds4[player].dev; });
    return d ? ((__bridge TestHandle *)d).hid : nil;
}
static TestHid *pro_of(int player) {
    __block IOHIDDeviceRef d; dispatch_sync(queue, ^{ d = pro[player].dev; });
    return d ? ((__bridge TestHandle *)d).hid : nil;
}
static TestHid *abitdo_of(int player) {
    __block IOHIDDeviceRef d; dispatch_sync(queue, ^{ d = abitdo[player].dev; });
    return d ? ((__bridge TestHandle *)d).hid : nil;
}
static kern_return_t test_matching_services(mach_port_t port, CFDictionaryRef match, io_iterator_t *it) {
    (void)port;
    int vendor = [(__bridge NSNumber *)CFDictionaryGetValue(match, CFSTR(kIOHIDVendorIDKey)) intValue];
    assert(!listed && (vendor == 0x054C || vendor == 0x057E || vendor == 0x2DC8));
    CFRelease(match);
    NSMutableArray<TestHid *> *want = [NSMutableArray new];
    for (TestHid *h in registry) if (h.vendor == vendor) [want addObject:h];
    listed = want; listedNext = 0; objects++; *it = HID_ITERATOR;
    return KERN_SUCCESS;
}
static io_object_t test_iterator_next(io_iterator_t it) {
    assert(it == HID_ITERATOR);
    if (listedNext == listed.count) return 0;
    objects++; return listed[listedNext++].service;
}
static kern_return_t test_object_release(io_object_t o) {
    objects--; if (o == HID_ITERATOR) listed = nil;
    return KERN_SUCCESS;
}
static TestHid *hid_listed(io_service_t s) {
    for (TestHid *h in listed) if (h.service == s) return h;
    abort();
}
static kern_return_t test_entry_id(io_registry_entry_t s, uint64_t *id) { *id = hid_listed(s).service; return KERN_SUCCESS; }
static IOHIDDeviceRef test_device_create(CFAllocatorRef a, io_service_t s) {
    (void)a;
    TestHandle *handle = [TestHandle new]; handle.hid = hid_listed(s);
    return (__bridge_retained IOHIDDeviceRef)handle;
}
static CFTypeRef test_device_property(IOHIDDeviceRef d, CFStringRef key) {
    TestHid *h = ((__bridge TestHandle *)d).hid;
    if (CFEqual(key, CFSTR(kIOHIDGCSyntheticDeviceKey))) return h.synthetic ? kCFBooleanTrue : NULL;
    if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return (__bridge CFTypeRef)@(h.product);
    if (CFEqual(key, CFSTR(kIOHIDPrimaryUsagePageKey))) return (__bridge CFTypeRef)@(h.page);
    if (CFEqual(key, CFSTR(kIOHIDPrimaryUsageKey))) return (__bridge CFTypeRef)@(h.usage);
    assert(CFEqual(key, CFSTR(kIOHIDTransportKey)));
    return (__bridge CFTypeRef)(h.bluetooth ? @"Bluetooth" : @"USB");
}
static IOReturn test_device_open(IOHIDDeviceRef d, IOOptionBits o) {
    TestHid *h = ((__bridge TestHandle *)d).hid; assert(o == kIOHIDOptionsTypeNone);
    if (h.failOpen) return kIOReturnNotPermitted;
    h.opens++; return kIOReturnSuccess;
}
static IOReturn test_device_close(IOHIDDeviceRef d, IOOptionBits o) { (void)o; ((__bridge TestHandle *)d).hid.closes++; return kIOReturnSuccess; }
static IOReturn test_device_report(IOHIDDeviceRef d, IOHIDReportType t, CFIndex id, const uint8_t *r, CFIndex n) {
    TestHid *h = ((__bridge TestHandle *)d).hid;
    assert(t == kIOHIDReportTypeOutput && id == r[0] && h.opens > h.closes);
    h.reportAttempts++;
    if (![registry containsObject:h]) return kIOReturnNoDevice;
    if (h.failReport) return kIOReturnError;
    h.report = [NSData dataWithBytes:r length:n];
    [h.reports addObject:h.report];
    return kIOReturnSuccess;
}
static void test_sink(const dm_haptic_state *s) { (void)s; }
static void drain(void) { dispatch_sync(queue, ^{}); dispatch_sync(queue, ^{}); }
static void advance(NSTimeInterval seconds) {
    uint64_t end = testTime + (uint64_t)llround(seconds * NSEC_PER_SEC);
    for (;;) {
        __block BOOL fired = NO;
        dispatch_sync(queue, ^{
            TestTimer *next = nil;
            for (TestTimer *timer in timers)
                if (timer.deadline <= end && (!next || timer.deadline < next.deadline)) next = timer;
            if (!next) { testTime = end; return; }
            testTime = next.deadline; [timers removeObject:next];
            next.block(); fired = YES;
        });
        drain();   // a callback may enqueue an epoch change or another output snapshot
        if (!fired) return;
    }
}
static void expire_retries(void) {
    dispatch_sync(queue, ^{ for (DmRumbleChannel *c in channels[0]) c.retryAfter = 0; });
}
static void send_pair(float left, float right) {
    dm_vibration_update u[] = {{0, 0, {left, 160, 0, 320}}, {0, 1, {right, 160, 0, 320}}};
    dm_haptics_send(u, 2); drain();
}
static void t_joycon_recovery(NSSet *localities, GCHapticsLocality left, GCHapticsLocality right) {
    // Both halves, with and without advanced players: a refused factory or engine start must retry that same actuator, never Default.
    for (int side = 0; side < 2; side++) for (int stage = 1; stage <= 2; stage++) for (int plain = 0; plain < 2; plain++) {
        TestController *controller = [TestController new]; TestHaptics *h = [TestHaptics new]; controller.haptics = h;
        h.supportedLocalities = localities; h.failureStage = stage; h.noAdvanced = plain;
        h.failureAttempt = plain ? 2 : 1;   // fail the replacement plain engine too, after the advanced player's refusal
        dm_haptics_apple_controller(0, (id)controller); drain();
        NSArray<DmRumbleChannel *> *route = channels[0];
        send_pair(side ? 0 : 0.6, side ? 0.6 : 0);
        assert(h.attempts == h.failureAttempt && channels[0] == route && route.count == 2);
        assert(!route[0].fallback && !route[1].fallback && route[0].motor == 0 && route[1].motor == 1);
        assert(dm_haptics_actual(0, side).amplitude_low == 0);
        void (^lateReset)(void) = h.engines.lastObject.startingReset;
        for (int i = 0; i < 3; i++) send_pair(side ? 0 : 0.6, side ? 0.6 : 0);
        assert(h.attempts == h.failureAttempt);   // retain the existing backoff instead of hammering the failed helper
        send_pair(side ? 0.4 : 0, side ? 0 : 0.4);
        TestEngine *other = h.engines.lastObject;
        assert(h.attempts == h.failureAttempt + (plain ? 2 : 1) && other.testPlayer.levels.lastObject.floatValue == 0.4f);
        expire_retries(); send_pair(side ? 0.4 : 0.6, side ? 0.6 : 0.4);
        TestEngine *recovered = h.engines.lastObject;
        GCHapticsLocality failedLocality = side ? right : left, otherLocality = side ? left : right;
        NSArray *want = plain ? @[failedLocality, failedLocality, otherLocality, otherLocality, failedLocality] : @[failedLocality, otherLocality, failedLocality];
        assert([h.requestedLocalities isEqual:want] && channels[0] == route);
        assert((id)route[1 - side].engine == other && recovered != other);
        if (lateReset) { lateReset(); drain(); }
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0.6f && other.testPlayer.levels.lastObject.floatValue == 0.4f);
        assert(dm_haptics_actual(0, side).amplitude_low == 0.6f && dm_haptics_actual(0, 1 - side).amplitude_low == 0.4f);
        send_pair(0, 0);
    }
}
static void t_joycon_lifetime(NSSet *localities, GCHapticsLocality left, GCHapticsLocality right) {
    // Simulate controller-object replacement on a physical split/join. The guest's npad assignment is tested in t_hid.
    TestController *pair = [TestController new]; pair.haptics = [TestHaptics new];
    pair.haptics.supportedLocalities = localities; pair.haptics.noAdvanced = YES;
    dm_haptics_apple_controller(0, (id)pair); send_pair(0.2, 0.7);
    NSArray<DmRumbleChannel *> *oldRoute = channels[0];   // keep old channels alive: callbacks must be stale, not merely lose their weak target
    TestEngine *oldLeft = pair.haptics.engines[1], *oldRight = pair.haptics.engines[3];
    void (^oldReset)(void) = oldLeft.resetHandler;
    void (^oldStopped)(CHHapticEngineStoppedReason) = oldRight.stoppedHandler;
    dm_haptic_state oldPair = {0, dm_haptics_epoch(0), {{1, 160, 0, 320}, {1, 160, 0, 320}}};
    TestController *singleLeft = [TestController new], *singleRight = [TestController new];
    singleLeft.haptics = [TestHaptics new]; singleRight.haptics = [TestHaptics new];
    singleLeft.haptics.supportedLocalities = [NSSet setWithObjects:GCHapticsLocalityDefault, left, nil];
    singleRight.haptics.supportedLocalities = [NSSet setWithObjects:GCHapticsLocalityDefault, right, nil];
    singleLeft.haptics.noAdvanced = singleRight.haptics.noAdvanced = YES;
    dm_haptics_apple_controller(0, (id)singleLeft); dm_haptics_apple_controller(1, (id)singleRight); drain();
    assert(oldLeft.testPlayer.levels.lastObject.floatValue == 0 && oldRight.testPlayer.levels.lastObject.floatValue == 0);
    dm_vibration_update u[] = {{0, 0, {0.4, 160, 0, 320}}, {1, 1, {0.6, 160, 0, 320}}};
    dm_haptics_send(u, 2); drain();
    TestEngine *splitLeft = singleLeft.haptics.engines.lastObject, *splitRight = singleRight.haptics.engines.lastObject;
    assert(splitLeft.testPlayer.levels.lastObject.floatValue == 0.4f && splitRight.testPlayer.levels.lastObject.floatValue == 0.6f);
    NSArray<DmRumbleChannel *> *leftRoute = channels[0], *rightRoute = channels[1];
    dm_haptic_state oldSingle = {1, dm_haptics_epoch(1), {{1, 160, 0, 320}, {1, 160, 0, 320}}};
    dm_haptics_apple_controller(0, nil); dm_haptics_apple_controller(1, nil);
    dm_haptics_apple_controller(0, (id)pair); drain();   // rejoin, with no effect replayed until a fresh send
    assert(pair.haptics.attempts == 4 && !(dm_haptics_available() & 2));
    assert(splitLeft.testPlayer.levels.lastObject.floatValue == 0 && splitRight.testPlayer.levels.lastObject.floatValue == 0);
    send_pair(0.5, 0.8);
    TestEngine *joinedLeft = pair.haptics.engines[5], *joinedRight = pair.haptics.engines[7];
    oldReset(); oldStopped(CHHapticEngineStoppedReasonSystemError);
    splitLeft.startingReset(); splitRight.startingReset();
    apply(oldPair); apply(oldSingle); drain();
    advance(1.1);   // old pulse renewals and delayed stops run alongside the rejoined pair's renewals
    assert(oldRoute[0].pattern == nil && oldRoute[1].pattern == nil && leftRoute[0].pattern == nil && rightRoute[0].pattern == nil);
    assert(oldLeft.plainPlayers == 1 && oldRight.plainPlayers == 1 && splitLeft.plainPlayers == 1 && splitRight.plainPlayers == 1);
    assert(oldLeft.stops == 1 && oldRight.stops == 1 && splitLeft.stops == 1 && splitRight.stops == 1);
    assert(joinedLeft.stops == 0 && joinedRight.stops == 0 && joinedLeft.plainPlayers > 1 && joinedRight.plainPlayers > 1);
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.5f && dm_haptics_actual(0, 1).amplitude_low == 0.8f);
    NSArray *joinedRoute = channels[0];
    dm_haptics_apple_controller(0, (id)pair); drain();
    assert(channels[0] == joinedRoute && pair.haptics.attempts == 8 && dm_haptics_actual(0, 1).amplitude_low == 0.8f);
    dm_haptics_apple_controller(0, nil); drain();
    assert(joinedLeft.testPlayer.levels.lastObject.floatValue == 0 && joinedRight.testPlayer.levels.lastObject.floatValue == 0);
    dm_haptics_apple_controller(0, (id)pair); drain();
    assert(pair.haptics.attempts == 8 && dm_haptics_actual(0, 0).amplitude_low == 0 && dm_haptics_actual(0, 1).amplitude_low == 0);
    send_pair(0, 0.3);
    assert(pair.haptics.attempts == 10 && [pair.haptics.requestedLocalities.lastObject isEqual:right]);
    assert(pair.haptics.engines.lastObject.testPlayer.levels.lastObject.floatValue == 0.3f);
    send_pair(0, 0);
}
static int made;   // engines the test factories made
int main(void) { @autoreleasepool {
    queue = dispatch_queue_create("test.rumble", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(queue, &queue, &queue, NULL);
    timers = [NSMutableArray new];
    dm_haptics_init(apply, 1); drain();
    TestEngine *left = [TestEngine new], *right = [TestEngine new];
    dispatch_sync(queue, ^{
        channels[0] = [NSMutableArray new];
        add_engine(0, 0, @"test left", ^CHHapticEngine *(NSError **e) { (void)e; made++; return (id)left; });
        add_engine(0, 1, @"test right", ^CHHapticEngine *(NSError **e) { (void)e; made++; return (id)right; });
        dm_haptics_route(0, 1);
    }); drain();
    send_pair(0.2, 0.7);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.2f && right.testPlayer.levels.lastObject.floatValue == 0.7f);
    send_pair(0, 0);
    assert(left.testPlayer.levels.lastObject.floatValue == 0 && right.testPlayer.levels.lastObject.floatValue == 0);
    assert(left.testPlayer.levels.count == 2);   // short pulse ends immediately, without a minimum duration
    send_pair(0.4, 0.5);
    advance(1.1);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.4f && left.testPlayer.stops == 0);   // old idle timer cannot stop a new effect
    assert(left.starts == 1);   // reused player; no per-frame engine restart
    dm_haptic_state stale = {0, dm_haptics_epoch(0), {{1, 160, 0, 320}, {1, 160, 0, 320}}};
    suspend(DM_HAPTICS_INACTIVE, YES); suspend(DM_HAPTICS_INACTIVE, NO); drain();
    int starts = left.starts; apply(stale); drain(); assert(left.starts == starts);
    send_pair(0.3, 0.6); assert(left.starts == starts + 1);
    void (^oldReset)(void) = left.resetHandler;
    suspend(DM_HAPTICS_INACTIVE, YES); suspend(DM_HAPTICS_INACTIVE, NO); drain();
    send_pair(0.5, 0.5); oldReset(); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.5f);   // delayed callback from a stopped engine is stale
    left.resetHandler(); drain(); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0 && right.testPlayer.levels.lastObject.floatValue == 0);
    left.fail = YES; send_pair(0.8, 0.8); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0);   // failed output is cleared, so a fresh identical send may retry
    left.fail = NO; expire_retries(); send_pair(0.8, 0.8);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.8f);
    send_pair(0, 0); advance(0.99);
    assert(left.testPlayer.stops == 0);   // idle release must not run early
    advance(0.01);
    assert(left.testPlayer.stops == 1);   // idle hardware eventually released
    int engines = made; starts = left.starts; send_pair(0.7, 0.7);
    assert(made == engines && left.starts == starts + 1 && left.testPlayer.levels.lastObject.floatValue == 0.7f);   // the released engine is restarted, not made again
    // An engine that stopped on its own raises instead of returning an error: that is a failed engine, not a crash of the app.
    left.testPlayer.raiseSend = YES; send_pair(0.9, 0.9); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0);   // cleared, so a fresh identical send retries after the backoff
    left.testPlayer.raiseSend = NO; expire_retries(); send_pair(0.9, 0.9);
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.9f);
    send_pair(0, 0);
    // A permanently broken helper/engine is replaced, rather than retried for the rest of the session.
    // Exercise the real controller route (including a nil engine factory result), without hardware.
    for (int stage = 1; stage <= 5; stage++) {
        TestController *controller = [TestController new]; controller.vendorName = @"test controller";
        TestHaptics *h = [TestHaptics new]; h.failureStage = stage; controller.haptics = h;
        dm_haptics_apple_controller(0, (id)controller); drain();
        assert(dm_haptics_available() & 1);
        send_pair(0.6, 0); drain();
        if (stage == 3) {   // a refused advanced player is taken as a server without them: plain players from a new engine play at once
            assert(h.attempts == 2 && h.engines[1].plainPlayers == 1 && dm_haptics_actual(0, 0).amplitude_low == 0.6f);
            send_pair(0, 0); drain(); continue;
        }
        assert(h.attempts == 1 && dm_haptics_actual(0, 0).amplitude_low == 0);
        assert(stage == 1 || h.engines[0].handlersAtStart);   // callbacks cover startup itself
        void (^lateReset)(void) = h.engines.firstObject.startingReset;
        for (int i = 0; i < 20; i++) send_pair(0.6, 0);
        assert(h.attempts == 1);   // an ongoing effect must not hammer a failing XPC service
        expire_retries(); send_pair(0.6, 0); drain();
        assert(h.attempts == 2);
        TestEngine *recovered = h.engines.lastObject;
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0.6f);
        assert(dm_haptics_actual(0, 0).amplitude_low == 0.6f);
        if (lateReset) { lateReset(); drain(); }
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0.6f);   // old callbacks cannot silence the replacement
        send_pair(0, 0); drain();
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0);
    }
    // A server without advanced players (game controllers on macOS 27): the first effect plays at once in plain pulses from a new engine, the pulses
    // follow each other while it lasts and take its level, and a stop never cuts one (stopAtTime): it silences them and stops the engine after the last
    TestController *plainc = [TestController new]; plainc.haptics = [TestHaptics new]; plainc.haptics.noAdvanced = YES;
    dm_haptics_apple_controller(0, (id)plainc); drain();
    send_pair(0.6, 0); drain();
    TestEngine *pe = plainc.haptics.engines.lastObject;
    assert(plainc.haptics.attempts == 2 && pe.plainPlayers == 1 && pe.testPlayer.levels.lastObject.floatValue == 0.6f);
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.6f);
    send_pair(0.2, 0); drain();
    assert(pe.plainPlayers == 1 && pe.testPlayer.levels.lastObject.floatValue == 0.2f);
    advance(1.1);
    int pulses = pe.plainPlayers; assert(pulses >= 3);   // renewed while the effect lasts
    send_pair(0, 0); advance(1.7);
    assert(pe.plainPlayers == pulses && pe.testPlayer.levels.lastObject.floatValue == 0);   // silent: no new pulse
    assert(pe.stops == 1 && pe.testPlayer.stops == 0);   // the engine stopped after the last pulse, which was not cut
    send_pair(0.5, 0); drain();
    assert(plainc.haptics.attempts == 2 && pe.plainPlayers == pulses + 1 && pe.testPlayer.levels.lastObject.floatValue == 0.5f);
    // A plain player whose engine stopped under it raises: the engine is dropped without calling its players again (a stop sends them zero,
    // which would raise once more), and the next effect after the backoff plays from a new engine
    pe.testPlayer.raiseSend = YES; send_pair(0.7, 0); drain();
    assert(channels[0].firstObject.engine == nil && dm_haptics_actual(0, 0).amplitude_low == 0);
    expire_retries(); send_pair(0.7, 0); drain();
    TestEngine *pe2 = plainc.haptics.engines.lastObject;
    assert(plainc.haptics.attempts == 3 && pe2 != pe && pe2.testPlayer.levels.lastObject.floatValue == 0.7f);
    // The same from the idle release, which silences the pulses a second after the effect ends
    send_pair(0, 0); drain(); pe2.testPlayer.raiseSend = YES; advance(1.1);
    assert(channels[0].firstObject.engine == nil);
    expire_retries(); send_pair(0.4, 0); drain();
    assert(plainc.haptics.attempts == 4 && plainc.haptics.engines.lastObject.testPlayer.levels.lastObject.floatValue == 0.4f);
    send_pair(0, 0); drain();
    TestController *split = [TestController new]; split.haptics = [TestHaptics new];
    split.haptics.supportedLocalities = [NSSet setWithObjects:GCHapticsLocalityDefault, GCHapticsLocalityLeftHandle, GCHapticsLocalityRightHandle, nil];
    split.haptics.failureStage = 2;
    dm_haptics_apple_controller(0, (id)split); drain();
    send_pair(0.3, 0.4); drain();
    assert(split.haptics.attempts == 1);   // the old right-hand route must not replay the failed batch
    assert(channels[0].count == 1);
    expire_retries(); send_pair(0.3, 0.4); drain();
    assert(split.haptics.attempts == 2 && [split.haptics.requestedLocalities.lastObject isEqual:GCHapticsLocalityDefault]);
    assert(fabsf(split.haptics.engines.lastObject.testPlayer.levels.lastObject.floatValue - 0.5f) < 1e-6f);
    TestController *split2 = [TestController new]; split2.haptics = [TestHaptics new];
    split2.haptics.supportedLocalities = split.haptics.supportedLocalities;
    split2.haptics.failureStage = 3;
    dm_haptics_apple_controller(0, (id)split2); drain();
    send_pair(0.3, 0.4); drain();
    // A refused advanced player turns the handle route into one engine for both motors (the handles' engines override each other without them),
    // which plays the next send in plain pulses at the mixed level
    assert(split2.haptics.attempts == 1 && channels[0].count == 1 && channels[0].firstObject.simple);
    send_pair(0.3, 0.4); drain();
    NSArray *asked = split2.haptics.requestedLocalities;
    assert(split2.haptics.attempts == 2 && [asked[1] isEqual:GCHapticsLocalityDefault] && split2.haptics.engines[1].plainPlayers == 1);
    assert(fabsf(split2.haptics.engines[1].testPlayer.levels.lastObject.floatValue - 0.5f) < 1e-6f);
    send_pair(0, 0); drain();
    TestController *split3 = [TestController new]; split3.haptics = [TestHaptics new]; split3.haptics.noAdvanced = YES;
    split3.haptics.supportedLocalities = [NSSet setWithObjects:GCHapticsLocalityDefault, GCHapticsLocalityHandles, GCHapticsLocalityLeftHandle, GCHapticsLocalityRightHandle, nil];
    dm_haptics_apple_controller(0, (id)split3); drain();
    send_pair(0.3, 0.4); send_pair(0.3, 0.4); drain();
    assert([split3.haptics.requestedLocalities.lastObject isEqual:GCHapticsLocalityHandles]);   // the handles' own group when the controller lists it
    send_pair(0, 0); drain();
    // A DualSense keeps its handle route when advanced players are refused: each handle moves to plain players on a new engine of its own
    // locality, at its own level
    TestController *sense = [TestController new]; sense.haptics = [TestHaptics new]; sense.haptics.noAdvanced = YES;
    sense.productCategory = GCProductCategoryDualSense; sense.haptics.supportedLocalities = split3.haptics.supportedLocalities;
    dm_haptics_apple_controller(0, (id)sense); drain();
    assert(channels[0].count == 2 && !channels[0][0].simple && !channels[0][0].merge && !channels[0][1].merge);
    send_pair(0.3, 0.4); drain();
    NSArray *want = @[GCHapticsLocalityLeftHandle, GCHapticsLocalityLeftHandle, GCHapticsLocalityRightHandle, GCHapticsLocalityRightHandle];
    assert(channels[0].count == 2 && channels[0][0].simple && channels[0][1].simple && [sense.haptics.requestedLocalities isEqual:want]);
    assert(sense.haptics.engines[1].plainPlayers == 1 && sense.haptics.engines[3].plainPlayers == 1);
    assert(fabsf(sense.haptics.engines[1].testPlayer.levels.lastObject.floatValue - 0.3f) < 1e-6f);
    assert(fabsf(sense.haptics.engines[3].testPlayer.levels.lastObject.floatValue - 0.4f) < 1e-6f);
    send_pair(0, 0.4); drain();   // one side alone
    assert(sense.haptics.engines[1].testPlayer.levels.lastObject.floatValue == 0 && fabsf(sense.haptics.engines[3].testPlayer.levels.lastObject.floatValue - 0.4f) < 1e-6f);
    send_pair(0, 0); drain();
    // With advanced players a DualSense plays them, as any controller with both handles does
    TestController *sense2 = [TestController new]; sense2.haptics = [TestHaptics new];
    sense2.productCategory = GCProductCategoryDualSense; sense2.haptics.supportedLocalities = split3.haptics.supportedLocalities;
    dm_haptics_apple_controller(0, (id)sense2); drain();
    send_pair(0.3, 0.4); drain();
    assert(sense2.haptics.attempts == 2 && !channels[0][0].simple && !channels[0][1].simple);
    assert(sense2.haptics.engines[0].plainPlayers == 0 && sense2.haptics.engines[1].plainPlayers == 0);
    assert(fabsf(sense2.haptics.engines[1].testPlayer.levels.lastObject.floatValue - 0.4f) < 1e-6f);
    send_pair(0, 0); drain();
    // Paired Joy-Cons report "JoyCon (L)" and "JoyCon (R)" instead of handle localities, and keep both channels on plain players
    GCHapticsLocality joyL = (GCHapticsLocality)@"JoyCon (L)", joyR = (GCHapticsLocality)@"JoyCon (R)";
    NSSet *joyLocalities = [NSSet setWithObjects:GCHapticsLocalityAll, GCHapticsLocalityDefault, joyL, joyR, nil];
    TestController *joy = [TestController new]; joy.haptics = [TestHaptics new]; joy.haptics.noAdvanced = YES;
    joy.vendorName = @"Joy-Con (L/R)"; joy.productCategory = @"Nintendo Switch Joy-Con (L/R)";
    joy.haptics.supportedLocalities = joyLocalities;
    dm_haptics_apple_controller(0, (id)joy); drain();
    assert(channels[0].count == 2 && !channels[0][0].simple && !channels[0][0].merge && !channels[0][1].merge);
    send_pair(0.6, 0); drain();   // left Joy-Con alone (e.g. npad 0 in single Joy-Con mode): right engine is not even started
    NSArray *joyLeftOnly = @[joyL, joyL];
    assert(channels[0].count == 2 && channels[0][0].simple && !channels[0][1].simple && [joy.haptics.requestedLocalities isEqual:joyLeftOnly]);
    assert(fabsf(joy.haptics.engines[1].testPlayer.levels.lastObject.floatValue - 0.6f) < 1e-6f);
    send_pair(0, 0.4); drain();   // right Joy-Con alone (e.g. npad 1 in single Joy-Con mode): left silences, right starts its own plain player
    NSArray *joyBoth = @[joyL, joyL, joyR, joyR];
    assert(channels[0][1].simple && [joy.haptics.requestedLocalities isEqual:joyBoth]);
    assert(joy.haptics.engines[1].testPlayer.levels.lastObject.floatValue == 0);
    assert(fabsf(joy.haptics.engines[3].testPlayer.levels.lastObject.floatValue - 0.4f) < 1e-6f);
    send_pair(0.3, 0.7); drain();
    assert(fabsf(joy.haptics.engines[1].testPlayer.levels.lastObject.floatValue - 0.3f) < 1e-6f);
    assert(fabsf(joy.haptics.engines[3].testPlayer.levels.lastObject.floatValue - 0.7f) < 1e-6f);
    send_pair(0, 0); drain();
    t_joycon_recovery(joyLocalities, joyL, joyR);
    t_joycon_lifetime(joyLocalities, joyL, joyR);
    dm_haptics_apple_controller(0, nil); drain();   // disconnect discards the controller factory and its old callbacks
    assert(!(dm_haptics_available() & 1));
    TestController *unsupported = [TestController new]; unsupported.haptics = [TestHaptics new];
    unsupported.haptics.supportedLocalities = [NSSet set];
    dm_haptics_apple_controller(0, (id)unsupported); drain();
    assert(!(dm_haptics_available() & 1) && unsupported.haptics.attempts == 0);
    // DM_RUMBLE_MIN: controller levels below it are silent, the rest stretched over the full range; the phone keeps its levels
    assert(controller_level(0) == 0 && controller_level(0.3f) == 0.3f && controller_level(1) == 1 && controller_level(NAN) == 0);
    rumble_min = 0.45f;
    assert(controller_level(0.4f) == 0 && controller_level(0.45f) == 0 && fabsf(controller_level(0.725f) - 0.5f) < 1e-6f && controller_level(1) == 1);
    TestController *cut = [TestController new]; cut.haptics = [TestHaptics new];
    dm_haptics_apple_controller(0, (id)cut); drain();
    send_pair(0.3, 0); drain();
    assert(cut.haptics.attempts == 0);   // silent: no engine is made
    send_pair(0.725, 0); drain();
    assert(fabsf(cut.haptics.engines.lastObject.testPlayer.levels.lastObject.floatValue - 0.5f) < 1e-6f);
    send_pair(0, 0); drain();
    rumble_min = 0;
    // A DualShock 4 goes through its effects report only while a HID device is free; without one it keeps Core Haptics
    registry = [NSMutableArray new];
    TestController *ds = [TestController new]; ds.haptics = [TestHaptics new]; ds.productCategory = GCProductCategoryDualShock4;
    dm_haptics_apple_controller(0, (id)ds); drain();
    assert(!hid_of(0));
    send_pair(0.6, 0); drain();
    assert(ds.haptics.attempts == 1 && ds.haptics.engines[0].testPlayer.levels.lastObject.floatValue == 0.6f);
    send_pair(0, 0); drain();
    dm_haptics_apple_controller(0, nil); drain();
    // Devices the route cannot use are passed over: another Sony product, a synthetic GameController device, one that refuses to open
    hid_add(0x0CE6); TestHid *synth = hid_add(0x05C4); synth.synthetic = YES;
    TestHid *refused = hid_add(0x05C4); refused.failOpen = YES;
    TestHid *first = hid_add(0x05C4);
    TestController *ds1 = [TestController new]; ds1.haptics = [TestHaptics new]; ds1.productCategory = GCProductCategoryDualShock4;
    dm_haptics_apple_controller(0, (id)ds1); drain();
    assert(hid_of(0) == first && first.opens == 1 && synth.opens == 0 && channels[0].count == 2 && [channels[0][0] isKindOfClass:DmDs4Channel.class]);
    // A controller that connects long after the first gets its own device, not the first one's and not Core Haptics (issue #1165)
    TestController *ds2 = [TestController new]; ds2.haptics = [TestHaptics new]; ds2.productCategory = GCProductCategoryDualShock4;
    dm_haptics_apple_controller(1, (id)ds2); drain();
    assert(!hid_of(1) && channels[1].count == 1);   // no device left for it yet: the first is not shared
    dm_haptics_apple_controller(1, nil); drain();
    TestHid *second = hid_add(0x05C4); second.bluetooth = NO;
    dm_haptics_apple_controller(1, (id)ds2); drain();
    assert(hid_of(1) == second && hid_of(0) == first && ds2.haptics.attempts == 0);
    // The effects report: each player's motors on its own device, strong from the low band and weak from the high one, over either transport
    dm_vibration_update u1[] = {{0, 0, {1, 160, 0, 320}}, {0, 1, {0, 160, 0, 320}}, {1, 0, {0, 160, 0, 320}}, {1, 1, {0, 160, 1, 320}}};
    dm_haptics_send(u1, 4); drain();
    const uint8_t *b = first.report.bytes;
    assert(first.report.length == 78 && b[0] == 0x11 && b[1] == 0xC4 && b[3] == 0x01 && b[7] == 217 && b[6] == 0);   // 0.85 of full on the strong motor
    uint8_t crc[75] = {0xA2}; memcpy(crc + 1, b, 74); uint32_t sum = ds4_crc(crc, sizeof crc);
    assert(!memcmp(b + 74, &sum, 4));
    b = second.report.bytes;
    assert(second.report.length == 32 && b[0] == 0x05 && b[1] == 0x01 && b[5] == 0 && b[4] == 217);
    // A controller that goes away and comes back has a new device; its old one is closed and never used again
    dm_haptics_apple_controller(0, nil); drain();
    assert(first.closes == 1 && !hid_of(0) && first.report.length == 78 && ((const uint8_t *)first.report.bytes)[7] == 0);   // stopped on the way out
    [registry removeObject:first];
    TestHid *back = hid_add(0x09CC);
    dm_haptics_apple_controller(0, (id)[TestController new]); drain();   // not a DualShock 4: no device, whatever is free
    assert(!hid_of(0) && back.opens == 0);
    dm_haptics_apple_controller(0, (id)ds1); drain();
    assert(hid_of(0) == back && back.opens == 1 && hid_of(1) == second);
    send_pair(0.5, 0); drain();
    assert(back.report.length == 78 && first.closes == 1);
    send_pair(0, 0); drain();
    dm_haptics_apple_controller(0, nil); dm_haptics_apple_controller(1, nil); drain();
    assert(back.closes == 1 && second.closes == 1 && objects == 0);   // every iterator and service released
    assert(ds4_crc((const uint8_t *)"123456789", 9) == 0xCBF43926u);   // the report's CRC32 (IEEE, reflected)

    // Switch Pro Controller route: falls back to Core Haptics without a free live 057E:2009 HID device
    [registry removeAllObjects];
    TestController *pc = [TestController new]; pc.haptics = [TestHaptics new]; pc.productCategory = @"Switch Pro Controller"; pc.vendorName = @"Pro Controller";
    dm_haptics_apple_controller(0, (id)pc); drain();
    assert(!pro_of(0));
    send_pair(0.6, 0); drain();
    assert(pc.haptics.attempts == 1 && pc.haptics.engines[0].testPlayer.levels.lastObject.floatValue == 0.6f);
    send_pair(0, 0); drain();
    dm_haptics_apple_controller(0, nil); drain();
    // Skips another Nintendo product, a synthetic GameController device, a refused open and a dead entry whose 0x48 enable fails (#2237)
    hid_add_vp(0x057E, 0x2006);
    TestHid *proSynth = hid_add_vp(0x057E, 0x2009); proSynth.synthetic = YES;
    TestHid *proRefused = hid_add_vp(0x057E, 0x2009); proRefused.failOpen = YES;
    TestHid *proDead = hid_add_vp(0x057E, 0x2009); proDead.failReport = YES;
    TestHid *proBt = hid_add_vp(0x057E, 0x2009);
    TestController *pc1 = [TestController new]; pc1.haptics = [TestHaptics new]; pc1.productCategory = @"Switch Pro Controller"; pc1.vendorName = @"Pro Controller";
    dm_haptics_apple_controller(0, (id)pc1); drain();
    assert(pro_of(0) == proBt && proBt.opens == 1 && proSynth.opens == 0 && proDead.opens == 1 && proDead.closes == 1);
    assert(channels[0].count == 2 && [channels[0][0] isKindOfClass:DmProChannel.class] && pc1.haptics.attempts == 0);
    // Connect enables vibration with 49-byte subcommand 0x48 01 over Bluetooth
    assert(proBt.reports.count == 1 && proBt.report.length == JC1_REPORT_LEN);
    b = proBt.report.bytes;
    assert(b[0] == 0x01 && b[1] == 0x00 && !memcmp(b + 2, jc1_neutral_rumble, 8) && b[10] == JC1_SUB_VIBRATION && b[11] == 0x01);
    // Second Pro Controller over USB gets its own device and 64-byte reports
    TestHid *proUsb = hid_add_vp(0x057E, 0x2009); proUsb.bluetooth = NO;
    TestController *pc2 = [TestController new]; pc2.haptics = [TestHaptics new]; pc2.productCategory = @"Switch Pro Controller"; pc2.vendorName = @"Pro Controller";
    dm_haptics_apple_controller(1, (id)pc2); drain();
    assert(pro_of(1) == proUsb && pro_of(0) == proBt && proUsb.reports.count == 1 && proUsb.report.length == 64);
    b = proUsb.report.bytes;
    assert(b[0] == 0x01 && b[10] == JC1_SUB_VIBRATION && b[11] == 0x01);
    // Stereo L/R and both frequency bands via pad_jc1.h's exact table (including 0.04 -> code 9, and silent non-default frequency -> neutral)
    dm_vibration_update pu[] = {
        {0, 0, {0.04f, 160, 0.5f, 1000}},
        {0, 1, {0, 80, 0, 600}},
        {1, 0, {0, 160, 0, 320}},
        {1, 1, {1, 160, 1, 320}},
    };
    dm_haptics_send(pu, 4); drain();
    uint8_t wantL[4], wantR[4];
    dm_vibration vL = {0.04f, 160, 0.5f, 1000}, vFull = {1, 160, 1, 320};
    jc1_rumble_encode(&vL, wantL); jc1_rumble_encode(&vFull, wantR);
    assert(proBt.reports.count == 2 && proBt.report.length == JC1_REPORT_LEN);
    b = proBt.report.bytes;
    assert(b[0] == 0x10 && b[1] == 1 && !memcmp(b + 2, wantL, 4) && !memcmp(b + 6, jc1_neutral_rumble, 4));
    assert((b[4] >> 7) == (9 & 1) && b[5] == 0x40 + 9 / 2);   // exact log table code 9 for 0.04
    assert(proUsb.reports.count == 2 && proUsb.report.length == 64);
    b = proUsb.report.bytes;
    assert(b[0] == 0x10 && b[1] == 1 && !memcmp(b + 2, jc1_neutral_rumble, 4) && !memcmp(b + 6, wantR, 4));
    // Steady state refreshes every 30 ms without extra writes in between
    advance(0.029);
    assert(proBt.reports.count == 2 && proUsb.reports.count == 2);
    advance(0.001);
    assert(proBt.reports.count == 3 && proUsb.reports.count == 3);
    assert(((const uint8_t *)proBt.report.bytes)[1] == 2);
    advance(0.030);
    assert(proBt.reports.count == 4 && ((const uint8_t *)proBt.report.bytes)[1] == 3);
    // 15 ms pacing with peak-hold: a 5 ms spike (0.8) cleared to silence at 10 ms is not dropped and does not flood Bluetooth
    NSUInteger before = proBt.reports.count;
    advance(0.005); send_pair(0.8f, 0);
    assert(proBt.reports.count == before);   // throttled inside 15 ms
    advance(0.005); send_pair(0, 0);
    assert(proBt.reports.count == before);
    advance(0.005);   // 15 ms after last send: plays held 0.8 peak
    assert(proBt.reports.count == before + 1);
    uint8_t wantPeak[4]; dm_vibration vPeak = {0.8f, 160, 0, 320}; jc1_rumble_encode(&vPeak, wantPeak);
    b = proBt.report.bytes;
    assert(b[0] == 0x10 && !memcmp(b + 2, wantPeak, 4) && !memcmp(b + 6, jc1_neutral_rumble, 4));
    advance(0.015);   // 15 ms later: sends neutral and goes idle
    assert(proBt.reports.count == before + 2);
    b = proBt.report.bytes;
    assert(b[0] == 0x10 && !memcmp(b + 2, jc1_neutral_rumble, 8));
    advance(0.2);
    assert(proBt.reports.count == before + 2);   // idle: no refresh timers while silent
    // Rumble cutoff (DM_RUMBLE_MIN) and gain apply to the Pro Controller route
    rumble_min = 0.5f; gain = 2.0f;
    before = proBt.reports.count;
    send_pair(0.2f, 0);   // 0.2 * 2 = 0.4 <= 0.5: silenced by cutoff
    assert(proBt.reports.count == before);
    send_pair(0.375f, 0);   // 0.375 * 2 = 0.75 -> stretched to 0.5
    assert(proBt.reports.count == before + 1);
    uint8_t wantCut[4]; dm_vibration vCut = {0.5f, 160, 0, 320}; jc1_rumble_encode(&vCut, wantCut);
    assert(!memcmp((const uint8_t *)proBt.report.bytes + 2, wantCut, 4));
    rumble_min = 0; gain = 1;
    // Focus suspend immediately silences and cancels pending refreshes; returning focus replays nothing
    before = proBt.reports.count;
    NSUInteger usbBefore = proUsb.reports.count;
    suspend(DM_HAPTICS_INACTIVE, YES); drain();
    assert(proBt.reports.count == before + 1 && !memcmp((const uint8_t *)proBt.report.bytes + 2, jc1_neutral_rumble, 8));
    assert(proUsb.reports.count == usbBefore + 1 && !memcmp((const uint8_t *)proUsb.report.bytes + 2, jc1_neutral_rumble, 8));
    suspend(DM_HAPTICS_INACTIVE, NO); advance(0.1);
    assert(proBt.reports.count == before + 1 && proUsb.reports.count == usbBefore + 1);
    // Switching a player to a sink closes its Pro Controller HID handle; clearing the sink re-acquires it
    dm_haptics_apple_set_sink(0, test_sink); drain();
    assert(!pro_of(0) && proBt.closes == 1);
    dm_haptics_apple_set_sink(0, NULL); drain();
    assert(pro_of(0) == proBt && proBt.opens == 2);
    // Failed silence write on a gone device tries once at the pacing boundary and does not spin retrying silence
    send_pair(0.6f, 0);
    advance(0.005); proBt.failReport = YES; send_pair(0, 0);
    before = proBt.reports.count;
    int attemptsBefore = proBt.reportAttempts;
    advance(0.2);
    assert(proBt.reports.count == before && proBt.reportAttempts == attemptsBefore + 1);
    proBt.failReport = NO;
    // Disconnect stops an active controller once and releases every IOKit object
    dm_vibration_update pu2[] = {{1, 0, {0.7f, 160, 0, 320}}};
    dm_haptics_send(pu2, 1); drain();
    usbBefore = proUsb.reports.count;
    dm_haptics_apple_controller(0, nil); dm_haptics_apple_controller(1, nil); drain();
    assert(!pro_of(0) && !pro_of(1) && proBt.closes == 2 && proUsb.closes == 1 && objects == 0);
    assert(proUsb.reports.count == usbBefore + 1 && !memcmp((const uint8_t *)proUsb.report.bytes + 2, jc1_neutral_rumble, 8));

    // 8BitDo Ultimate 2 Wireless (2DC8:6012) route: taken only when GameController gives no haptics and a live gamepad interface accepts report 0x05
    [registry removeAllObjects];
    TestController *eb = [TestController new]; eb.haptics = [TestHaptics new]; eb.haptics.supportedLocalities = [NSSet set]; eb.vendorName = @"8BitDo Ultimate 2 Wireless";
    dm_haptics_apple_controller(0, (id)eb); drain();
    assert(!abitdo_of(0) && channels[0].count == 0);   // no HID device: stays without rumble
    dm_haptics_apple_controller(0, nil); drain();
    // Skips another 8BitDo PID, non-gamepad interface (keyboard), synthetic device, refused open, and dead/unsupported entry whose 0x05 probe fails
    hid_add_vp(0x2DC8, 0x301B);
    TestHid *ebKeys = hid_add_vp(0x2DC8, 0x6012); ebKeys.usage = 6;
    TestHid *ebSynth = hid_add_vp(0x2DC8, 0x6012); ebSynth.synthetic = YES;
    TestHid *ebRefused = hid_add_vp(0x2DC8, 0x6012); ebRefused.failOpen = YES;
    TestHid *ebDead = hid_add_vp(0x2DC8, 0x6012); ebDead.failReport = YES;
    TestHid *ebPad = hid_add_vp(0x2DC8, 0x6012);
    // A controller with haptics of its own or another name does not take the 8BitDo HID device
    TestController *ebHaptic = [TestController new]; ebHaptic.haptics = [TestHaptics new]; ebHaptic.vendorName = @"8BitDo Pro 2";
    dm_haptics_apple_controller(0, (id)ebHaptic); drain();
    assert(!abitdo_of(0) && channels[0].count == 1 && ebPad.opens == 0);
    TestController *generic = [TestController new]; generic.haptics = [TestHaptics new]; generic.haptics.supportedLocalities = [NSSet set]; generic.vendorName = @"Generic Gamepad";
    dm_haptics_apple_controller(0, (id)generic); drain();
    assert(!abitdo_of(0) && channels[0].count == 0 && ebPad.opens == 0);
    // Live gamepad connects after probing {0x05, 0, 0, 0, 0}
    dm_haptics_apple_controller(0, (id)eb); drain();
    assert(abitdo_of(0) == ebPad && ebPad.opens == 1 && ebSynth.opens == 0 && ebKeys.opens == 1 && ebKeys.closes == 1 && ebDead.opens == 1 && ebDead.closes == 1);
    assert(channels[0].count == 2 && [channels[0][0] isKindOfClass:DmAbitdoChannel.class]);
    assert(ebPad.reports.count == 1 && ebPad.report.length == 5);
    b = ebPad.report.bytes;
    assert(b[0] == 0x05 && b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0);
    // Second 8BitDo gets its own HID device
    TestHid *ebPad2 = hid_add_vp(0x2DC8, 0x6012);
    TestController *eb2 = [TestController new]; eb2.haptics = [TestHaptics new]; eb2.haptics.supportedLocalities = [NSSet set]; eb2.vendorName = @"8BitDo Ultimate 2 Wireless";
    dm_haptics_apple_controller(1, (id)eb2); drain();
    assert(abitdo_of(1) == ebPad2 && abitdo_of(0) == ebPad && ebPad2.reports.count == 1);
    // Linear 0.85/0.15 low/high band mix: strong motor follows low band, weak motor follows high band independently
    dm_vibration_update ebu[] = {{0, 0, {1, 160, 0, 320}}, {0, 1, {0, 160, 0, 320}}, {1, 0, {0, 160, 0, 320}}, {1, 1, {0, 160, 1, 320}}};
    dm_haptics_send(ebu, 4); drain();
    b = ebPad.report.bytes;
    assert(ebPad.reports.count == 2 && ebPad.report.length == 5 && b[0] == 0x05 && b[1] == 217 && b[2] == 0 && b[3] == 0 && b[4] == 0);
    b = ebPad2.report.bytes;
    assert(ebPad2.reports.count == 2 && ebPad2.report.length == 5 && b[0] == 0x05 && b[1] == 0 && b[2] == 217 && b[3] == 0 && b[4] == 0);
    // Steady active level refreshes every 50 ms without extra writes in between
    advance(0.049);
    assert(ebPad.reports.count == 2 && ebPad2.reports.count == 2);
    advance(0.001);
    assert(ebPad.reports.count == 3 && ebPad2.reports.count == 3);
    advance(0.050);
    assert(ebPad.reports.count == 4 && ebPad2.reports.count == 4);
    // 20 ms pacing with peak-hold: a 5 ms spike cleared at 10 ms is not dropped, then silence goes out three times 20 ms apart and ticks stop
    before = ebPad.reports.count;
    advance(0.005); send_pair(0.8f, 0.8f);   // low band 0.8 -> 204
    assert(ebPad.reports.count == before);   // throttled inside 20 ms
    advance(0.005); send_pair(0, 0);
    assert(ebPad.reports.count == before);
    advance(0.010);   // 20 ms after last send: plays held peak (204)
    b = ebPad.report.bytes;
    assert(ebPad.reports.count == before + 1 && b[1] == 204 && b[2] == 0);
    advance(0.020);   // 1st zero report
    b = ebPad.report.bytes;
    assert(ebPad.reports.count == before + 2 && b[1] == 0 && b[2] == 0);
    advance(0.020);   // 2nd zero report
    assert(ebPad.reports.count == before + 3);
    advance(0.020);   // 3rd zero report -> idle
    assert(ebPad.reports.count == before + 4);
    advance(0.2);
    assert(ebPad.reports.count == before + 4);   // idle: no timers while silent
    // Rumble cutoff (DM_RUMBLE_MIN) and gain apply linearly
    rumble_min = 0.5f; gain = 2.0f;
    before = ebPad.reports.count;
    send_pair(0.2f, 0.2f);   // 0.2 * 2 = 0.4 <= 0.5: silenced
    assert(ebPad.reports.count == before);
    send_pair(0.375f, 0.375f);   // 0.375 * 2 = 0.75 -> stretched to 0.5 -> 128
    b = ebPad.report.bytes;
    assert(ebPad.reports.count == before + 1 && b[1] == 128 && b[2] == 0);
    rumble_min = 0; gain = 1;
    // Focus suspend immediately silences once and cancels pending refreshes; returning focus replays nothing
    before = ebPad.reports.count;
    NSUInteger eb2Before = ebPad2.reports.count;
    suspend(DM_HAPTICS_INACTIVE, YES); drain();
    b = ebPad.report.bytes;
    assert(ebPad.reports.count == before + 1 && b[1] == 0 && b[2] == 0);
    b = ebPad2.report.bytes;
    assert(ebPad2.reports.count == eb2Before + 1 && b[1] == 0 && b[2] == 0);
    suspend(DM_HAPTICS_INACTIVE, NO); advance(0.2);
    assert(ebPad.reports.count == before + 1 && ebPad2.reports.count == eb2Before + 1);
    // Switching a player to a sink closes its HID handle; clearing the sink re-acquires it
    dm_haptics_apple_set_sink(0, test_sink); drain();
    assert(!abitdo_of(0) && ebPad.closes == 1);
    dm_haptics_apple_set_sink(0, NULL); drain();
    assert(abitdo_of(0) == ebPad && ebPad.opens == 2);
    // Failed silence write on a gone device tries once at the pacing boundary and does not spin retrying silence
    send_pair(0.6f, 0.6f);
    advance(0.005); ebPad.failReport = YES; send_pair(0, 0);
    before = ebPad.reports.count;
    attemptsBefore = ebPad.reportAttempts;
    advance(0.2);
    assert(ebPad.reports.count == before && ebPad.reportAttempts == attemptsBefore + 1);
    ebPad.failReport = NO;
    // Disconnect stops an active controller once and releases every IOKit object
    dm_vibration_update ebu2[] = {{1, 0, {0.7f, 160, 0, 320}}, {1, 1, {0.7f, 160, 0, 320}}};
    dm_haptics_send(ebu2, 2); drain();
    eb2Before = ebPad2.reports.count;
    dm_haptics_apple_controller(0, nil); dm_haptics_apple_controller(1, nil); drain();
    assert(!abitdo_of(0) && !abitdo_of(1) && ebPad.closes == 2 && ebPad2.closes == 1 && objects == 0);
    b = ebPad2.report.bytes;
    assert(ebPad2.reports.count == eb2Before + 1 && b[1] == 0 && b[2] == 0);
    advance(0.2);
    assert(ebPad2.reports.count == eb2Before + 1);

    dm_haptics_apple_shutdown();
    puts("Apple haptics: channels, pulses, idle release, startup handlers, engine replacement, backoff, stale callbacks, plain players, Joy-Con recovery and lifetime, the DualSense's handles, level cutoff, DualShock 4, Pro Controller and 8BitDo report routes passed");
} }
