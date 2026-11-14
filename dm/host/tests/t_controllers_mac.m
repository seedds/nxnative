// GameController snapshots exercise the same read/remap/publish path as hardware.
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#include <assert.h>
// Mock only the OS HID boundary. The adapter's validation, registry discovery,
// ownership, report decoding, slot allocation and cleanup all run unchanged.
static NSMutableDictionary *hid_registry, *hid_opened, *hid_scheduled, *hid_callbacks;
static NSMutableDictionary *hid_nodes;
static NSArray *hid_scan; static NSUInteger hid_scan_index;
static BOOL hid_scan_fails, hid_scan_invalid, hid_scan_empty;
static NSArray<GCController *> *native_devices;
static int hid_open_count, hid_close_count;
static IOHIDDeviceRef hid_last_closed;
static void check_hid_stopped(IOHIDDeviceRef device);
static CFTypeRef test_hid_property(IOHIDDeviceRef device, CFStringRef key) {
    return (__bridge CFTypeRef)((__bridge NSDictionary *)device)[(__bridge NSString *)key];
}
// Match the Copy API ownership contract so callers can release the fixture array.
static CFArrayRef test_hid_elements(IOHIDDeviceRef device,CFDictionaryRef match,IOOptionBits options) CF_RETURNS_RETAINED {
    (void)match; (void)options;
    id elements=((__bridge NSDictionary *)device)[@"elements"];
    return elements ? CFRetain((__bridge CFArrayRef)elements) : NULL;
}
static IOHIDElementType test_element_type(IOHIDElementRef e) { (void)e; return kIOHIDElementTypeInput_Misc; }
static uint32_t test_element_report(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"report"] unsignedIntValue]; }
static uint32_t test_element_size(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"bits"] unsignedIntValue]; }
static uint32_t test_element_count(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"count"] unsignedIntValue]; }
static Boolean test_element_array(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"array"] boolValue]; }
static io_service_t test_hid_service(IOHIDDeviceRef device) {
    return [((__bridge NSDictionary *)device)[@"registry"] unsignedIntValue];
}
static kern_return_t test_registry_id(io_registry_entry_t service,uint64_t *id) { *id=service; return KERN_SUCCESS; }
// Keep the SDK ownership contracts when analysis does not inline these mocks.
static CFTypeRef CF_RETURNS_RETAINED test_registry_property(io_registry_entry_t service,const io_name_t plane,CFStringRef key,CFAllocatorRef allocator,IOOptionBits options) {
    (void)plane; (void)key; (void)allocator; (void)options;
    id value=hid_registry[@(service)][@"interface"]; return value ? CFRetain((__bridge CFTypeRef)value) : NULL;
}
static IOReturn test_hid_open(IOHIDDeviceRef device,IOOptionBits options) {
    NSDictionary *properties=(__bridge NSDictionary *)device;
    assert(options==kIOHIDOptionsTypeSeizeDevice || (options==kIOHIDOptionsTypeNone && [properties[@"generic"] boolValue])); ++hid_open_count;
    if([properties[@"fail"] boolValue]) return kIOReturnExclusiveAccess;
    assert(!hid_opened[properties[@"registry"]]); hid_opened[properties[@"registry"]]=properties;
    return kIOReturnSuccess;
}
static void test_hid_callback(IOHIDDeviceRef device,uint8_t *report,CFIndex size,IOHIDReportCallback callback,void *context) {
    (void)report; (void)size;
    NSNumber *id=((__bridge NSDictionary *)device)[@"registry"];
    assert(hid_opened[id]);
    if(callback) { assert(context); hid_callbacks[id]=@YES; }
    else [hid_callbacks removeObjectForKey:id];
}
static void test_hid_value_callback(IOHIDDeviceRef device,IOHIDValueCallback callback,void *context) {
    NSNumber *id=((__bridge NSDictionary *)device)[@"registry"]; assert(hid_opened[id]);
    if(callback) { assert(context); hid_callbacks[id]=@YES; }
    else [hid_callbacks removeObjectForKey:id];
}
static uint32_t test_element_page(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"page"] unsignedIntValue]; }
static uint32_t test_element_usage(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"usage"] unsignedIntValue]; }
static CFIndex test_element_min(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"min"] longValue]; }
static CFIndex test_element_max(IOHIDElementRef e) { return [((__bridge NSDictionary *)e)[@"max"] longValue]; }
static IOHIDElementRef test_value_element(IOHIDValueRef v) { return (__bridge IOHIDElementRef)((__bridge NSDictionary *)v)[@"element"]; }
static CFIndex test_value_integer(IOHIDValueRef v) { return [((__bridge NSDictionary *)v)[@"value"] longValue]; }
static void test_hid_schedule(IOHIDDeviceRef device,CFRunLoopRef loop,CFStringRef mode) {
    assert(loop==CFRunLoopGetMain() && CFEqual(mode,kCFRunLoopCommonModes));
    NSNumber *id=((__bridge NSDictionary *)device)[@"registry"]; assert(hid_opened[id] && hid_callbacks[id]); hid_scheduled[id]=@YES;
}
static void test_hid_unschedule(IOHIDDeviceRef device,CFRunLoopRef loop,CFStringRef mode) {
    (void)loop; (void)mode;
    NSNumber *id=((__bridge NSDictionary *)device)[@"registry"]; assert(hid_scheduled[id]); [hid_scheduled removeObjectForKey:id];
}
static IOReturn test_hid_close(IOHIDDeviceRef device,IOOptionBits options) {
    assert(options==kIOHIDOptionsTypeNone);
    NSNumber *id=((__bridge NSDictionary *)device)[@"registry"];
    assert(hid_opened[id] && !hid_callbacks[id] && !hid_scheduled[id]); check_hid_stopped(device);
    [hid_opened removeObjectForKey:id]; hid_last_closed=device; ++hid_close_count;
    return kIOReturnSuccess;
}
static kern_return_t test_matching_services(mach_port_t port,CFDictionaryRef match CF_RELEASES_ARGUMENT,io_iterator_t *iterator) {
    (void)port; assert([((__bridge NSDictionary *)match)[@kIOHIDVendorIDKey] intValue]==0x28de);
    CFRelease(match); if(hid_scan_fails) return kIOReturnError;
    hid_scan=hid_registry.allKeys; hid_scan_index=0; *iterator=hid_scan_empty ? 0 : 100; return KERN_SUCCESS;
}
static io_object_t test_iterator_next(io_iterator_t iterator) { assert(iterator==100); return hid_scan_index<hid_scan.count ? [hid_scan[hid_scan_index++] unsignedIntValue] : 0; }
static boolean_t test_iterator_valid(io_iterator_t iterator) { assert(iterator==100); return !hid_scan_invalid; }
static IOHIDDeviceRef test_hid_create(CFAllocatorRef allocator,io_service_t service) {
    (void)allocator; if([hid_registry[@(service)][@"create_fail"] boolValue]) return NULL;
    return (IOHIDDeviceRef)CFRetain((__bridge CFTypeRef)hid_registry[@(service)]);
}
static kern_return_t test_object_retain(io_object_t object) { (void)object; return KERN_SUCCESS; }
static boolean_t test_object_conforms(io_object_t object,const io_name_t name) { return [hid_nodes[@(object)][@"class"] isEqual:@(name)]; }
static kern_return_t test_registry_parent(io_registry_entry_t entry,const io_name_t plane,io_registry_entry_t *parent) {
    (void)plane; *parent=[hid_nodes[@(entry)][@"parent"] unsignedIntValue]; return *parent ? KERN_SUCCESS : KERN_FAILURE;
}
static kern_return_t test_object_release(io_object_t object) { (void)object; return KERN_SUCCESS; }
@interface GCController (HIDFixture)
+ (BOOL)testSupportsHIDDevice:(IOHIDDeviceRef)device;
+ (NSArray<GCController *> *)testControllers;
@end
@implementation GCController (HIDFixture)
+ (BOOL)testSupportsHIDDevice:(IOHIDDeviceRef)device { return [((__bridge NSDictionary *)device)[@"native"] boolValue]; }
+ (NSArray<GCController *> *)testControllers { return native_devices ?: @[]; }
@end
#define supportsHIDDevice testSupportsHIDDevice
#define controllers testControllers
#define IOHIDDeviceRegisterInputValueCallback test_hid_value_callback
#define IOHIDElementGetUsagePage test_element_page
#define IOHIDElementGetUsage test_element_usage
#define IOHIDElementGetLogicalMin test_element_min
#define IOHIDElementGetLogicalMax test_element_max
#define IOHIDValueGetElement test_value_element
#define IOHIDValueGetIntegerValue test_value_integer
#define IOHIDDeviceCopyMatchingElements test_hid_elements
#define IOHIDElementGetType test_element_type
#define IOHIDElementGetReportID test_element_report
#define IOHIDElementGetReportSize test_element_size
#define IOHIDElementGetReportCount test_element_count
#define IOHIDElementIsArray test_element_array
#define IOHIDDeviceGetProperty test_hid_property
#define IOHIDDeviceGetService test_hid_service
#define IORegistryEntryGetRegistryEntryID test_registry_id
#define IORegistryEntrySearchCFProperty test_registry_property
#define IOHIDDeviceOpen test_hid_open
#define IOHIDDeviceRegisterInputReportCallback test_hid_callback
#define IOHIDDeviceScheduleWithRunLoop test_hid_schedule
#define IOHIDDeviceUnscheduleFromRunLoop test_hid_unschedule
#define IOHIDDeviceClose test_hid_close
#define IOServiceGetMatchingServices test_matching_services
#define IOIteratorNext test_iterator_next
#define IOIteratorIsValid test_iterator_valid
#define IOHIDDeviceCreate test_hid_create
#define IOObjectRelease test_object_release
#define IOObjectRetain test_object_retain
#define IOObjectConformsTo test_object_conforms
#define IORegistryEntryGetParentEntry test_registry_parent
#import "../controllers_mac.m"
#include "../home_quit.h"
#include "../ctrl_prompt.h"
static void (^test_home_poll)(void);
void front_home_poll(void) { if(test_home_poll) test_home_poll(); }
static int (*test_controller_prompt_poll)(void);
int front_controller_prompt_poll(void) { return test_controller_prompt_poll ? test_controller_prompt_poll() : 0; }

