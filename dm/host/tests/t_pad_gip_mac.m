// Synthetic IOUSBHost transport, detach-race, and slot-ownership tests for pad_gip_mac.h (GIP Xbox One + XUSB Xbox 360).
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOUSBHost/IOUSBHost.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dm_input.h"
#include "../pad_slots.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
__attribute__((format(printf, 1, 2))) static void test_log(const char *fmt, ...) { (void)fmt; }
#define LOG(...) test_log(__VA_ARGS__)

static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta);
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t q, dispatch_block_t b);
static void test_dispatch_async(dispatch_queue_t q, dispatch_block_t b);

// Fake USB registry nodes and IOUSBHost objects
@interface TestUSBNode : NSObject
@property uint64_t entry;
@property NSString *serviceClass;
@property NSMutableDictionary *props;
@property NSMutableArray<TestUSBNode *> *children;
@property NSData *configDesc;
@property (weak) id hostDevice;
@end
@implementation TestUSBNode
- (instancetype)init {
    if ((self = [super init])) { _props = [NSMutableDictionary new]; _children = [NSMutableArray new]; }
    return self;
}
@end

@interface TestUSBPipe : NSObject
@property uint8_t address;
@property BOOL isIn;
@property NSMutableArray *pendingBufs;
@property NSMutableArray *pendingHandlers;
@property BOOL failNext;
@end
@implementation TestUSBPipe
- (instancetype)init {
    if ((self = [super init])) { _pendingBufs = [NSMutableArray new]; _pendingHandlers = [NSMutableArray new]; }
    return self;
}
- (BOOL)enqueueIORequestWithData:(NSMutableData *)data completionTimeout:(NSTimeInterval)t error:(NSError **)err
               completionHandler:(void (^)(IOReturn, NSUInteger))handler {
    (void)t;
    if (_failNext) {
        if (err) *err = [NSError errorWithDomain:@"TestUSB" code:1 userInfo:nil];
        return NO;
    }
    [_pendingBufs addObject:data];
    [_pendingHandlers addObject:[handler copy]];
    return YES;
}
- (void)completeNext:(IOReturn)st bytes:(const void *)bytes length:(NSUInteger)len {
    assert(_pendingHandlers.count > 0);
    NSMutableData *buf = _pendingBufs[0];
    void (^h)(IOReturn, NSUInteger) = _pendingHandlers[0];
    [_pendingBufs removeObjectAtIndex:0];
    [_pendingHandlers removeObjectAtIndex:0];
    if (bytes && len) memcpy(buf.mutableBytes, bytes, len);
    h(st, len);
}
@end

// Integer handle table for Mach/IOKit 32-bit io_object_t / io_iterator_t / io_service_t handles
@interface TestIOHandle : NSObject
@property id obj;
@property int refs;
@end
@implementation TestIOHandle
@end
static NSMutableDictionary<NSNumber *, TestIOHandle *> *g_handles;
static uint32_t g_next_handle = 1;
static io_object_t io_retain_obj(id obj) {
    if (!obj) return 0;
    if (!g_handles) g_handles = [NSMutableDictionary new];
    uint32_t h = g_next_handle++;
    TestIOHandle *entry = [TestIOHandle new];
    entry.obj = obj;
    entry.refs = 1;
    g_handles[@(h)] = entry;
    return (io_object_t)h;
}
static id io_lookup(io_object_t h) {
    TestIOHandle *entry = g_handles[@(h)];
    assert(entry && entry.refs > 0);
    return entry.obj;
}
static void io_release_handle(io_object_t h) {
    if (!h) return;
    TestIOHandle *entry = g_handles[@(h)];
    assert(entry && entry.refs > 0);
    if (--entry.refs == 0) [g_handles removeObjectForKey:@(h)];
}

