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
static CFArrayRef test_hid_elements(IOHIDDeviceRef device,CFDictionaryRef match,IOOptionBits options) {
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
static CFTypeRef test_registry_property(io_registry_entry_t service,const io_name_t plane,CFStringRef key,CFAllocatorRef allocator,IOOptionBits options) {
    (void)plane; (void)key; (void)allocator; (void)options;
    id value=hid_registry[@(service)][@"interface"]; return value ? CFRetain((__bridge CFTypeRef)value) : NULL;
}
static IOReturn test_hid_open(IOHIDDeviceRef device,IOOptionBits options) {
    assert(options==kIOHIDOptionsTypeSeizeDevice); ++hid_open_count;
    NSDictionary *properties=(__bridge NSDictionary *)device;
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
static kern_return_t test_matching_services(mach_port_t port,CFDictionaryRef match,io_iterator_t *iterator) {
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
static uint64_t received[DM_PLAYERS]; static int32_t axes[DM_PLAYERS][4]; static uint32_t players;
static GCController *rumble[DM_PLAYERS];
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
void dm_haptics_apple_output(int p, id owner, dm_haptics_output output) { (void)owner; ++route_changes[p]; raw_output[p]=[output copy]; rumble[p]=nil; }
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
void dm_haptics_apple_controller(int p, GCController *c) { ++route_changes[p]; rumble[p] = c; raw_output[p]=nil; }
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
        steam_removed(NULL,kIOReturnSuccess,NULL,(__bridge IOHIDDeviceRef)device);
    }
    for(DMController *d in g_devices) controller_disconnect(d);
    g_devices=[NSMutableArray array]; g_slots=(pad_slots){0}; g_hid=NULL;
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
    int slot=peer->slot; peer->input.buttons=DM_BTN_B; controller_publish(peer);
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
        DMController *raw=g_steam_hids[@10].raw; raw->connected=YES; controllers_changed(nil);
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