static unsigned pointer_activity;
void front_game_input(void) { ++pointer_activity; }
static uint64_t received[DM_PLAYERS]; static int32_t axes[DM_PLAYERS][4]; static uint32_t players;
static GCController *rumble[DM_PLAYERS];
static void (*sampled_sink[DM_PLAYERS])(const dm_haptic_state *);
static int switch2_active = 1;
int dm_switch2_start(const dm_switch2_front *front) { (void)front; return 0; }
void dm_switch2_active(int active) { switch2_active = active; }
void dm_switch2_rumble(const dm_haptic_state *s) { (void)s; }
static unsigned route_changes[DM_PLAYERS], pad_updates[DM_PLAYERS];
static dm_haptics_output raw_output[DM_PLAYERS];
static uint8_t written[64]; static CFIndex written_n, written_id; static IOHIDReportType written_type;
static IOReturn fake_write(IOHIDDeviceRef d, IOHIDReportType type, CFIndex id, const uint8_t *b, CFIndex n) {
    assert(d); written_type=type; written_n=n; written_id=id; memcpy(written,b,n); return kIOReturnSuccess;
}
static dm_touch touches[16]; static int ntouches;
uint64_t now_ns(void) { return (uint64_t)(controller_now() * 1e9); }
static int native_motion_player = -1, native_motion_cleared = -1, native_motion_samples;
void dm_motion_set(int p, const float a[3], const float r[3]) { native_motion_player=p; native_motion_samples++; assert(a[2] == -1 && r[1] == 1); }
void dm_motion_clear(int p) { native_motion_cleared=p; }
void dm_motion_set_hid(int p, const float a[3], const float r[3], uint64_t t) { (void)p; (void)a; (void)r; (void)t; }
void dm_motion_clear_hid(int p) { (void)p; }
static dm_motion motions[DM_PLAYERS]; static int motion_present[DM_PLAYERS];
static int output_focused=1;
void dm_haptics_apple_focus(int active) { output_focused=active; }
void dm_touch_set(const dm_touch *t, int n) { ntouches=n; if(n) memcpy(touches,t,n*sizeof *t); }
void dm_pad_motion(int p, const dm_motion *m) { motion_present[p]=m!=NULL; if(m) motions[p]=*m; }
void dm_haptics_apple_output(int p, id owner, dm_haptics_output output) { (void)owner; ++route_changes[p]; raw_output[p]=[output copy]; rumble[p]=nil; sampled_sink[p]=NULL; }
dm_haptic_level dm_haptics_level(const dm_vibration *v, int count, float gain) { (void)count; return (dm_haptic_level){v->amplitude_low*gain,0}; }
static BOOL test_intensity_cutoff;
static unsigned intensity_calls;
static float intensity_inputs[2];
float dm_haptics_apple_controller_intensity(float intensity) {
    if (!test_intensity_cutoff) return intensity;
    assert(intensity_calls < 2);
    intensity_inputs[intensity_calls] = intensity;
    return intensity_calls++ ? 0.5f : 0; // the real cutoff mapping is tested in t_haptics_apple
}
void dm_pad_set_player(int p, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { ++pad_updates[p]; received[p] = b; axes[p][0]=lx; axes[p][1]=ly; axes[p][2]=rx; axes[p][3]=ry; }
void dm_pad_players(uint32_t m) { players = m; }
void dm_haptics_apple_controller(int p, GCController *c) { ++route_changes[p]; rumble[p] = c; raw_output[p]=nil; sampled_sink[p]=NULL; }
void dm_haptics_apple_set_sink(int p, void (*sink)(const dm_haptic_state *)) {
    ++route_changes[p]; sampled_sink[p] = sink; raw_output[p] = nil; rumble[p] = nil;
}
// GameController's newer live-input API is read-only. Adapt real mutable GC
// snapshots at the OS boundary and supply event times explicitly.
@interface TestButton : NSObject
@property GCControllerButtonInput *button;
@end
@implementation TestButton
- (id)pressedInput { return self; }
- (BOOL)isPressed { return self.button.isPressed; }
@end
@interface TestLiveInput : NSObject
@property GCController *snapshot;
@property NSTimeInterval lastEventTimestamp, lastEventLatency;
@property dispatch_queue_t queue;
@property(copy) void (^elementValueDidChangeHandler)(id<GCDevicePhysicalInput>, id<GCPhysicalInputElement>);
@end
@implementation TestLiveInput
- (id)capture { return self; }
- (id)buttons {
    NSMutableDictionary *buttons=[NSMutableDictionary dictionary];
    for(NSString *key in self.snapshot.physicalInputProfile.buttons) {
        TestButton *b=[TestButton new]; b.button=self.snapshot.physicalInputProfile.buttons[key]; buttons[key]=b;
    }
    return buttons;
}
- (id)dpads { return self.snapshot.physicalInputProfile.dpads; }
@end
// A queued pre-seize event updates the live state between separate timestamp
// and latency reads. Capturing first sees one coherent state at time 20.
@interface RacingLiveInput : TestLiveInput @end
@implementation RacingLiveInput
- (NSTimeInterval)lastEventTimestamp { return 10; }
- (NSTimeInterval)lastEventLatency { return 0; }
- (id)capture {
    TestLiveInput *frozen=[TestLiveInput new]; frozen.snapshot=self.snapshot;
    frozen.lastEventTimestamp=20; frozen.lastEventLatency=0; return frozen;
}
@end
@interface NamedSteamController : GCController
@property TestLiveInput *testInput;
@end
@implementation NamedSteamController
- (instancetype)init {
    if((self=[super init])) {
        self.testInput=[TestLiveInput new]; self.testInput.snapshot=[GCController controllerWithExtendedGamepad];
        self.testInput.lastEventTimestamp=10; self.testInput.lastEventLatency=1;
    }
    return self;
}
- (NSString *)vendorName { return @"Steam Controller"; }
- (NSString *)productCategory { return @"Extended Gamepad"; }
- (GCExtendedGamepad *)extendedGamepad { return self.testInput.snapshot.extendedGamepad; }
- (GCControllerLiveInput *)input { return (id)self.testInput; }
@end
static void gc_event_at(DMController *d, double time) {
    TestLiveInput *input=((NamedSteamController *)d.controller).testInput;
    input.lastEventTimestamp=time;
    assert(input.elementValueDidChangeHandler);
    input.elementValueDidChangeHandler((id)input,nil);
}
static void gc_event(DMController *d) { gc_event_at(d,d->gc_not_before+1); }
@interface TestNativeMotion : NSObject
@property BOOL hasRotationRate, hasGravityAndUserAcceleration, sensorsRequireManualActivation, sensorsActive;
@property GCAcceleration acceleration, gravity, userAcceleration;
@property GCRotationRate rotationRate;
@property(copy) GCMotionValueChangedHandler valueChangedHandler;
@end
@implementation TestNativeMotion @end
@interface MotionController : NamedSteamController
@property TestNativeMotion *testMotion;
@end
@implementation MotionController
- (GCMotion *)motion { return (id)self.testMotion; }
- (NSString *)productCategory { return @"Switch Pro Controller"; }
- (GCPhysicalInputProfile *)physicalInputProfile { return self.testInput.snapshot.physicalInputProfile; }
@end
// Dropping upstream's Nintendo label mapping must fail through the real adapter,
// including a shared remap applied after the physical buttons are normalized.
static void test_nintendo_labels(void) {
    MotionController *c = [MotionController new];
    DMController *d = controller_add_gc(c);
    GCControllerButtonInput *buttons[] = {c.extendedGamepad.buttonA, c.extendedGamepad.buttonB, c.extendedGamepad.buttonX, c.extendedGamepad.buttonY};
    uint64_t targets[] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y};
    for (int i = 0; i < 4; i++) {
        [buttons[i] setValue:1]; gc_event(d); assert(received[d->slot] == targets[i]);
        [buttons[i] setValue:0]; gc_event(d); assert(!received[d->slot]);
    }
    d->map[0] = 3;
    [c.extendedGamepad.buttonA setValue:1]; gc_event(d); assert(received[d->slot] == DM_BTN_Y);
    controller_disconnect(d); [g_devices removeObject:d]; controller_pro_changed();
    g_slots = (pad_slots){0};
}
static void test_native_motion_integration(void) {
    for (int merged = 0; merged < 2; merged++) {
        g_merge = merged; g_slots = (pad_slots){0};
        static char keyboard; if (!merged) pad_slot_take(&g_slots, &keyboard, "keyboard");
        MotionController *c = [MotionController new]; c.testMotion = [TestNativeMotion new];
        TestNativeMotion *m = c.testMotion; m.hasRotationRate = YES; m.sensorsRequireManualActivation = YES;
        m.acceleration = (GCAcceleration){0, 0, -1}; m.rotationRate = (GCRotationRate){0, 1, 0};
        DMController *d = controller_add_gc(c);
        [c.extendedGamepad.buttonB setValue:1]; gc_event(d);
        int player = merged ? 0 : 1;
        assert(d->slot == player && m.sensorsActive && m.valueChangedHandler);
        assert(g_pro_controllers == 1 && g_pro_slot == player);
        m.valueChangedHandler((id)m); assert(native_motion_player == player && native_motion_samples);
        front_controllers_focus(0); assert(!m.sensorsActive && !m.valueChangedHandler && native_motion_cleared == player && !g_pro_active);
        front_controllers_focus(1); assert(m.sensorsActive && m.valueChangedHandler && g_pro_active);
        m.valueChangedHandler((id)m); assert(native_motion_player == player);
        controller_disconnect(d); assert(!m.valueChangedHandler && !m.sensorsActive && g_pro_slot == -1);
        [g_devices removeObject:d]; controller_pro_changed(); assert(g_pro_controllers == 0);
    }
    g_slots = (pad_slots){0}; g_merge = 0;
}
static void test_native_registration_race(void) {
    NamedSteamController *native=[NamedSteamController new];
    RacingLiveInput *input=[RacingLiveInput new]; input.snapshot=native.testInput.snapshot; native.testInput=input;
    [native.extendedGamepad.buttonB setValue:1];
    DMController *d=controller_add_gc(native);
    input.elementValueDidChangeHandler((id)input,nil);
    assert(d->slot<0 && !players); // cached button must not create a duplicate player
    [g_devices removeObject:d];
}
static void test_raw_ownership(void) {
    steam_write=fake_write;
    DMController *raw=[DMController new], *empty=[DMController new];
    raw->device=(IOHIDDeviceRef)CFRetain(CFSTR("raw")); empty->device=(IOHIDDeviceRef)CFRetain(CFSTR("empty"));
    raw.name=empty.name=@"Steam Controller (2026)"; raw.model=empty.model=@"Valve Triton";
    controller_load_map(raw); controller_load_map(empty);
    GCController *native=[NamedSteamController new], *identical=[NamedSteamController new];
    g_devices=[@[empty,raw] mutableCopy];
    controllers_reconcile(@[native,identical]);
    assert(raw->slot<0 && empty->slot<0 && !players); // idle Puck owns no player
    raw->connected=YES; controllers_reconcile(@[native,identical]);
    assert(raw->slot==0 && players==1 && raw_output[0]); // ambiguous names keep all raw features
    empty->connected=YES; controllers_reconcile(@[native,identical]);
    assert(empty->slot==1 && players==3 && raw_output[1]); // identical live devices remain independent
    DMController *cached=g_devices[2], *unrelated=g_devices[3];
    [native.extendedGamepad.buttonB setValue:1];
    gc_event_at(cached,cached->gc_not_before); // event queued before the exclusive claim
    assert(cached->slot<0 && players==3);
    gc_event(unrelated); assert(unrelated->slot<0); // neutral events do not join
    [identical.extendedGamepad.leftThumbstick.xAxis setValue:.01];
    gc_event(unrelated); assert(unrelated->slot<0); // neither does idle stick noise
    front_controllers_window(0); // joining from the mapping panel must work
    [identical.extendedGamepad.buttonA setValue:1]; gc_event(unrelated);
    assert(unrelated->slot==2 && players==7 && !received[2] && !output_focused);
    front_controllers_window(1); gc_event(unrelated);
    assert(unrelated->slot==2 && players==7 && received[2]==DM_BTN_B && rumble[2]==identical);
    unsigned char packet[46]={0x42}; packet[30]=1; packet[39]=0x40; packet[45]=0x10;
    for(DMController *d in @[raw,empty]) {
        steam_report((__bridge void *)d,kIOReturnSuccess,NULL,kIOHIDReportTypeInput,0x42,packet,46);
        assert(motion_present[d->slot] && motions[d->slot].gyro[2]>0);
        d->input.pad[0]=(steam_pad){1,25000,0,1}; d->input.pad[1]=(steam_pad){1,0,0,1}; controller_publish(d);
        assert(received[d->slot]==DM_BTN_RIGHT);
        raw_output[d->slot]((dm_haptic_state){.motor={{.amplitude_low=1}}},1);
        assert(written_id==0x80 && steam_u16(written+4)==65535);
    }
    assert(ntouches==2 && touches[0].id!=touches[1].id);
    raw->connected=NO; controllers_reconcile(@[native,identical]);
    assert(raw->slot<0 && !raw_output[0] && !motion_present[0] && ntouches==1);
    assert(empty->slot==1 && raw_output[1] && unrelated->slot==2 && rumble[2]==identical);
    controllers_reconcile(@[native,identical]); assert(cached->slot<0); // cached profile cannot resurrect a slot
    raw->connected=YES; controllers_reconcile(@[native,identical]); assert(raw->slot==0 && raw_output[0]);
    for(DMController *d in g_devices) controller_disconnect(d);
    g_devices=[NSMutableArray array]; g_slots=(pad_slots){0};
}
static void check_hid_stopped(IOHIDDeviceRef device) {
    for(DMController *d in g_devices) if(d->device==device) {
        assert(d->slot<0 && !d->motion.have_tick && d->touch_id[0]<0 && d->touch_id[1]<0);
    }
}
static NSDictionary *hid_device(int id,int pid,int interface,int page,int usage,BOOL fail) {
    NSDictionary *properties=@{@"registry":@(id), @kIOHIDVendorIDKey:@0x28de, @kIOHIDProductIDKey:@(pid),
        @"interface":@(interface), @"elements":@[@{@"report":@(page==1 && (usage==2 || usage==6) ? 1 : 0x42), @"bits":@360}], @kIOHIDPrimaryUsagePageKey:@(page), @kIOHIDPrimaryUsageKey:@(usage), @"fail":@(fail), @kIOHIDPhysicalDeviceUniqueIDKey:[NSString stringWithFormat:@"fixture-%d",id]};
    hid_registry[@(id)]=properties; return properties;
}
static void test_hid_ownership(void) {
    hid_registry=[NSMutableDictionary dictionary]; hid_opened=[NSMutableDictionary dictionary];
    hid_scheduled=[NSMutableDictionary dictionary]; hid_callbacks=[NSMutableDictionary dictionary];
    g_steam_attempted=[NSMutableSet set]; steam_write=fake_write;
    hid_device(1,0x1304,0,0xff00,1,NO); // Puck non-controller interface
    hid_device(2,0x1302,0,1,2,NO);      // composite mouse
    hid_device(3,0x1303,0,1,6,NO);      // composite keyboard
    hid_device(4,0x1102,0,0xff00,1,NO); // original Steam Controller protocol
    steam_discover(); assert(!hid_open_count); // opt-out: no HID manager
    g_hid=(IOHIDManagerRef)CFSTR("test manager");
    steam_discover(); assert(!hid_open_count && !g_devices.count);
    NSDictionary *first=hid_device(5,0x1304,2,0xff00,1,NO);
    NSDictionary *second=hid_device(6,0x1304,3,0xff00,1,NO);
    hid_device(7,0x1302,0,0xff00,1,YES); // another owner denies exclusive access
    steam_discover(); assert(hid_open_count==3 && g_devices.count==2 && !players);
    DMController *raw=nil, *peer=nil;
    for(DMController *d in g_devices) { if(d->registry_id==5) raw=d; if(d->registry_id==6) peer=d; }
    assert(raw && peer && hid_scheduled.count==2);
    // The manager's handle is a different object referring to the same service.
    NSMutableDictionary *duplicate=[first mutableCopy];
    assert((__bridge IOHIDDeviceRef)duplicate!=raw->device);
    steam_added(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)duplicate);
    assert(hid_open_count==3 && g_devices.count==2);
    raw->connected=peer->connected=YES; controllers_changed(nil);
    int player=raw->slot, peer_player=peer->slot;
    assert(player>=0 && peer_player>=0 && player!=peer_player && players==3);
    assert(raw_output[player] && raw_output[peer_player]);
    raw->input.pad[1]=(steam_pad){1,0,0,1}; controller_publish(raw); assert(ntouches==1);
    [hid_registry removeObjectForKey:@5]; // removal means registry scan no longer returns it
    steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)duplicate);
    assert(hid_close_count==1 && hid_last_closed==(__bridge IOHIDDeviceRef)first);
    assert(raw->slot<0 && !raw_output[player] && ntouches==0 && peer->slot==peer_player && raw_output[peer_player]);
    assert(![g_steam_attempted containsObject:@5]);
    // Failed claims do not suppress native controls or retry midway through play.
    GCController *native=[NamedSteamController new]; controllers_reconcile(@[native]);
    DMController *d=g_devices.lastObject; [native.extendedGamepad.buttonB setValue:1]; gc_event(d);
    assert(d->slot>=0 && rumble[d->slot]==native);
    steam_discover(); assert(hid_open_count==3);
    controller_disconnect(d); [g_devices removeObject:d];
    NSDictionary *failed=hid_registry[@7]; [hid_registry removeObjectForKey:@7];
    steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)failed);
    assert(![g_steam_attempted containsObject:@7]);
    hid_device(7,0x1302,0,0xff00,1,NO); steam_discover(); assert(hid_open_count==4);
    // An unrelated hotplug must not interrupt already admitted native pads,
    // including identical names and held buttons that produce no further event.
    NamedSteamController *late=[NamedSteamController new], *unrelated=[NamedSteamController new];
    DMController *late_pad=controller_add_gc(late), *native_peer=controller_add_gc(unrelated);
    [late.extendedGamepad.buttonB setValue:1]; [unrelated.extendedGamepad.buttonA setValue:1];
    gc_event_at(late_pad,12); gc_event_at(native_peer,12);
    assert(late_pad->slot>=0 && native_peer->slot>=0);
    int late_slot=late_pad->slot, native_slot=native_peer->slot;
    unsigned old_routes=route_changes[native_slot], old_updates=pad_updates[native_slot];
    late.testInput.lastEventLatency=unrelated.testInput.lastEventLatency=8; // claim time 20
    NSDictionary *late_hid=hid_device(8,0x1303,0,0xff00,1,NO); steam_discover();
    assert(late_pad->slot==late_slot && native_peer->slot==native_slot && peer->slot==peer_player);
    assert(received[late_slot]==DM_BTN_A && received[native_slot]==DM_BTN_B && rumble[native_slot]==unrelated);
    assert(route_changes[native_slot]==old_routes && pad_updates[native_slot]==old_updates);
    controller_disconnect(late_pad); controller_disconnect(native_peer); [g_devices removeObject:late_pad]; [g_devices removeObject:native_peer];
    [hid_registry removeObjectForKey:@8]; steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)late_hid);
    for(NSDictionary *device in @[second,hid_registry[@7]]) {
        [hid_registry removeObjectForKey:device[@"registry"]];
        steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
    }
    assert(!players && !g_devices.count && !hid_opened.count && hid_close_count==4);
    g_hid=NULL; g_slots=(pad_slots){0};
}
static void hid_fixture_start(void) {
    hid_registry=[NSMutableDictionary dictionary]; hid_opened=[NSMutableDictionary dictionary];
    hid_scheduled=[NSMutableDictionary dictionary]; hid_callbacks=[NSMutableDictionary dictionary];
    g_steam_hids=nil; g_steam_fallback=nil; hid_nodes=[NSMutableDictionary dictionary];
    g_steam_attempted=[NSMutableSet set]; g_hid=(IOHIDManagerRef)CFSTR("test manager"); steam_write=fake_write;
}
static void hid_fixture_finish(void) {
    native_devices=nil;
    for(NSDictionary *device in hid_registry.allValues.copy) {
        [hid_registry removeObjectForKey:device[@"registry"]];
        hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
        steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
    }
    for(DMController *d in g_devices) controller_disconnect(d);
    g_devices=[NSMutableArray array]; g_slots=(pad_slots){0}; g_hid=NULL;
    assert(!g_hidpads && !g_hidpads_waiting && !g_hidpads_gc && (!g_hidpad_deferred || !CFArrayGetCount(g_hidpad_deferred)));
}
static void test_native_event_admission(BOOL composite) {
    hid_fixture_start();
    NamedSteamController *other=[NamedSteamController new], *late=[NamedSteamController new];
    native_devices=@[other,late];
    DMController *held=controller_add_gc(other);
    [other.extendedGamepad.buttonA setValue:1]; [other.extendedGamepad.leftThumbstick.xAxis setValue:.5]; gc_event(held);
    assert(held->slot==0 && received[0]==DM_BTN_B && axes[0][0]==16383);
    unsigned routes=route_changes[0], updates=pad_updates[0];
    DMController *candidate=controller_add_gc(late); // native notification beats HID callback
    [late.extendedGamepad.buttonB setValue:1]; late.testInput.lastEventLatency=8;
    NSMutableDictionary *source=[hid_device(9,0x1304,2,1,composite ? 2 : 5,NO) mutableCopy];
    source[@"native"]=@YES; hid_registry[@9]=source;
    if(composite) {
        // Two collections in the same Puck port share its interface ancestor.
        hid_nodes[@9]=@{@"parent":@102}; hid_nodes[@102]=@{@"class":@"IOUSBHostInterface"};
        steam_discover(); assert(!hid_opened.count); // native-only discovery remains undecided
        gc_event_at(candidate,10); // stale cached input cannot commit native fallback
        [late.extendedGamepad.buttonB setValue:0]; gc_event_at(candidate,12); // neither can neutral input
        assert(candidate->slot<0 && !g_steam_fallback.count);
        [late.extendedGamepad.buttonB setValue:1];
        hid_device(10,0x1304,2,0xff00,1,NO); hid_nodes[@10]=@{@"parent":@102};
    }
    gc_event_at(candidate,12); // first physical event must discover and claim its source
    assert(candidate->slot<0 && hid_opened[@9] && players==1);
    assert(held->slot==0 && received[0]==DM_BTN_B && axes[0][0]==16383 && rumble[0]==other);
    assert(route_changes[0]==routes && pad_updates[0]==updates);
    DMController *raw=g_devices.lastObject;
    if(composite) {
        assert(hid_opened[@9] && hid_opened[@10]);
        assert(!hid_callbacks[@9] && hid_callbacks[@10]); // suppression-only source has no decoder
    }
    unsigned char packet[46]={0x42}; packet[30]=1; packet[39]=0x40; packet[45]=0x10;
    steam_report((__bridge void *)raw,kIOReturnSuccess,NULL,kIOHIDReportTypeInput,0x42,packet,46);
    assert(raw->slot==1 && motion_present[1] && raw_output[1] && candidate->slot<0 && players==3);
    assert(held->slot==0 && received[0]==DM_BTN_B && route_changes[0]==routes && pad_updates[0]==updates);
    gc_event_at(candidate,19); assert(candidate->slot<0); // queued pre-claim native input stays suppressed
    for(NSDictionary *device in hid_registry.allValues.copy) {
        [hid_registry removeObjectForKey:device[@"registry"]];
        steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
        assert(held->slot==0 && received[0]==DM_BTN_B && rumble[0]==other && route_changes[0]==routes && pad_updates[0]==updates);
    }
    hid_fixture_finish();
}
static void test_sibling_fallback(int failure) {
    hid_fixture_start();
    NamedSteamController *other=[NamedSteamController new], *native=[NamedSteamController new];
    native_devices=@[other,native];
    DMController *held=controller_add_gc(other); [other.extendedGamepad.buttonA setValue:1]; gc_event(held);
    unsigned routes=route_changes[0], updates=pad_updates[0];
    DMController *candidate=controller_add_gc(native); [native.extendedGamepad.buttonB setValue:1];
    // Direct USB siblings can be different interfaces of one controller.
    NSMutableDictionary *source=[hid_device(9,0x1302,0,1,5,failure==1) mutableCopy];
    source[@"elements"]=@[@{@"report":@1,@"bits":@360}]; // standard gamepad, no Triton state
    source[@"native"]=@YES; hid_registry[@9]=source;
    hid_nodes[@9]=@{@"parent":@100}; hid_nodes[@100]=@{@"class":@"IOUSBHostDevice"};
    if(failure==3) {
        gc_event(candidate); // native admission freezes this unresolved group
        assert(candidate->slot==1 && !hid_opened.count);
    }
    hid_device(10,0x1302,2,0xff00,1,failure==2); hid_nodes[@10]=@{@"parent":@100};
    steam_discover();
    if(failure) {
        assert(!hid_opened.count && !hid_scheduled.count && !hid_callbacks.count);
        gc_event(candidate); assert(candidate->slot==1 && players==3 && rumble[1]==native);
        assert(!raw_output[1] && received[1]==DM_BTN_A);
        // A rescan cannot seize the successful sibling behind native fallback.
        int opens=hid_open_count; steam_discover(); assert(hid_open_count==opens);
    } else {
        assert(hid_opened.count==2 && hid_callbacks.count==1 && !hid_callbacks[@9]);
        DMController *raw=g_steam_hids[@10].raw; assert(raw);
        unsigned char packet[46]={0x42}; packet[30]=1;
        steam_report((__bridge void *)raw,kIOReturnSuccess,NULL,kIOHIDReportTypeInput,0x42,packet,46);
        assert(raw->slot==1 && candidate->slot<0 && players==3);
    }
    assert(held->slot==0 && received[0]==DM_BTN_B && rumble[0]==other);
    assert(route_changes[0]==routes && pad_updates[0]==updates);
    hid_fixture_finish(); assert(!hid_opened.count);
}
static void test_puck_transaction_isolation(void) {
    hid_fixture_start();
    hid_nodes[@9]=@{@"parent":@102}; hid_nodes[@102]=@{@"class":@"IOUSBHostInterface",@"parent":@100};
    hid_nodes[@10]=@{@"parent":@103}; hid_nodes[@103]=@{@"class":@"IOUSBHostInterface",@"parent":@100};
    hid_nodes[@100]=@{@"class":@"IOUSBHostDevice"};
    hid_device(9,0x1304,2,0xff00,1,NO); hid_device(10,0x1304,3,0xff00,1,NO); steam_discover();
    DMController *peer=g_steam_hids[@9].raw, *raw=g_steam_hids[@10].raw;
    peer->connected=raw->connected=YES; controllers_changed(nil);
    int slot=peer->slot; assert(slot>=0 && slot<DM_PLAYERS);
    peer->input.buttons=DM_BTN_B; controller_publish(peer);
    unsigned routes=route_changes[slot], updates=pad_updates[slot];
    // A late native collection fails on port 3; only port 3 rolls back.
    NSMutableDictionary *source=[hid_device(11,0x1304,3,1,2,YES) mutableCopy];
    source[@"native"]=@YES; hid_registry[@11]=source; hid_nodes[@11]=@{@"parent":@103}; steam_discover();
    assert(raw->slot<0 && !hid_opened[@10] && hid_opened[@9]);
    assert(peer->slot==slot && received[slot]==DM_BTN_B && raw_output[slot]);
    assert(route_changes[slot]==routes && pad_updates[slot]==updates);
    // Removing one sibling cannot clear the failure latch for the remaining one.
    [hid_registry removeObjectForKey:@11]; steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)source);
    int opens=hid_open_count; steam_discover(); assert(hid_open_count==opens);
    hid_fixture_finish(); assert(!hid_opened.count);
}
static void test_unknown_hid_identity(BOOL raw_first) {
    hid_fixture_start();
    NSMutableDictionary *device=[hid_device(9,0x1303,0,0xff00,1,NO) mutableCopy];
    device[@"native"]=@YES; device[@kIOHIDPhysicalDeviceUniqueIDKey]=@42; hid_registry[@9]=device;
    if(raw_first) {
        [hid_registry removeObjectForKey:@9];
        hid_device(10,0x1303,0,0xff00,1,NO); steam_discover();
        DMController *raw=g_steam_hids[@10].raw; assert(raw);
        raw->connected=YES; controllers_changed(nil);
        assert(raw->slot==0); raw->input.buttons=DM_BTN_A; controller_publish(raw);
        hid_registry[@9]=device;
    }
    NamedSteamController *native=[NamedSteamController new]; native_devices=@[native];
    DMController *d=controller_add_gc(native); [native.extendedGamepad.buttonA setValue:1]; gc_event(d);
    if(raw_first) {
        assert(d->slot<0 && hid_opened.count==1 && players==1 && received[0]==DM_BTN_A);
    } else {
        assert(d->slot==0 && received[0]==DM_BTN_B && !hid_opened.count);
        // A sibling with asymmetric metadata must not create a raw duplicate.
        hid_device(10,0x1303,0,0xff00,1,NO); steam_discover();
        assert(!hid_opened.count && players==1 && received[0]==DM_BTN_B);
        [hid_registry removeObjectForKey:@9]; steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
        steam_discover(); assert(!hid_opened.count); // latch lasts for the remaining sibling
    }
    hid_fixture_finish();
}
static void test_protocol_metadata(void) {
    BOOL complete=YES;
    NSDictionary *short_report=@{@"elements":@[@{@"report":@0x42,@"bits":@180,@"count":@2}]};
    assert(!steam_protocol((__bridge IOHIDDeviceRef)short_report,&complete) && complete);
    NSDictionary *array_report=@{@"elements":@[@{@"report":@0x45,@"bits":@8,@"count":@45,@"array":@YES}]};
    assert(steam_protocol((__bridge IOHIDDeviceRef)array_report,&complete) && complete);
    assert(!steam_protocol((__bridge IOHIDDeviceRef)@{},&complete) && !complete);
}
static void test_incomplete_discovery(void) {
    hid_fixture_start();
    NamedSteamController *c=[NamedSteamController new]; native_devices=@[c];
    DMController *d=controller_add_gc(c); [c.extendedGamepad.buttonB setValue:1];
    hid_scan_fails=YES; gc_event(d); assert(d->slot<0 && !players); hid_scan_fails=NO;
    hid_scan_invalid=YES; gc_event(d); assert(d->slot<0 && !players); hid_scan_invalid=NO;
    NSMutableDictionary *device=[hid_device(9,0x1302,0,0xff00,1,NO) mutableCopy]; hid_registry[@9]=device;
    device[@"create_fail"]=@YES; gc_event(d); assert(d->slot<0 && !players); [device removeObjectForKey:@"create_fail"];
    device[@"registry"]=@0; gc_event(d); assert(d->slot<0 && !players); device[@"registry"]=@9;
    [device removeObjectForKey:@kIOHIDPrimaryUsagePageKey]; gc_event(d); assert(d->slot<0 && !players);
    [hid_registry removeAllObjects]; hid_scan_empty=YES;
    gc_event(d); assert(d->slot==0 && received[0]==DM_BTN_A); hid_scan_empty=NO;
    hid_fixture_finish();
    // The opt-out admits native input even if raw discovery would fail.
    hid_scan_fails=YES; c=[NamedSteamController new]; d=controller_add_gc(c);
    [c.extendedGamepad.buttonB setValue:1]; gc_event(d); assert(d->slot==0 && received[0]==DM_BTN_A);
    controller_disconnect(d); [g_devices removeObject:d]; hid_scan_fails=NO; g_slots=(pad_slots){0};
}
@interface TestHaptics : NSObject @end
@implementation TestHaptics
- (NSArray *)supportedLocalities { return @[GCHapticsLocalityDefault]; }
@end
@interface HapticController : NamedSteamController @end
@implementation HapticController
- (GCDeviceHaptics *)haptics { return (id)[TestHaptics new]; }
@end
static void test_merged_hotplug(void) {
    g_merge=1; steam_write=fake_write;
    DMController *puck=[DMController new]; puck.name=@"Steam Controller (2026)"; puck.model=@"Valve Triton";
    puck->device=(IOHIDDeviceRef)CFRetain(CFSTR("idle Puck")); [g_devices addObject:puck]; controller_load_map(puck);
    GCController *c=[HapticController new]; DMController *held=controller_add_gc(c);
    [c.extendedGamepad.buttonA setValue:1]; gc_event(held);
    assert(rumble[0]==c && received[0]==DM_BTN_B);
    unsigned routes=route_changes[0];
    puck->connected=YES; controller_connect(puck); controller_publish(puck);
    assert(held->slot==0 && received[0]==DM_BTN_B && rumble[0]==c && route_changes[0]==routes);
    controller_disconnect(puck); assert(rumble[0]==c && received[0]==DM_BTN_B && route_changes[0]==routes);
    controller_connect(puck); controller_disconnect(held); assert(raw_output[0]); // ownership changes only when current owner leaves
    controller_disconnect(puck); assert(!raw_output[0] && !rumble[0]);
    g_devices=[NSMutableArray array]; g_slots=(pad_slots){0}; g_merge=0;
}
// A launcher-authored map must reach both adapters, and a panel edit must be
// visible to the launcher in the same domain. Each run owns a disposable domain.
static void test_shared_controls(void) {
    NSString *domain=[@"org.nxnative.test.controls." stringByAppendingString:NSUUID.UUID.UUIDString];
    g_controls_domain=domain;
    CFPreferencesSetAppValue(CFSTR("launcher.DM_PAD_MAP"),CFSTR("A=B,B=A"),(__bridge CFStringRef)domain);
    assert(CFPreferencesAppSynchronize((__bridge CFStringRef)domain));
    controller_reload_maps();
    DMController *native=[DMController new]; native.model=@"Native"; controller_load_map(native);
    DMController *steam=[DMController new]; steam.model=@"Valve Triton"; controller_load_map(steam);
    assert(native->map[0]==1 && steam->map[1]==0);
    [g_devices addObject:native]; [g_devices addObject:steam];
    assert(controller_set_button(0,3));
    assert(native->map[0]==3 && steam->map[0]==3);
    id saved=CFBridgingRelease(CFPreferencesCopyAppValue(CFSTR("launcher.DM_PAD_MAP"),(__bridge CFStringRef)domain));
    assert([saved isEqual:@"A=Y,B=A"]);
    // Extra buttons are final Switch targets: the shared map is not applied twice.
    steam->map[16]=0; steam->pad_mode[0]=PAD_RIGHT_STICK; steam->gyro_enabled=NO;
    assert(controller_save_map(steam)); controller_load_map(steam);
    assert(pad_map_buttons(1u<<16,steam->map)==DM_BTN_A && steam->pad_mode[0]==PAD_RIGHT_STICK && !steam->gyro_enabled);
    // Standalone and launcher/file overrides are temporary and lock standard rows.
    setenv("DM_PAD_MAP","A=X",1); unsetenv("DM_ENV_LAUNCH_KEYS"); controller_reload_maps();
    assert(steam->map[0]==2 && g_map_fixed && !controller_set_button(0,1));
    unsetenv("DM_PAD_MAP"); setenv("DM_ENV_LAUNCH_KEYS","",1); controller_reload_maps();
    assert(steam->map[0]==3 && !g_map_fixed);
    // Launcher-exported saved values are refreshed from the domain, not mistaken for overrides.
    setenv("DM_PAD_MAP","A=B",1); controller_reload_maps(); assert(steam->map[0]==3);
    setenv("DM_ENV_FILE_KEYS","DM_PAD_MAP",1); controller_reload_maps(); assert(steam->map[0]==1 && g_map_fixed);
    unsetenv("DM_ENV_FILE_KEYS"); unsetenv("DM_PAD_MAP"); unsetenv("DM_ENV_LAUNCH_KEYS");
    setenv("DM_SAVED_CHOICES","0",1); controller_reload_maps();
    assert(steam->map[0]==0 && steam->map[16]==-1 && steam->gyro_enabled);
    assert(controller_set_button(0,2)); steam->map[16]=1; assert(controller_save_map(steam));
    controller_reload_maps(); assert(steam->map[0]==2 && steam->map[16]==1);
    saved=CFBridgingRelease(CFPreferencesCopyAppValue(CFSTR("launcher.DM_PAD_MAP"),(__bridge CFStringRef)domain));
    assert([saved isEqual:@"A=Y,B=A"]);
    unsetenv("DM_SAVED_CHOICES"); controller_reload_maps(); assert(steam->map[0]==3 && steam->map[16]==0);
    // A model dictionary cannot override standard buttons or reinterpret malformed fields.
    NSDictionary *bad=@{@"buttons":@{@"0":@2,@"16":@1.5,@"17":@63,@"18":@64},@"pads":@[@1.5,@5],@"gyro":@"false"};
    CFPreferencesSetAppValue(CFSTR("controller.Valve Triton"),(__bridge CFDictionaryRef)bad,(__bridge CFStringRef)domain);
    controller_reload_maps();
    assert(steam->map[0]==3 && steam->map[16]==-1 && steam->map[17]==63 && steam->map[18]==-1);
    assert(steam->pad_mode[0]==PAD_DPAD && steam->pad_mode[1]==PAD_TOUCH && steam->gyro_enabled);
    // Removing a choice restores defaults; a bad value cannot partially change a saved map.
    CFPreferencesSetAppValue(CFSTR("launcher.DM_PAD_MAP"),CFSTR("A=X,Q=B"),(__bridge CFStringRef)domain);
    controller_reload_maps(); assert(steam->map[0]==0);
    assert(controller_set_button(0,63)); controller_reload_maps(); assert(steam->map[0]==63);
    assert(controller_set_button(0,0));
    assert(!CFPreferencesCopyAppValue(CFSTR("launcher.DM_PAD_MAP"),(__bridge CFStringRef)domain));
    for(NSString *key in @[@"launcher.DM_PAD_MAP",@"controller.Valve Triton"]) CFPreferencesSetAppValue((__bridge CFStringRef)key,NULL,(__bridge CFStringRef)domain);
    assert(CFPreferencesAppSynchronize((__bridge CFStringRef)domain));
    g_devices=[NSMutableArray array];
}
// The OS boundary supplies real decoder values; routing, mappings, focus and
// mixed transport ownership are the production code above, with no device I/O.
static NSDictionary *generic_device(int id) {
    return @{@"registry":@(id), @"generic":@YES, @kIOHIDVendorIDKey:@PAD_HID_DEFAULT_VENDOR,
        @kIOHIDProductIDKey:@1, @kIOHIDProductKey:@"8BitDo fixture",
        @"elements":@[@{@"page":@1,@"usage":@0x30},@{@"page":@1,@"usage":@0x31}]};
}
static void generic_value(hid_pad *pad,int page,int usage,int value,int min,int max) {
    NSDictionary *v=@{@"element":@{@"page":@(page),@"usage":@(usage),@"min":@(min),@"max":@(max)},@"value":@(value)};
    hidpad_value(pad,kIOReturnSuccess,NULL,(__bridge IOHIDValueRef)v);
}
// The real adapter callbacks and prompt state machine, without a window or a
// wall-clock delay. Only admitted GC/Triton inputs can operate the prompt.
static void test_home_quit_routing(void) {
    hid_fixture_start();
    DMController *raw=[DMController new]; raw.name=raw.model=@"Triton";
    raw->device=(IOHIDDeviceRef)CFRetain(CFSTR("quit controller")); raw->connected=YES;
    raw->map[22]=0; // short Guide presses deliver Switch A
    raw->map[0]=raw->map[1]=3; // both physical face buttons deliver Switch Y
    [g_devices addObject:raw]; controller_connect(raw);
    const int raw_slot=raw->slot; assert(raw_slot==0);

    NamedSteamController *native=[NamedSteamController new], *cached=[NamedSteamController new];
    native_devices=@[native,cached];
    controller_add_gc(cached); [cached.extendedGamepad.buttonA setValue:1]; // no fresh input to admit this profile
    DMController *gc=controller_add_gc(native);
    [native.extendedGamepad.buttonX setValue:1]; gc_event(gc);
    const int gc_slot=gc->slot; assert(gc_slot==1);
    NSDictionary *props=generic_device(907); IOHIDDeviceRef hid=(__bridge IOHIDDeviceRef)props;
    hidpad_found(NULL,kIOReturnSuccess,NULL,hid);
    DMController *generic=controller_for_hid(g_hidpads); assert(generic);
    assert(generic->slot < 0);
    generic_value(g_hidpads,9,2,1,0,1);
    const int generic_slot=generic->slot; assert(generic_slot==2);

    __block home_quit q={0}; __block double now=0;
    test_home_poll=^{
        int home,a,b; front_controllers_quit_buttons(&home,&a,&b);
        front_controllers_quit_prompt(hq_blocks_input(hq_step(&q,now,home,a,b)));
    };
    void (^report)(unsigned char,unsigned char)=^(unsigned char faces,unsigned char guide) {
        uint8_t bytes[46]={0x42,0,faces,0,guide};
        steam_report((__bridge void *)raw,kIOReturnSuccess,NULL,kIOHIDReportTypeInput,0x42,bytes,sizeof bytes);
    };
    GCControllerButtonInput *logo=native.extendedGamepad.buttonHome;
    assert(logo && logo.pressedChangedHandler);
    [logo setValue:1]; logo.pressedChangedHandler(logo,1,YES);
    int home,a,b; assert(front_controllers_quit_buttons(&home,&a,&b)==native && home && !a && !b);
    assert(q.phase==HQ_HOLDING); now+=HQ_HOLD_S; front_home_poll(); assert(q.phase==HQ_PROMPT);
    [logo setValue:0]; logo.pressedChangedHandler(logo,0,NO);
    [logo setValue:1]; logo.pressedChangedHandler(logo,1,YES); assert(q.phase==HQ_SETTLE);
    [logo setValue:0]; logo.pressedChangedHandler(logo,0,NO); assert(q.phase==HQ_IDLE);

    report(0,1);
    assert(q.phase==HQ_HOLDING && received[raw_slot]==DM_BTN_A && received[generic_slot]==DM_BTN_A);
    dm_touch finger={0,.5,.5}; front_controllers_mouse(&finger);
    now+=HQ_HOLD_S; front_home_poll();
    assert(q.phase==HQ_PROMPT && !output_focused && !received[raw_slot] && !received[gc_slot] && !received[generic_slot] && !ntouches);
    report(0,0); assert(q.phase==HQ_PROMPT); // the cached native A cannot quit
    report(2,0); assert(q.phase==HQ_SETTLE && !received[raw_slot]); // raw wire B cancels despite remapping
    front_controllers_mouse(&finger); assert(!ntouches);
    report(0,0); assert(q.phase==HQ_IDLE && output_focused && received[gc_slot] && received[generic_slot]==DM_BTN_A);
    report(0,1); now+=HQ_HOLD_S; front_home_poll(); assert(q.phase==HQ_PROMPT);
    front_controllers_window(0); hq_cancel(&q); front_controllers_quit_prompt(0);
    front_controllers_window(1); assert(q.phase==HQ_IDLE); // raw Guide held across focus loss
    report(0,0); report(0,1); now+=HQ_HOLD_S; front_home_poll();
    report(0,0); [native.extendedGamepad.buttonB setValue:1]; gc_event(gc);
    assert(q.phase==HQ_SETTLE && !received[gc_slot]); // native B cancels a raw Guide hold
    [native.extendedGamepad.buttonB setValue:0]; gc_event(gc);
    assert(q.phase==HQ_IDLE && received[gc_slot]);
    report(0,1); now+=HQ_HOLD_S; front_home_poll(); report(1,1);
    assert(q.phase==HQ_QUIT && !received[raw_slot]); // wire A quits, regardless of its guest mapping
    test_home_poll=nil; front_controllers_quit_prompt(0);
    hidpad_removed(NULL,kIOReturnSuccess,NULL,hid); hid_fixture_finish();
    puts("Home quit: adapter ownership, physical labels, prompt/settle gating and focus recovery: OK");
}
static void test_generic_hid_routing(void) {
    for(int merged=0;merged<2;merged++) {
        hid_fixture_start(); g_merge=merged; g_slots=(pad_slots){0};
        static char keyboard; if(!merged) pad_slot_take(&g_slots,&keyboard,"keyboard");
        NamedSteamController *native=[NamedSteamController new]; native_devices=@[native];
        DMController *gc=controller_add_gc(native); [native.extendedGamepad.buttonA setValue:1];
        unsigned activity=pointer_activity; gc_event(gc); assert(pointer_activity>activity);
        int gcslot=gc->slot; assert(gcslot==(merged ? 0 : 1));
        NSDictionary *props=generic_device(901); IOHIDDeviceRef hid=(__bridge IOHIDDeviceRef)props;
        setenv("DM_HID_MAP","A=3",1); hidpad_found(NULL,kIOReturnSuccess,NULL,hid); unsetenv("DM_HID_MAP");
        hid_pad *pad=g_hidpads; assert(pad && pad->hid==hid);
        DMController *d=controller_for_hid(pad); assert(d && !d->device && !d.controller);
        if (!merged) {
            assert(d->slot < 0 && players == 3); // A receiver with no button events takes no player.
            generic_value(pad,9,3,1,0,1);
            generic_value(pad,9,3,0,0,1);
        }
        assert(d->slot==(merged ? 1 : 2) && gc->slot==gcslot && players==(merged ? 1u : 7u));
        assert(pad->st.has[PH_X] && pad->st.has[PH_Y]);
        assert(rumble[gcslot]==native && !raw_output[d->slot]);
        CFIndex old_written=written_n; steam_keepalive(d); assert(written_n==old_written);
        d->map[0]=3; // raw usage3 -> A, then shared A -> Y
        int player=merged ? 0 : d->slot;
        activity=pointer_activity; generic_value(pad,9,3,1,0,1);
        assert(received[player]==(DM_BTN_Y | (merged ? DM_BTN_B : 0)) && pointer_activity>activity);
        generic_value(pad,1,0x30,100,0,100); assert(axes[player][0]==DM_STICK_MAX);
        front_controllers_window(0); assert(!received[player] && !axes[player][0]);
        activity=pointer_activity; generic_value(pad,9,3,0,0,1); generic_value(pad,9,3,1,0,1);
        assert(!received[player] && pointer_activity==activity);
        front_controllers_window(1); assert(received[player]==(DM_BTN_Y | (merged ? DM_BTN_B : 0)) && axes[player][0]==DM_STICK_MAX);
        NSMutableDictionary *alias = [props mutableCopy]; alias[@"match"] = @2;
        IOHIDDeviceRef other = (__bridge IOHIDDeviceRef)alias;
        int opens = hid_open_count;
        hidpad_found(NULL,kIOReturnSuccess,NULL,hid);
        hidpad_found(NULL,kIOReturnSuccess,NULL,other);
        assert(g_hidpads==pad && !pad->next && hid_open_count==opens);
        int closes = hid_close_count;
        hidpad_removed(NULL,kIOReturnSuccess,NULL,other);
        hidpad_removed(NULL,kIOReturnSuccess,NULL,hid);
        assert(hid_close_count==closes+1 && hid_last_closed==hid);
        assert(!g_hidpads && !d->generic && ![g_devices containsObject:d] && !hid_opened[@901]);
        assert(received[player]==(merged ? DM_BTN_B : 0) && rumble[gcslot]==native);
        hidpad_found(NULL,kIOReturnSuccess,NULL,hid);
        if (!merged) {
            assert(controller_for_hid(g_hidpads)->slot < 0);
            generic_value(g_hidpads,9,2,1,0,1);
            generic_value(g_hidpads,9,2,0,0,1);
        }
        assert(controller_for_hid(g_hidpads)->slot==(merged ? 1 : 2));
        controller_disconnect(gc); [g_devices removeObject:gc]; native_devices=@[];
        activity=pointer_activity; generic_value(g_hidpads,1,0x30,51,0,100); assert(pointer_activity==activity); // drift
        generic_value(g_hidpads,1,0x30,100,0,100); assert(pointer_activity>activity);
        generic_value(g_hidpads,1,0x30,50,0,100);
        hidpad_removed(NULL,kIOReturnSuccess,NULL,hid); assert(!received[player] && !hid_opened.count);
        hid_fixture_finish(); g_merge=0;
    }
    puts("Generic HID mixed slots, remapping, focus, merged release and pointer activity: OK");
}
static void test_generic_hid_ownership(void) {
    hid_fixture_start();
    // Reject native/synthetic devices before opening, and leave no state on failure.
    for(NSString *key in @[@"native",@kIOHIDGCSyntheticDeviceKey,@"fail"]) {
        NSMutableDictionary *props=[generic_device(902) mutableCopy]; props[key]=@YES;
        IOHIDDeviceRef match = (__bridge IOHIDDeviceRef)props;
        hidpad_found(NULL,kIOReturnSuccess,NULL,match);
        assert(!g_hidpads && !hid_opened.count && !g_devices.count);
        // A promised but unlisted GC match is retained for main's timed fallback.
        if ([key isEqual:@"native"]) {
            NSMutableDictionary *alias = [props mutableCopy]; alias[@"match"] = @2;
            IOHIDDeviceRef other = (__bridge IOHIDDeviceRef)alias;
            hidpad_found(NULL,kIOReturnSuccess,NULL,other);
            assert(g_hidpads_waiting && g_hidpads_waiting->hid == match && !g_hidpads_waiting->next);
            hidpad_removed(NULL,kIOReturnSuccess,NULL,other);
            assert(!g_hidpads_waiting);
        }
    }
    // Callback order cannot let a generic sibling pre-open the raw group.
    NSMutableDictionary *sibling=[hid_device(903,0x1302,0,1,5,NO) mutableCopy];
    sibling[@"elements"]=@[]; sibling[@kIOHIDPhysicalDeviceUniqueIDKey]=@"mixed";
    hid_registry[@903]=sibling;
    NSMutableDictionary *protocol=[hid_device(904,0x1302,1,0xff00,1,NO) mutableCopy];
    protocol[@kIOHIDPhysicalDeviceUniqueIDKey]=@"mixed"; hid_registry[@904]=protocol;
    hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)sibling);
    assert(!g_hidpads && hid_opened.count==2 && g_devices.count==1);
    DMController *raw=g_devices.firstObject; assert(raw->device && !raw->generic);
    raw->connected=YES; controllers_reconcile(@[]);
    raw->input.buttons=DM_BTN_A; unsigned activity=pointer_activity; controller_publish(raw); assert(pointer_activity>activity);
    hid_fixture_finish();
    // Failed raw ownership permits an unsupported standard collection to work
    // generically, and that admitted fallback cannot be seized by later scans.
    hid_fixture_start();
    sibling=[hid_device(905,0x1302,0,1,5,NO) mutableCopy];
    sibling[@"elements"]=@[]; sibling[@"generic"]=@YES; sibling[@kIOHIDPhysicalDeviceUniqueIDKey]=@"fallback";
    hid_registry[@905]=sibling;
    protocol=[hid_device(906,0x1302,1,0xff00,1,YES) mutableCopy];
    protocol[@kIOHIDPhysicalDeviceUniqueIDKey]=@"fallback"; hid_registry[@906]=protocol;
    hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)sibling);
    assert(g_hidpads && g_devices.count==1 && !g_devices.firstObject->device);
    assert([g_steam_fallback containsObject:@"hid:4866:fallback:-1"]);
    unsigned opened=hid_open_count;
    protocol[@"fail"]=@NO; // permission becomes available, but no mid-play takeover
    steam_discover(); assert((unsigned)hid_open_count==opened && g_devices.count==1);
    hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)sibling); hid_fixture_finish();
    hid_fixture_start();
    setenv("DM_HID_PADS","0",1); hidpad_start(); unsetenv("DM_HID_PADS");
    assert(!g_hidpad_manager && g_hid); // generic opt-out does not disable Triton
    g_hid=NULL; // DM_STEAM_HID=0: generic gamepads remain independently available
    NSDictionary *plain=generic_device(907);
    hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)plain); assert(g_hidpads);
    hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)plain); hid_fixture_finish();
    puts("Generic HID GC/synthetic exclusions and Triton ownership coordination: OK");
}
static void test_generic_unknown_identity(void) {
    for(int raw_first=0;raw_first<2;raw_first++) {
        hid_fixture_start();
        NSDictionary *raw=hid_device(908,0x1302,1,0xff00,1,NO);
        if(raw_first) steam_discover(); else [hid_registry removeObjectForKey:@908];
        NSMutableDictionary *unknown=[hid_device(909,0x1302,0,1,5,NO) mutableCopy];
        unknown[@"elements"]=@[]; unknown[@"generic"]=@YES;
        [unknown removeObjectForKey:@kIOHIDPhysicalDeviceUniqueIDKey]; hid_registry[@909]=unknown;
        hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)unknown);
        if(raw_first) {
            assert(!g_hidpads && g_devices.count==1 && g_devices.firstObject->device);
            [hid_registry removeObjectForKey:@908];
            steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)raw);
            assert(g_hidpads && g_devices.count==1 && !g_devices.firstObject->device);
            hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)unknown);
        } else {
            assert(g_hidpads && g_devices.count==1 && !g_devices.firstObject->device);
            hid_registry[@908]=raw; steam_discover();
            assert(hid_opened.count==1 && !g_devices.firstObject->device);
            hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)unknown);
        }
        hid_fixture_finish();
    }
    // Incomplete registry scans defer input without losing the matching event.
    hid_fixture_start();
    NSMutableDictionary *deferred=[hid_device(910,0x1302,0,1,5,NO) mutableCopy];
    deferred[@"elements"]=@[]; deferred[@"generic"]=@YES; hid_registry[@910]=deferred;
    NSMutableDictionary *deferred_alias = [deferred mutableCopy]; deferred_alias[@"match"] = @2;
    hid_scan_fails=YES; hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)deferred);
    hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)deferred_alias);
    assert(!g_hidpads && g_hidpad_deferred && CFArrayGetCount(g_hidpad_deferred)==1);
    hid_scan_fails=NO; controllers_changed(nil);
    assert(g_hidpads && !g_hidpads->next && CFArrayGetCount(g_hidpad_deferred)==0);
    hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)deferred); hid_fixture_finish();
    // Puck interface metadata is optional for a standard nonprotocol gamepad;
    // without it no physical port may be guessed for a raw ownership claim.
    hid_fixture_start();
    NSMutableDictionary *puck=[hid_device(911,0x1304,-1,1,5,NO) mutableCopy];
    puck[@"elements"]=@[]; puck[@"generic"]=@YES; hid_registry[@911]=puck;
    hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)puck);
    assert(g_hidpads && g_devices.count==1 && !g_devices.firstObject->device);
    hidpad_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)puck); hid_fixture_finish();
    puts("Generic HID missing identity and deferred admission recover without duplicate raw routes: OK");
}
@interface TwinFixtureController : NamedSteamController @end
@implementation TwinFixtureController
- (NSString *)vendorName { return @"8BitDo fixture"; }
@end
static void test_generic_twins(void) {
    for (int fail = 0; fail < 2; fail++) {
        hid_fixture_start();
        TwinFixtureController *first = [TwinFixtureController new];
        native_devices = @[first];
        controllers_changed(nil);
        DMController *native = g_devices.firstObject;
        assert(native && native.controller == first);
        [first.extendedGamepad.buttonA setValue:1];
        gc_event(native);
        assert(native->slot == 0);
        NSMutableDictionary *left = [generic_device(913) mutableCopy], *right = [generic_device(914) mutableCopy];
        left[@"native"] = right[@"native"] = @YES;
        left[@kIOHIDLocationIDKey] = @1; right[@kIOHIDLocationIDKey] = @2;
        left[@"fail"] = @(fail);
        hid_registry[@913] = left; hid_registry[@914] = right;
        g_twins_due = 1; // Exercise the takeover after main's normal listing window.
        hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)left);
        NSMutableDictionary *alias = [left mutableCopy]; alias[@"match"] = @2;
        hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)alias);
        assert(native->slot == 0 && !g_hidpads && g_hidpads_gc && !g_hidpads_gc->next);
        hidpad_found(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)right);
        for (hid_pad *entry = g_hidpads_gc; entry; entry = entry->next) if (entry->opened) hidpad_heard(entry, 1);
        controllers_changed(nil);
        g_twins_due = 0;
        if (fail) {
            assert(g_devices.count == 1 && native->slot == 0 && hid_opened.count == 1 && !g_hidpads);
        } else {
            assert(native->slot < 0 && g_devices.count == 2 && hid_opened.count == 2 && players == 3);
            assert(controller_for_hid(g_hidpads)->slot == 1 && g_hidpads->hid == (__bridge IOHIDDeviceRef)right);
            native_devices = @[first, [TwinFixtureController new]];
            controllers_changed(nil);
            assert(!g_hidpads && !hid_opened.count && g_devices.count == 2);
            for (DMController *d in g_devices) assert(d.controller && !d->generic);
        }
        hid_fixture_finish();
    }
    puts("Identical HID: complete takeover, GC handback and failed-open preservation passed");
}