@interface TestUSBInterface : NSObject
@property TestUSBNode *node;
@property NSData *configDesc;
@property NSUInteger ifaceOffset;
@property NSMutableArray<TestUSBPipe *> *pipes;
@property BOOL destroyed;
@end
@implementation TestUSBInterface
- (instancetype)initWithIOService:(io_service_t)svc options:(IOUSBHostObjectInitOptions)opt queue:(dispatch_queue_t)q
                            error:(NSError **)err interestHandler:(id)ih {
    (void)opt; (void)q; (void)err; (void)ih;
    if ((self = [super init])) {
        _node = io_lookup(svc);
        _pipes = [NSMutableArray new];
        // Locate this interface's config descriptor and descriptor offset from its parent or own property
        _configDesc = _node.configDesc;
        _ifaceOffset = [_node.props[@"_ifaceOffset"] unsignedIntegerValue];
    }
    return self;
}
- (const IOUSBConfigurationDescriptor *)configurationDescriptor { return _configDesc.bytes; }
- (const IOUSBInterfaceDescriptor *)interfaceDescriptor {
    if (!_configDesc || _ifaceOffset >= _configDesc.length) return NULL;
    return (const IOUSBInterfaceDescriptor *)((const uint8_t *)_configDesc.bytes + _ifaceOffset);
}
- (TestUSBPipe *)copyPipeWithAddress:(NSUInteger)addr error:(NSError **)err {
    (void)err;
    TestUSBPipe *p = [TestUSBPipe new];
    p.address = (uint8_t)addr;
    p.isIn = (addr & 0x80) != 0;
    [_pipes addObject:p];
    return p;
}
- (NSMutableData *)ioDataWithCapacity:(NSUInteger)cap error:(NSError **)err { (void)err; return [NSMutableData dataWithLength:cap]; }
- (void)destroy {
    _destroyed = YES;
    for (TestUSBPipe *p in _pipes) {
        while (p.pendingHandlers.count > 0) {
            NSMutableData *buf = p.pendingBufs[0];
            void (^h)(IOReturn, NSUInteger) = p.pendingHandlers[0];
            [p.pendingBufs removeObjectAtIndex:0];
            [p.pendingHandlers removeObjectAtIndex:0];
            test_dispatch_async(dispatch_get_main_queue(), ^{ (void)buf; h(kIOReturnAborted, 0); });
        }
    }
}
@end

@interface TestUSBDevice : NSObject
@property TestUSBNode *node;
@property (nonatomic, copy) void (^interestHandler)(id, uint32_t, void *);
@property BOOL configured;
@property BOOL destroyed;
@property NSUInteger configuredValue;
@property (nonatomic, copy) void (^onConfigure)(TestUSBDevice *dev);
@end
@implementation TestUSBDevice
- (instancetype)initWithIOService:(io_service_t)svc options:(IOUSBHostObjectInitOptions)opt queue:(dispatch_queue_t)q
                            error:(NSError **)err interestHandler:(void (^)(id, uint32_t, void *))ih {
    (void)opt; (void)q; (void)err;
    if ((self = [super init])) {
        _node = io_lookup(svc);
        _interestHandler = [ih copy];
        _node.hostDevice = self;
    }
    return self;
}
- (io_service_t)ioService { return io_retain_obj(_node); }
- (const IOUSBConfigurationDescriptor *)configurationDescriptorWithIndex:(NSUInteger)idx error:(NSError **)err {
    (void)idx; (void)err;
    return _node.configDesc.bytes;
}
- (BOOL)configureWithValue:(NSUInteger)val matchInterfaces:(BOOL)m error:(NSError **)err {
    (void)m; (void)err;
    _configured = YES;
    _configuredValue = val;
    if (_onConfigure) _onConfigure(self);
    return YES;
}
- (void)destroy { _destroyed = YES; }
@end

// Registry & iterator shims
static NSMutableArray<TestUSBNode *> *g_registry;
static uint64_t g_next_entry = 100;
@interface TestIOIter : NSObject
@property NSArray *items;
@property NSUInteger idx;
@end
@implementation TestIOIter
@end

static kern_return_t test_child_iter(io_registry_entry_t e, const io_name_t plane, io_iterator_t *it) {
    (void)plane;
    TestUSBNode *n = io_lookup(e);
    TestIOIter *iter = [TestIOIter new];
    iter.items = [n.children copy];
    *it = io_retain_obj(iter);
    return KERN_SUCCESS;
}
static boolean_t test_conforms(io_object_t o, const io_name_t cls) {
    TestUSBNode *n = io_lookup(o);
    return [n.serviceClass isEqualToString:@(cls)];
}
static io_object_t test_iter_next(io_iterator_t it) {
    TestIOIter *iter = io_lookup(it);
    if (iter.idx >= iter.items.count) return 0;
    return io_retain_obj(iter.items[iter.idx++]);
}
static kern_return_t test_obj_release(io_object_t o) { io_release_handle(o); return KERN_SUCCESS; }
static CFTypeRef test_create_prop(io_registry_entry_t e, CFStringRef key, CFAllocatorRef a, IOOptionBits o) {
    (void)a; (void)o;
    TestUSBNode *n = io_lookup(e);
    id v = n.props[(__bridge NSString *)key];
    return v ? CFBridgingRetain(v) : NULL;
}
static io_service_t test_get_matching_service(mach_port_t p, CFDictionaryRef match) {
    (void)p;
    NSDictionary *d = CFBridgingRelease(match);
    uint64_t entry = [d[@kIORegistryEntryIDKey] unsignedLongLongValue];
    for (TestUSBNode *n in g_registry) if (n.entry == entry) return io_retain_obj(n);
    return 0;
}
static kern_return_t test_entry_id(io_registry_entry_t e, uint64_t *id) {
    TestUSBNode *n = io_lookup(e);
    *id = n.entry;
    return KERN_SUCCESS;
}
static IONotificationPortRef test_port_create(mach_port_t p) { (void)p; return (IONotificationPortRef)(uintptr_t)0x1234; }
static void test_port_queue(IONotificationPortRef p, dispatch_queue_t q) { (void)p; (void)q; }
static void test_port_destroy(IONotificationPortRef p) { (void)p; }
static NSMutableArray *g_watches;
static kern_return_t test_add_match(IONotificationPortRef port, const io_name_t type, CFDictionaryRef match,
                                    IOServiceMatchingCallback cb, void *ref, io_iterator_t *it) {
    (void)port; (void)type;
    NSDictionary *d = CFBridgingRelease(match);
    NSDictionary *pm = d[@kIOPropertyMatchKey];
    [g_watches addObject:@{@"props": pm, @"cb": [NSValue valueWithPointer:(void *)cb], @"ref": [NSValue valueWithPointer:ref]}];
    TestIOIter *iter = [TestIOIter new];
    NSMutableArray *matched = [NSMutableArray new];
    for (TestUSBNode *n in g_registry) {
        if ([n.props[@"bDeviceClass"] isEqual:pm[@"bDeviceClass"]] &&
            [n.props[@"bDeviceSubClass"] isEqual:pm[@"bDeviceSubClass"]] &&
            [n.props[@"bDeviceProtocol"] isEqual:pm[@"bDeviceProtocol"]]) [matched addObject:n];
    }
    iter.items = matched;
    *it = io_retain_obj(iter);
    return KERN_SUCCESS;
}

// Deterministic timer & async queue
static dispatch_time_t timer_now = 1, timer_due[128];
static __strong dispatch_block_t timer_block[128];
static int ntimers;
static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta) {
    assert(when == DISPATCH_TIME_NOW && delta >= 0);
    return timer_now + (uint64_t)delta;
}
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t q, dispatch_block_t b) {
    (void)q; assert(ntimers < 128);
    timer_due[ntimers] = when; timer_block[ntimers++] = [b copy];
}
static void test_dispatch_async(dispatch_queue_t q, dispatch_block_t b) {
    test_dispatch_after(timer_now, q, b);
}
static void advance_timers(uint64_t ns) {
    dispatch_time_t end = timer_now + ns;
    for (;;) {
        int first = -1;
        for (int i = 0; i < ntimers; i++) if (timer_due[i] <= end && (first < 0 || timer_due[i] < timer_due[first])) first = i;
        if (first < 0) break;
        timer_now = timer_due[first];
        dispatch_block_t b = timer_block[first];
        for (int i = first; i + 1 < ntimers; i++) { timer_due[i] = timer_due[i + 1]; timer_block[i] = timer_block[i + 1]; }
        timer_block[--ntimers] = nil;
        b();
    }
    timer_now = end;
}
static void flush_async(void) { advance_timers(0); }