static ctrl_prompt test_applet;
static int poll_test_applet(void) {
    int a, b;
    front_controllers_applet_buttons(&test_applet, &a, &b);
    cp_phase phase = cp_step(&test_applet, __builtin_popcount(players), front_controllers_waiting(), a, b, 0, 0);
    if (phase == CP_IDLE) front_controllers_applet(0);
    return cp_blocks_input(phase);
}
static void test_controller_applet_routing(void) {
    hid_fixture_start();
    g_press_to_join = 1;
    NamedSteamController *native = [NamedSteamController new];
    native_devices = @[native];
    DMController *gc = controller_add_gc(native);
    NSDictionary *props = generic_device(912);
    IOHIDDeviceRef device = (__bridge IOHIDDeviceRef)props;
    setenv("DM_HID_MAP","A=3,B=4",1);
    hidpad_found(NULL,kIOReturnSuccess,NULL,device);
    unsetenv("DM_HID_MAP");
    hid_pad *pad = g_hidpads;
    assert(pad);
    DMController *raw = controller_for_hid(pad);
    assert(raw && raw->slot < 0 && gc->slot < 0 && front_controllers_waiting() == 2);
    raw->map[0] = 3; // Guest remapping must not change the applet's physical A.
    cp_open(&test_applet, 1, 2, 0, 0, 0);
    test_controller_prompt_poll = poll_test_applet;
    front_controllers_applet(1);
    [native.extendedGamepad.leftThumbstick.xAxis setValue:.5];
    gc_event(gc);
    assert(gc->slot < 0 && !players);
    generic_value(pad,9,3,1,0,1);
    assert(raw->slot == 0 && players == 1 && test_applet.phase == CP_OPEN && !received[0]);
    generic_value(pad,9,3,0,0,1);
    front_controllers_window(0);
    generic_value(pad,9,3,1,0,1);
    assert(test_applet.phase == CP_OPEN); // An unfocused press cannot confirm.
    front_controllers_window(1);
    generic_value(pad,9,3,1,0,1);
    assert(test_applet.phase == CP_OPEN); // Still held from before recovery.
    generic_value(pad,9,3,0,0,1);
    front_controllers_modal(1);
    generic_value(pad,9,4,1,0,1);
    assert(test_applet.phase == CP_OPEN); // A modal keyboard owns new input.
    front_controllers_modal(0);
    generic_value(pad,9,4,1,0,1);
    assert(test_applet.phase == CP_OPEN);
    generic_value(pad,9,4,0,0,1);
    generic_value(pad,9,3,1,0,1);
    assert(test_applet.phase == CP_SETTLE && test_applet.ok && !received[0]);
    front_controllers_window(0);
    assert(poll_test_applet()); // A stays held across loss of focus.
    front_controllers_window(1);
    generic_value(pad,9,3,0,0,1);
    assert(test_applet.phase == CP_IDLE && g_focus && !received[0]);
    generic_value(pad,9,3,1,0,1);
    assert(received[0] == DM_BTN_Y);
    [native.extendedGamepad.buttonA setValue:1];
    gc_event(gc);
    assert(gc->slot == 1 && players == 3);
    test_controller_prompt_poll = NULL;
    g_press_to_join = 0;
    hidpad_removed(NULL,kIOReturnSuccess,NULL,device);
    hid_fixture_finish();
    puts("Controller applet: button-only join, physical confirm and release gating passed");
}