#define IOUSBHostDevice TestUSBDevice
#define IOUSBHostInterface TestUSBInterface
#define IOUSBHostPipe TestUSBPipe
#define IOUSBHostObject TestUSBDevice
#define IORegistryEntryGetChildIterator test_child_iter
#define IOObjectConformsTo test_conforms
#define IOIteratorNext test_iter_next
#define IOObjectRelease test_obj_release
#define IORegistryEntryCreateCFProperty test_create_prop
#define IOServiceGetMatchingService test_get_matching_service
#define IORegistryEntryGetRegistryEntryID test_entry_id
#define IONotificationPortCreate test_port_create
#define IONotificationPortSetDispatchQueue test_port_queue
#define IONotificationPortDestroy test_port_destroy
#define IOServiceAddMatchingNotification test_add_match
#define dispatch_time test_dispatch_time
#define dispatch_after test_dispatch_after
#define dispatch_async test_dispatch_async

#include "../pad_gip_mac.h"

#undef dispatch_async
#undef dispatch_after
#undef dispatch_time
#undef IOServiceAddMatchingNotification
#undef IONotificationPortDestroy
#undef IONotificationPortSetDispatchQueue
#undef IONotificationPortCreate
#undef IORegistryEntryGetRegistryEntryID
#undef IOServiceGetMatchingService
#undef IORegistryEntryCreateCFProperty
#undef IOObjectRelease
#undef IOIteratorNext
#undef IOObjectConformsTo
#undef IORegistryEntryGetChildIterator
#undef IOUSBHostObject
#undef IOUSBHostPipe
#undef IOUSBHostInterface
#undef IOUSBHostDevice

// Front-end slot/detach/prompt counters mimicking front_mac.m
static pad_slots g_slots;
static int g_press_to_join, g_merge;
static uint64_t g_player_btn[DM_PLAYERS];
static int32_t g_player_axes[DM_PLAYERS][4];
static uint32_t g_players_mask;
static int g_cp_polls, g_reads, g_attaches, g_detaches;

static void gippad_read(gip_pad *d) {
    g_reads++;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, d);
    if (p < 0 && (!g_press_to_join || !d->st.buttons || (p = pad_slot_take_lowest(&g_slots, d, d->name)) < 0)) return;
    g_players_mask = pad_slot_mask(&g_slots);
    g_player_btn[p] = d->st.buttons;
    memcpy(g_player_axes[p], d->st.s, sizeof g_player_axes[p]);
}
static void gippad_attach(gip_pad *d) {
    g_attaches++;
    if (g_press_to_join && !g_merge) { g_cp_polls++; return; }
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name);
    if (p >= 0 && !g_merge) g_players_mask = pad_slot_mask(&g_slots);
    g_cp_polls++;
}
static void gippad_detach(gip_pad *d) {
    g_detaches++;
    if (g_merge) return;
    int p = pad_slot_free(&g_slots, d);
    if (p < 0) { g_cp_polls++; return; }
    memset(g_player_axes[p], 0, sizeof g_player_axes[p]);
    g_player_btn[p] = 0;
    g_players_mask = pad_slot_mask(&g_slots);
    g_cp_polls++;
}

// Config descriptor builders
static NSData *make_config(uint8_t cfg_val, uint8_t cls, uint8_t sub, uint8_t pro, uint8_t ep_in, uint8_t ep_out, NSUInteger *iface_off) {
    uint8_t buf[64]; size_t pos = 0;
    // Configuration descriptor (9 bytes)
    buf[0] = 9; buf[1] = kIOUSBDescriptorTypeConfiguration; buf[4] = 1; buf[5] = cfg_val;
    pos = 9;
    if (iface_off) *iface_off = pos;
    // Interface descriptor (9 bytes)
    buf[pos + 0] = 9; buf[pos + 1] = kIOUSBDescriptorTypeInterface;
    buf[pos + 4] = (ep_in ? 1 : 0) + (ep_out ? 1 : 0);
    buf[pos + 5] = cls; buf[pos + 6] = sub; buf[pos + 7] = pro;
    pos += 9;
    if (ep_in) {
        buf[pos + 0] = 7; buf[pos + 1] = kIOUSBDescriptorTypeEndpoint; buf[pos + 2] = ep_in; buf[pos + 3] = kIOUSBEndpointTypeInterrupt;
        buf[pos + 4] = 64; buf[pos + 5] = 0; pos += 7;
    }
    if (ep_out) {
        buf[pos + 0] = 7; buf[pos + 1] = kIOUSBDescriptorTypeEndpoint; buf[pos + 2] = ep_out; buf[pos + 3] = kIOUSBEndpointTypeInterrupt;
        buf[pos + 4] = 64; buf[pos + 5] = 0; pos += 7;
    }
    buf[2] = (uint8_t)(pos & 0xff); buf[3] = (uint8_t)(pos >> 8);
    return [NSData dataWithBytes:buf length:pos];
}

static TestUSBNode *make_usb_node(const char *name, int vid, int pid, int dc, int ds, int dp, NSData *cfg, NSUInteger iface_off, int if_cls, int if_sub, int if_pro, BOOL add_if) {
    TestUSBNode *dev = [TestUSBNode new];
    dev.entry = g_next_entry++;
    dev.serviceClass = @"IOUSBHostDevice";
    dev.configDesc = cfg;
    dev.props[@"USB Product Name"] = @(name);
    dev.props[@"idVendor"] = @(vid);
    dev.props[@"idProduct"] = @(pid);
    dev.props[@"bDeviceClass"] = @(dc);
    dev.props[@"bDeviceSubClass"] = @(ds);
    dev.props[@"bDeviceProtocol"] = @(dp);
    if (add_if) {
        TestUSBNode *ifc = [TestUSBNode new];
        ifc.entry = g_next_entry++;
        ifc.serviceClass = @"IOUSBHostInterface";
        ifc.configDesc = cfg;
        ifc.props[@"_ifaceOffset"] = @(iface_off);
        ifc.props[@"bInterfaceClass"] = @(if_cls);
        ifc.props[@"bInterfaceSubClass"] = @(if_sub);
        ifc.props[@"bInterfaceProtocol"] = @(if_pro);
        [dev.children addObject:ifc];
    }
    return dev;
}

static void reset_env(void) {
    while (g_gippads) gip_remove(g_gippads);
    flush_async();
    [g_registry removeAllObjects];
    [g_watches removeAllObjects];
    ntimers = 0;
    memset(&g_slots, 0, sizeof g_slots);
    memset(g_player_btn, 0, sizeof g_player_btn);
    memset(g_player_axes, 0, sizeof g_player_axes);
    g_players_mask = 0;
    g_press_to_join = g_merge = 0;
    g_cp_polls = g_reads = g_attaches = g_detaches = 0;
    g_gip_port = NULL; g_gip_iter = 0; g_xusb_iter = 0;
    unsetenv("DM_XUSB"); unsetenv("DM_GIP_PADS"); unsetenv("DM_XUSB_LOG"); unsetenv("DM_GIP_LOG");
}

static void fire_hotplug(TestUSBNode *node, int dc, int ds, int dp) {
    [g_registry addObject:node];
    for (NSDictionary *w in g_watches) {
        NSDictionary *pm = w[@"props"];
        if ([pm[@"bDeviceClass"] intValue] == dc && [pm[@"bDeviceSubClass"] intValue] == ds && [pm[@"bDeviceProtocol"] intValue] == dp) {
            IOServiceMatchingCallback cb = [w[@"cb"] pointerValue];
            void *ref = [w[@"ref"] pointerValue];
            TestIOIter *it = [TestIOIter new]; it.items = @[node];
            io_iterator_t handle = io_retain_obj(it);
            cb(ref, handle);
            io_release_handle(handle);
        }
    }
}

static TestUSBPipe *pad_in_pipe(gip_pad *d) { return (__bridge TestUSBPipe *)d->in; }
static TestUSBPipe *pad_out_pipe(gip_pad *d) { return (__bridge TestUSBPipe *)d->out; }
static TestUSBDevice *pad_dev(gip_pad *d) { return (__bridge TestUSBDevice *)d->dev; }