static void test_switch2_registry(void) {
    hid_fixture_start();
    g_press_to_join = 1;
    s2_pad pad = {.name = "Bluetooth Pro", .st.pro.have = 1};
    pad.st.pro.r = (s2_report){.lx=2048, .ly=2048, .rx=2048, .ry=2048};
    assert(switch2_attach(&pad, 0));
    s2_pad waiting = {.name = "Bluetooth peer", .st.pro.have = 1};
    assert(switch2_attach(&waiting, 0));
    DMController *ble = controller_for_switch2(&pad);
    assert(ble && switch2_player(&pad) < 0 && front_controllers_waiting() == 2 && !sampled_sink[0]);
    ble->map[0] = 3;
    cp_open(&test_applet, 1, 2, 0, 0, 0);
    test_controller_prompt_poll = poll_test_applet;
    front_controllers_applet(1);
    assert(switch2_blocked() && !switch2_active);

    pad.st.pro.r.buttons = S2_A;
    switch2_read(&pad);
    assert(switch2_player(&pad) == 0 && sampled_sink[0] == dm_switch2_rumble);
    assert(players == 1 && test_applet.phase == CP_OPEN && !received[0]);
    pad.st.pro.r.buttons = 0;
    switch2_read(&pad);
    front_controllers_modal(1);
    pad.st.pro.r.buttons = S2_A;
    switch2_read(&pad);
    front_controllers_modal(0);
    switch2_read(&pad);
    assert(test_applet.phase == CP_OPEN);
    pad.st.pro.r.buttons = 0;
    switch2_read(&pad);
    pad.st.pro.r.buttons = S2_A;
    switch2_read(&pad);
    assert(test_applet.phase == CP_SETTLE && test_applet.ok && !received[0]);
    front_controllers_window(0);
    assert(poll_test_applet());
    pad.st.pro.r.buttons = 0;
    switch2_read(&pad);
    assert(test_applet.phase == CP_IDLE && switch2_blocked() && !received[0]);
    front_controllers_window(1);
    assert(switch2_active && !switch2_blocked());
    pad.st.pro.r.buttons = S2_A | S2_HOME;
    switch2_read(&pad);
    int home, a, b;
    front_controllers_quit_buttons(&home, &a, &b);
    assert(home && a && !b && received[0] == DM_BTN_Y);
    test_controller_prompt_poll = NULL;

    s2_pad split = {.name="Bluetooth Joy-Con", .st.left.have=1};
    assert(switch2_attach(&split, 1) && switch2_player(&split) == 1);
    switch2_detach(&split);
    static const char reserved[DM_PLAYERS - 1];
    for (int i = 0; i < DM_PLAYERS - 1; i++) assert(pad_slot_take_lowest(&g_slots, &reserved[i], "occupied") > 0);
    assert(!switch2_attach(&split, 1) && !controller_for_switch2(&split));
    assert(front_controllers_waiting() == 1 && switch2_player(&waiting) < 0);
    for (int i = 0; i < DM_PLAYERS - 1; i++) pad_slot_free(&g_slots, &reserved[i]);
    switch2_detach(&waiting);
    switch2_detach(&pad);
    assert(!players && !received[0] && !sampled_sink[0] && switch2_player(&pad) < 0);

    g_press_to_join = 0;
    g_merge = 1;
    DMController *triton = [DMController new];
    triton.name = @"Triton test"; triton->device = (IOHIDDeviceRef)CFRetain(CFSTR("test device"));
    triton->input.buttons = DM_BTN_X;
    [g_devices addObject:triton]; controller_connect(triton); controller_publish(triton);
    assert(raw_output[0] && !sampled_sink[0]);
    assert(switch2_attach(&pad, 0) && switch2_player(&pad) == 0);
    assert(raw_output[0] && !sampled_sink[0]); // connection cannot steal the effect
    pad.st.pro.r.buttons = S2_A;
    switch2_read(&pad);
    assert(sampled_sink[0] == dm_switch2_rumble && !raw_output[0] && received[0] == (DM_BTN_X | DM_BTN_A));
    controller_haptic_input(triton, YES);
    assert(raw_output[0] && !sampled_sink[0]);
    switch2_read(&pad);
    assert(raw_output[0] && !sampled_sink[0]); // a repeated report cannot reclaim the effect
    switch2_detach(&pad);
    assert(raw_output[0] && received[0] == DM_BTN_X);
    controller_disconnect(triton);
    [g_devices removeObject:triton]; g_merge = 0;
    hid_fixture_finish();
    puts("Switch 2 registry: shared admission, physical prompt, focus release and Triton coexistence passed");
}

int main(int argc, char **argv) {
    @autoreleasepool {
        g_devices = [NSMutableArray array]; g_maps = [NSMutableDictionary dictionary];
        pad_map_default(&g_button_map);
        // Upstream focus handling releases only input the front end owns;
        // DM_CTL may be holding a scripted finger with no mouse/pad touch.
        dm_touch scripted={0,.25,.75}; dm_touch_set(&scripted,1);
        front_controllers_window(0); assert(ntouches==1 && touches[0].x==.25);
        front_controllers_window(1); dm_touch_set(NULL,0);
        test_nintendo_labels();
        test_native_motion_integration();
        test_native_registration_race();
        test_protocol_metadata();
        test_raw_ownership();
        test_hid_ownership();
        test_native_event_admission(NO);
        test_native_event_admission(YES);
        for(int failure=0;failure<4;failure++) test_sibling_fallback(failure);
        test_puck_transaction_isolation();
        test_unknown_hid_identity(NO);
        test_unknown_hid_identity(YES);
        test_incomplete_discovery();
        test_merged_hotplug();
        test_generic_hid_routing();
        test_home_quit_routing();
        test_generic_hid_ownership();
        test_generic_unknown_identity();
        test_controller_applet_routing();
        test_switch2_registry();
        test_generic_twins();
        test_shared_controls();
        NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        [NSFileManager.defaultManager createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil];
        g_controls_domain=[@"org.nxnative.test.controllers." stringByAppendingString:NSUUID.UUID.UUIDString];
        controller_reload_maps();
        GCController *c = [NamedSteamController new];
        DMController *d = controller_add_gc(c);
        [c.extendedGamepad.buttonB setValue:1]; [c.extendedGamepad.leftThumbstick.xAxis setValue:0.5]; gc_event(d);
        assert(received[0] == DM_BTN_A && axes[0][0] == 16383 && players == 1);
        assert(controller_set_button(0,3)); controller_publish(d); assert(received[0] == DM_BTN_Y);
        controller_reload_maps();
        pad_physical_default(d->map, 0); controller_load_map(d); assert(d->map[0] == 3);
        [c.extendedGamepad.buttonB setValue:0]; gc_event(d); assert(!received[0]);
        [c.extendedGamepad.buttonA setValue:1]; gc_event(d); assert(received[0] == DM_BTN_B);
        front_controllers_window(0); assert(!received[0]);
        front_controllers_window(1); assert(received[0]==DM_BTN_B); // restore held native state without another event
        controller_disconnect(d); assert(!received[0] && !players);
        controller_connect(d); gc_event(d); assert(d->slot == 0 && received[0] == DM_BTN_B);
        // A raw Steam pad shares the same slots and mapping, including extras.
        DMController *steam = [DMController new]; steam.name = @"Steam Controller (2026)"; steam.model = @"Valve Triton";
        [g_devices addObject:steam]; controller_load_map(steam); controller_connect(steam);
        steam->connected=YES; // already connected fixture; packets below exercise state updates
        assert(steam->slot == 1 && players == 3);
        assert(steam->pad_mode[0] == PAD_DPAD && steam->pad_mode[1] == PAD_TOUCH && steam->gyro_enabled);
        steam->input.pad[0]=(steam_pad){1,25000,-25000,1};
        steam->input.pad[1]=(steam_pad){1,-32767,32767,1};
        controller_publish(steam);
        assert(received[1] == (DM_BTN_RIGHT|DM_BTN_DOWN));
        assert(ntouches==1 && touches[0].x==0 && touches[0].y==0);
        unsigned finger=touches[0].id;
        dm_touch mouse={0,.5,.5}; front_controllers_mouse(&mouse);
        assert(ntouches==2 && touches[1].id==finger);
        front_controllers_mouse(NULL); assert(ntouches==1 && touches[0].id==finger);
        steam->pad_mode[0]=PAD_LEFT_STICK; controller_publish(steam);
        assert(!received[1] && axes[1][0]==25000 && axes[1][1]==-25000);
        assert(controller_save_map(steam)); steam->pad_mode[0]=PAD_OFF; controller_load_map(steam);
        assert(steam->pad_mode[0]==PAD_LEFT_STICK);
        unsigned char packet[46]={0x42}; packet[30]=1; packet[38]=0; packet[39]=0x40; packet[44]=0; packet[45]=0x10;
        steam_report((__bridge void *)steam,kIOReturnSuccess,NULL,kIOHIDReportTypeInput,0x42,packet,46);
        assert(motion_present[1] && motions[1].acc[2]==-1 && motions[1].gyro[2]>0);
        assert(ntouches==0); // finger-up packet releases a held pad
        steam->input.pad[1]=(steam_pad){1,0,0,1}; controller_publish(steam); assert(ntouches==1);
        front_controllers_focus(0); assert(!received[1] && ntouches==0 && !motion_present[1]);
        front_controllers_focus(1); assert(ntouches==0); // needs fresh input after focus loss
        front_controllers_window(0); assert(!output_focused);
        front_controllers_focus(1); assert(!output_focused); // app can be active with another window key
        steam_report((__bridge void *)steam,kIOReturnSuccess,NULL,kIOHIDReportTypeInput,0x42,packet,46);
        assert(!motion_present[1] && !received[1]);
        front_controllers_window(1); assert(output_focused);
        gc_event(d);
        steam->input=(steam_state){0};
        steam->input.buttons = 1u << 16; steam->map[16] = 0; controller_publish(steam); assert(received[1] == DM_BTN_A);
        controller_disconnect(steam); assert(received[1] == 0 && players == 1);
        assert(!motion_present[1] && ntouches==0);
        // Merge mode releases a removed pad without clearing a held peer.
        g_merge = 1; controller_connect(steam); steam->input.buttons = 1u << 16; controller_publish(steam);
        assert(rumble[0] == c);
        assert(received[0] == (DM_BTN_A | DM_BTN_B));
        controller_disconnect(steam); assert(received[0] == DM_BTN_B && rumble[0] == c);
        GCController *second = [NamedSteamController new];
        DMController *other = controller_add_gc(second);
        [second.extendedGamepad.buttonB setValue:1]; gc_event(other);
        [second.extendedGamepad.buttonB setValue:0]; gc_event(other);
        controller_disconnect(d); assert(received[0] == 0 && rumble[0] == second);
        controller_disconnect(other); assert(rumble[0] == nil);
        if (argc == 3 && !strcmp(argv[1], "--ui-smoke")) {
            [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            g_merge = 0; controller_connect(d); controller_connect(steam);
            front_controllers_menu(); [g_panel show:nil];
            assert(g_panel.choices.count == 2 && g_panel.buttons.count == DM_PAD_BUTTONS);
            [g_panel.devices selectItemAtIndex:1]; [g_panel selectDevice:g_panel.devices];
            assert(g_panel.selected == steam);
            steam->motion.have_tick=1;
            [g_panel.buttons[0] selectItemAtIndex:4]; [g_panel mapButton:g_panel.buttons[0]];
            assert(steam->map[0]==3 && d->map[0]==3 && steam->motion.have_tick);
            setenv("DM_PAD_MAP","A=B",1); controller_reload_maps(); [g_panel refresh];
            assert(!g_panel.buttons[0].enabled && g_panel.buttons[16].enabled && !g_panel.presets[0].enabled);
            unsetenv("DM_PAD_MAP"); controller_reload_maps(); [g_panel refresh];
            assert(g_panel.buttons[0].enabled && steam->map[0]==3);
            [g_panel.buttons[16] selectItemAtIndex:2]; [g_panel mapButton:g_panel.buttons[16]];
            assert(steam->map[16] == 1);
            [g_panel.pads[0] selectItemAtIndex:PAD_DPAD]; [g_panel mapPad:g_panel.pads[0]];
            assert(steam->pad_mode[0]==PAD_DPAD);
            g_panel.gyro.state=NSControlStateValueOff; [g_panel mapGyro:g_panel.gyro]; assert(!steam->gyro_enabled);
            g_panel.gyro.state=NSControlStateValueOn; [g_panel mapGyro:g_panel.gyro]; assert(steam->gyro_enabled);
            steam->motion.have_tick=1; [g_panel recenterGyro:nil]; assert(!steam->motion.have_tick);
            [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
            NSView *view = g_panel.panel.contentView;
            NSBitmapImageRep *bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
            [view cacheDisplayInRect:view.bounds toBitmapImageRep:bitmap];
            assert([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES]);
            if (getenv("DM_UI_HOLD")) [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:60]];
            [g_panel.panel close];
            puts("Controller mapping panel actions and rendering: OK");
        }
        // The direct route writes output report 0x80 and feature report 1; no
        // hardware is touched. The actual haptics queue/lifecycle is tested separately.
        g_merge=0; steam_write=fake_write; controller_disconnect(steam);
        steam->device=(IOHIDDeviceRef)CFRetain(CFSTR("test device")); controller_connect(steam);
        assert(raw_output[steam->slot]);
        dm_haptic_state effect={.player=steam->slot,.motor={{.amplitude_low=.5f},{.amplitude_low=1}}};
        raw_output[steam->slot](effect,1);
        assert(written_n==10 && written_type==kIOHIDReportTypeOutput && written_id==0x80);
        assert(steam_u16(written+4)==32767 && steam_u16(written+7)==65535);
        test_intensity_cutoff=YES;
        effect.motor[0].amplitude_low=.15f; effect.motor[1].amplitude_low=.3625f;
        raw_output[steam->slot](effect,2);
        assert(intensity_calls==2 && fabsf(intensity_inputs[0]-.3f)<1e-6f && fabsf(intensity_inputs[1]-.725f)<1e-6f);
        assert(steam_u16(written+4)==0 && steam_u16(written+7)==32767);
        test_intensity_cutoff=NO;
        raw_output[steam->slot]((dm_haptic_state){0},1); assert(!steam_u16(written+4) && !steam_u16(written+7));
        steam_keepalive(steam); assert(written_n==64 && written_type==kIOHIDReportTypeFeature && written_id==1 && written[3]==48 && written[4]==0x18);
        steam->gyro_enabled=NO; steam_keepalive(steam); assert(written[4]==0);
        controller_disconnect(steam);
        // An old 24-number mapping can be imported without overwriting shared choices on load.
        NSMutableArray *legacy=[NSMutableArray array]; for(int i=0;i<DM_PAD_BUTTONS;i++) [legacy addObject:@(i==0 ? 1 : -1)];
        g_legacy_maps=@{steam.model:legacy};
        controller_load_map(steam); assert(steam->map[0]==3); // never silently overrides shared choices
        assert(controller_import_map(steam)); controller_reload_maps();
        assert(steam->map[0]==1 && d->map[0]==1);
        for(NSString *key in @[@"launcher.DM_PAD_MAP",@"controller.Valve Triton",controller_map_key(d)]) CFPreferencesSetAppValue((__bridge CFStringRef)key,NULL,(__bridge CFStringRef)g_controls_domain);
        assert(CFPreferencesAppSynchronize((__bridge CFStringRef)g_controls_domain));
        [NSFileManager.defaultManager removeItemAtPath:tmp error:nil];
        puts("GameController routing, remapping, persistence and disconnect: OK");
    }
}