int main(void) {
    g_registry = [NSMutableArray new];
    g_watches = [NSMutableArray new];

    // 1. XUSB + GIP attached together; claimed Microsoft pad & ff/5d/81 wireless receiver skipped
    reset_env();
    NSUInteger xoff = 0, goff = 0, roff = 0;
    NSData *xcfg = make_config(1, 0xff, 0x5d, 0x01, 0x81, 0, &xoff);
    NSData *gcfg = make_config(1, 0xff, 0x47, 0xd0, 0x81, 0x01, &goff);
    NSData *rcfg = make_config(1, 0xff, 0x5d, 0x81, 0x81, 0, &roff);

    TestUSBNode *xnode = make_usb_node("PDP Xbox 360", 0x0e6f, 0x0301, 0xff, 0xff, 0xff, xcfg, xoff, 0xff, 0x5d, 0x01, YES);
    TestUSBNode *gnode = make_usb_node("Turtle Beach REACT-R", 0x10f5, 0x7024, 0xff, 0x47, 0xd0, gcfg, goff, 0xff, 0x47, 0xd0, YES);
    TestUSBNode *ms360 = make_usb_node("Microsoft Xbox 360", 0x045e, 0x028e, 0xff, 0xff, 0xff, xcfg, xoff, 0xff, 0x5d, 0x01, YES);
    TestUSBNode *drv = [TestUSBNode new]; drv.serviceClass = @"XboxUSBDevice"; [ms360.children addObject:drv];
    TestUSBNode *wrecv = make_usb_node("Xbox 360 Wireless Receiver", 0x045e, 0x0719, 0xff, 0xff, 0xff, rcfg, roff, 0xff, 0x5d, 0x81, YES);
    // Unconfigured wireless receiver (no children yet): configuration descriptor check must reject before configuring
    TestUSBNode *wrecv_unconf = make_usb_node("Unconfigured Receiver", 0x045e, 0x0719, 0xff, 0xff, 0xff, rcfg, roff, 0xff, 0x5d, 0x81, NO);

    [g_registry addObjectsFromArray:@[xnode, gnode, ms360, wrecv, wrecv_unconf]];
    gippad_start();
    advance_timers((uint64_t)(GIPPAD_WAIT * NSEC_PER_SEC) + 100);

    CHECK(g_attaches == 2 && g_players_mask == 0x3);
    CHECK(!ms360.hostDevice && !wrecv.hostDevice && !wrecv_unconf.hostDevice);   // claimed & non-pad receivers skipped
    // g_gippads is prepended: GIP watched first, then XUSB watched second, so XUSB is at head and GIP is next
    gip_pad *xpad = g_gippads, *gpad = g_gippads ? g_gippads->next : NULL;
    CHECK(gpad && xpad && gpad->xusb == 0 && xpad->xusb == 1);
    if (!gpad || !xpad) return 1;
    CHECK(pad_slot_of(&g_slots, gpad) == 0 && pad_slot_of(&g_slots, xpad) == 1);
    CHECK(pad_in_pipe(xpad) != nil && pad_out_pipe(xpad) == nil);
    CHECK(pad_in_pipe(gpad) != nil && pad_out_pipe(gpad).pendingHandlers.count == 1);   // GIP power-on sent, XUSB sends none

    // XUSB input report + dead-zone jitter deduplication (XUSB is player 1 / slot 1)
    uint8_t xr[20] = {0, 20, 0, 0x20, 0, 0, 102, 0, -443 & 0xff, (-443 >> 8) & 0xff};   // Xbox B -> Switch A
    [pad_in_pipe(xpad) completeNext:kIOReturnSuccess bytes:xr length:sizeof xr];
    CHECK(g_reads == 1 && g_player_btn[1] == DM_BTN_A);
    xr[6] = 103;   // 1-LSB change inside dead zone, buttons unchanged
    [pad_in_pipe(xpad) completeNext:kIOReturnSuccess bytes:xr length:sizeof xr];
    CHECK(g_reads == 1);   // deduplicated

    // 2. Unplug race A: read completion error fires BEFORE queued kIOMessageServiceIsTerminated
    TestUSBDevice *xdev = pad_dev(xpad);
    TestUSBPipe *xin = pad_in_pipe(xpad);
    xdev.interestHandler(xdev, kIOMessageServiceIsTerminated, NULL);   // queues async termination block
    [xin completeNext:kIOReturnAborted bytes:NULL length:0];          // read error drops first
    flush_async();                                                    // queued termination block runs safely (no UAF/double detach)
    CHECK(g_detaches == 1 && g_players_mask == 0x1 && g_player_btn[1] == 0);

    // 3. Unplug race B: kIOMessageServiceIsTerminated fires FIRST while read + out write are still pending
    TestUSBDevice *gdev = pad_dev(gpad);
    TestUSBPipe *gin = pad_in_pipe(gpad), *gout = pad_out_pipe(gpad);
    [gout completeNext:kIOReturnSuccess bytes:NULL length:0];
    // Queue another write so pending == 2 (1 read + 1 write) when termination fires
    const uint8_t vkey[6] = {GIP_CMD_VIRTUAL_KEY, GIP_OPT_ACK | GIP_OPT_INTERNAL, 7, 2, 1, 0};
    [gin completeNext:kIOReturnSuccess bytes:vkey length:sizeof vkey];
    CHECK(g_player_btn[0] == 0 && gpad->st.guide == 1 && gpad->pending == 2);
    gdev.interestHandler(gdev, kIOMessageServiceIsTerminated, NULL);
    flush_async();   // gip_remove detaches immediately and cancels pipes (draining pending to 0)
    CHECK(g_detaches == 2 && g_players_mask == 0 && g_cp_polls >= 4);

    // 4. Reconnect slot affinity + DM_PAD_PRESS_TO_JOIN=1 unjoined detach
    TestUSBNode *xnode2 = make_usb_node("PDP Xbox 360", 0x0e6f, 0x0301, 0xff, 0xff, 0xff, xcfg, xoff, 0xff, 0x5d, 0x01, YES);
    fire_hotplug(xnode2, 0xff, 0xff, 0xff);
    advance_timers((uint64_t)(GIPPAD_WAIT * NSEC_PER_SEC) + 100);
    CHECK(g_gippads && pad_slot_of(&g_slots, g_gippads) == 1);   // reconnected PDP gets slot 1 back

    // Unjoinable / DM_PAD_PRESS_TO_JOIN=1 test: pad connects unjoined, unplugs unjoined (cp_poll still called), reconnects and joins on first button
    g_press_to_join = 1;
    TestUSBNode *p3 = make_usb_node("Third Pad", 0x0e6f, 0x0302, 0xff, 0xff, 0xff, xcfg, xoff, 0xff, 0x5d, 0x01, YES);
    int polls_before = g_cp_polls;
    fire_hotplug(p3, 0xff, 0xff, 0xff);
    advance_timers((uint64_t)(GIPPAD_WAIT * NSEC_PER_SEC) + 100);
    gip_pad *pad3 = g_gippads;
    CHECK(pad3 && pad_slot_of(&g_slots, pad3) == -1 && g_cp_polls == polls_before + 1);
    pad_dev(pad3).interestHandler(pad_dev(pad3), kIOMessageServiceIsTerminated, NULL);
    flush_async();
    CHECK(g_cp_polls == polls_before + 2 && g_players_mask == 0x2);   // cp_poll notified on unjoined unplug

    TestUSBNode *p3b = make_usb_node("Third Pad", 0x0e6f, 0x0302, 0xff, 0xff, 0xff, xcfg, xoff, 0xff, 0x5d, 0x01, YES);
    fire_hotplug(p3b, 0xff, 0xff, 0xff);
    advance_timers((uint64_t)(GIPPAD_WAIT * NSEC_PER_SEC) + 100);
    pad3 = g_gippads;
    CHECK(pad_slot_of(&g_slots, pad3) == -1);
    uint8_t xr_press[20] = {0, 20, 0, 0x10};   // Xbox A -> Switch B
    [pad_in_pipe(pad3) completeNext:kIOReturnSuccess bytes:xr_press length:sizeof xr_press];
    CHECK(pad_slot_of(&g_slots, pad3) == 0 && g_player_btn[0] == DM_BTN_B);   // took lowest free slot 0 on first press

    // 5. DM_XUSB=0 disables XUSB watch while keeping GIP active
    reset_env();
    setenv("DM_XUSB", "0", 1);
    [g_registry addObjectsFromArray:@[xnode, gnode]];
    gippad_start();
    advance_timers((uint64_t)(GIPPAD_WAIT * NSEC_PER_SEC) + 100);
    CHECK(g_attaches == 1 && g_gippads && !g_gippads->xusb && !g_gippads->next);

    // 6. DM_GIP_PADS=0 disables GIP watch while keeping XUSB active
    reset_env();
    setenv("DM_GIP_PADS", "0", 1);
    [g_registry addObjectsFromArray:@[xnode, gnode]];
    gippad_start();
    advance_timers((uint64_t)(GIPPAD_WAIT * NSEC_PER_SEC) + 100);
    CHECK(g_attaches == 1 && g_gippads && g_gippads->xusb && !g_gippads->next);

    reset_env();
    printf("t_pad_gip_mac: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
