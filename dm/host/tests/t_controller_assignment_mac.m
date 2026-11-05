// Run the production swap adapter with fake native devices. The real HID barrier,
// source generations and guest reads are independently exercised by t_hid.
#import "../controller_assignment_apple.h"
#import "../controller_routes_ios.h"
#include <assert.h>

@interface SwapMotion : NSObject
@property BOOL hasRotationRate, hasGravityAndUserAcceleration, sensorsRequireManualActivation, sensorsActive;
@property GCAcceleration acceleration, gravity, userAcceleration;
@property GCRotationRate rotationRate;
@property(copy) GCMotionValueChangedHandler valueChangedHandler;
@end
@implementation SwapMotion
@end
@interface SwapPad : NSObject
@property(copy) GCExtendedGamepadValueChangedHandler valueChangedHandler;
@end
@implementation SwapPad
@end
@interface SwapDevice : NSObject
@property NSString *vendorName, *productCategory;
@property SwapPad *extendedGamepad;
@property SwapMotion *motion;
@end
@implementation SwapDevice
@end

static NSArray *devices;
static uint32_t configured[DM_PLAYERS], supported = 0x1f, supportedIDs = 0x3ff, connected, reset;
static uint64_t generations[DM_PLAYERS];
static int depth, published, samples[DM_PLAYERS], keepCalls, readCalls;
static GCController *routed[DM_PLAYERS];
static NSMutableArray *routing;
static NSArray<GCController *> *getDevices(void) { return (id)devices; }
static int mode(void) { return 1; }
static void readPad(GCExtendedGamepad *pad) { (void)pad; readCalls++; }
static void keep(GCController *device) { (void)device; keepCalls++; }
static void changed(GCController *device) { (void)device; assert(depth == 1); published++; }
void dm_pad_supported(uint32_t *styles, uint32_t *ids) { *styles = supported; *ids = supportedIDs; }
uint32_t dm_pad_configured_style(int player) { return configured[player]; }
void dm_pad_reconfigure_begin(void) { assert(!depth); depth++; }
void dm_pad_reconfigure_reset(uint32_t mask) { assert(depth == 1); reset = mask; for (int p = 0; p < DM_PLAYERS; p++) if (mask >> p & 1) generations[p]++; }
void dm_pad_reconfigure_end(void) { assert(depth == 1); depth--; }
static int follows_hold[DM_PLAYERS];
void dm_pad_style(int player, uint32_t style, int follow_hold) { assert(depth == 1); configured[player] = style; follows_hold[player] = follow_hold; }
void dm_pad_explicit(int enabled) { assert(depth == 1 && enabled); }
void dm_pad_players(uint32_t mask) { assert(depth == 1); connected = mask; }
uint64_t dm_pad_generation(int player) { return generations[player]; }
void dm_motion_set_generation(int player, uint64_t generation, const float a[3], const float r[3]) { (void)a; (void)r; if (generation == generations[player]) samples[player]++; }
void dm_motion_clear(int player) { (void)player; assert(depth == 1); }
void dm_haptics_apple_controller(int player, GCController *device) {
    assert(depth == 1); routed[player] = device;
    [routing addObject:[NSString stringWithFormat:@"%d:%@", player, device ? @"on" : @"off"]];
}
static SwapDevice *device(void) {
    SwapDevice *d = [SwapDevice new]; d.vendorName = @"DualSense"; d.extendedGamepad = [SwapPad new]; d.motion = [SwapMotion new];
    d.motion.hasRotationRate = YES; d.motion.sensorsRequireManualActivation = YES; d.motion.acceleration = (GCAcceleration){0, 0, -1};
    return d;
}
static NSArray *ios_sources(void) {
    NSMutableArray *sources = [NSMutableArray new];
    for (int p = 0; p < DM_PLAYERS; p++) [sources addObject:[NSMutableArray new]];
    return sources;
}

// Compatibility oracle copied from the original pads_slotted flow: disconnect
// owners first, then let each newly connected pad take its remembered slot.
static void original_ios_slots(pad_slots *slots, NSArray *connected) {
    for (int p = 0; p < DM_PLAYERS; p++) {
        const void *owner = slots->owner[p];
        if (owner && ![connected containsObject:(__bridge id)owner]) pad_slot_free(slots, owner);
    }
    for (SwapDevice *controller in connected) {
        const void *owner = (__bridge const void *)controller;
        if (pad_slot_of(slots, owner) < 0) pad_slot_take(slots, owner, controller.vendorName.UTF8String);
    }
}

static void test_ios_automatic_compatibility(void) {
    NSMutableArray *pool = [NSMutableArray new];
    for (int i = 0; i < 10; i++) {
        SwapDevice *controller = device();
        controller.vendorName = [NSString stringWithFormat:@"Controller family %d", i % 3];
        [pool addObject:controller];
    }
    for (int merge = 0; merge <= 1; merge++) {
        pad_slots original = {0};
        dm_ios_routes previous = {0};
        NSMutableArray *connected = [NSMutableArray new];
        uint32_t random = 0x72a10f3;
        for (int step = 0; step < 1000; step++) {
            random = random * 1664525u + 1013904223u;
            SwapDevice *controller = pool[(random >> 8) % pool.count];
            if (step == 0) [connected addObjectsFromArray:pool];  // exercise the eight-player limit
            else if ([connected containsObject:controller]) [connected removeObjectIdenticalTo:controller];
            else [connected insertObject:controller atIndex:(random >> 16) % (connected.count + 1)];

            if (!merge) original_ios_slots(&original, connected);
            NSArray *rows = cs_update(connected);
            NSMutableArray *draft = [NSMutableArray new];
            for (NSDictionary *row in rows)
                [draft addObject:@{@"id": row[@"id"], @"player": @(-1), @"style": @"auto"}];
            NSDictionary *overrides = nil;
            assert(!ios_validate_overrides(draft, &overrides) && !overrides.count);
            NSArray *sources = ios_sources();
            dm_ios_routes next = ios_plan_routes(previous.slots, rows, overrides, merge, sources);
            assert(!next.manual);
            for (int p = 0; p < DM_PLAYERS; p++) {
                assert(next.styles[p] == DM_STYLE_AUTO);
                if (merge) {
                    NSArray *expected = p == 0 ? connected : @[];
                    assert([sources[p] isEqualToArray:expected]);
                } else {
                    assert(next.slots.owner[p] == original.owner[p]);
                    assert(!strcmp(next.slots.last[p], original.last[p]));
                }
            }
            // Opening the menu, previewing, or submitting unchanged defaults
            // must not change slots or introduce overrides.
            NSArray *preview = ios_sources();
            dm_ios_routes same = ios_plan_routes(next.slots, rows, overrides, merge, preview);
            assert(!same.manual);
            for (int p = 0; p < DM_PLAYERS; p++)
                assert(same.slots.owner[p] == next.slots.owner[p] && [preview[p] isEqualToArray:sources[p]]);
            previous = next;
        }
    }
    puts("iOS compatibility: 2000 connection transitions and unchanged-menu drafts match original routing OK");
}

static void test_ios_routes(SwapDevice *a, SwapDevice *b, SwapDevice *c) {
    NSArray *rows = cs_update((id)@[a, b]);
    NSString *aid = rows[0][@"id"], *bid = rows[1][@"id"];
    NSDictionary *automatic = @{@"id": aid, @"player": @(-1), @"style": @"auto"};
    NSDictionary *overrides = nil;
    assert(!ios_validate_overrides(@[automatic], &overrides) && overrides.count == 0);
    NSArray *sources = ios_sources();
    dm_ios_routes initial = ios_plan_routes((pad_slots){0}, rows, overrides, NO, sources);
    assert(!initial.manual && [sources[0] firstObject] == a && [sources[1] firstObject] == b);

    NSDictionary *manual = @{@"id": aid, @"player": @2, @"style": @"left"};
    assert(!ios_validate_overrides(@[manual], &overrides));
    sources = ios_sources();
    dm_ios_routes swapped = ios_plan_routes(initial.slots, rows, overrides, NO, sources);
    assert(swapped.manual == 2 && swapped.styles[1] == DM_STYLE_LEFT);
    assert([sources[0] firstObject] == b && [sources[1] firstObject] == a);

    rows = cs_update((id)@[b, a, c]);
    sources = ios_sources();
    dm_ios_routes joined = ios_plan_routes(swapped.slots, rows, overrides, NO, sources);
    assert(joined.manual == 2 && [sources[0] firstObject] == b && [sources[2] firstObject] == c);

    // The legacy merged group stays together while a separate manual slot is used.
    sources = ios_sources();
    dm_ios_routes merged = ios_plan_routes(joined.slots, rows, overrides, YES, sources);
    assert((merged.manual == 2 && [sources[0] isEqualToArray:@[b, c]] && [sources[1] firstObject] == a));

    rows = cs_update((id)@[b, c]);
    sources = ios_sources();
    dm_ios_routes disconnected = ios_plan_routes(joined.slots, rows, overrides, NO, sources);
    assert(!disconnected.manual && ![sources[1] count] && [sources[2] firstObject] == c);
    rows = cs_update((id)@[b, c, a]);
    sources = ios_sources();
    dm_ios_routes reconnected = ios_plan_routes(disconnected.slots, rows, overrides, NO, sources);
    assert(!reconnected.manual && reconnected.styles[1] == DM_STYLE_AUTO && [sources[1] firstObject] == a);

    NSDictionary *disabled = @{@"id": rows[2][@"id"], @"player": @0, @"style": @"auto"};
    NSDictionary *disabledB = @{@"id": bid, @"player": @0, @"style": @"auto"};
    rows = cs_update((id)@[a, b]);
    assert(!ios_validate_overrides(@[disabled, disabledB], &overrides));
    sources = ios_sources();
    dm_ios_routes touch = ios_plan_routes(merged.slots, rows, overrides, NO, sources);
    assert(!pad_slot_mask(&touch.slots) && !touch.manual);
    assert([controller_recommended_style(DM_STYLE_LEFT | DM_STYLE_RIGHT, 0) isEqual:@"left"]);
    assert([controller_recommended_style(DM_STYLE_LEFT | DM_STYLE_RIGHT, 1) isEqual:@"right"]);
    assert([controller_recommended_style(DM_STYLE_PRO | DM_STYLE_LEFT, 0) isEqual:@"pro"]);
    assert([controller_recommended_style(DM_STYLE_PAIR, 0) isEqual:@"pair"]);
    assert(!controller_recommended_style(2u, 0));   // handheld-only games have no external recommendation
    puts("iOS routes: automatic, per-device override, hotplug, merged defaults and unassign OK");
}

int main(void) { @autoreleasepool {
    SwapDevice *a = device(), *b = device(), *c = device(); devices = @[a, b, c]; routing = [NSMutableArray new];
    pad_slots slots = {0}; GCController *held[DM_PLAYERS] = {nil}; int merge = 0, own = 0;
    dm_controller_host host = {&slots, held, &merge, &own, getDevices, mode, readPad, keep, changed};
    dm_motion_attach((id)a, 0);
    GCMotionValueChangedHandler legacyA = a.motion.valueChangedHandler;
    a.motion.valueChangedHandler = nil; a.motion.sensorsActive = NO;
    uint32_t affected = 0;
    assert(!controller_assignment_apply(host, @[], 1, 8, &affected) && affected == 0);
    legacyA((id)a.motion); assert(samples[0] == 1); samples[0] = 0;   // no old controller motion may drive the keyboard fallback
    NSArray *rows = controller_assignment_state(host)[@"controllers"];
    NSString *aid = rows[0][@"id"], *bid = rows[1][@"id"], *cid = rows[2][@"id"];
    NSArray *initial = @[@{@"id": aid, @"player": @1, @"style": @"pro"}, @{@"id": bid, @"player": @2, @"style": @"pro"}, @{@"id": cid, @"player": @3, @"style": @"pro"}];
    assert(!controller_assignment_apply(host, initial, 1, 8, &affected) && affected == 7 && !depth);
    assert(held[0] == (id)a && held[1] == (id)b && held[2] == (id)c && connected == 7);
    assert(a.motion.sensorsActive && b.motion.sensorsActive && c.motion.sensorsActive && keepCalls == 3);
    b.productCategory = @"Nintendo Switch Joy-Con (R)";
    GCMotionValueChangedHandler oldA = a.motion.valueChangedHandler, stableC = c.motion.valueChangedHandler;
    uint64_t stableGeneration = generations[2];
    [routing removeAllObjects];
    NSArray *swapped = @[@{@"id": aid, @"player": @2, @"style": @"left"}, @{@"id": bid, @"player": @1, @"style": @"right"}, initial[2]];
    assert(!controller_assignment_apply(host, swapped, 1, 8, &affected) && affected == 3 && reset == 3);
    assert(held[0] == (id)b && held[1] == (id)a && held[2] == (id)c);
    assert(pad_slot_of(&slots, (__bridge const void *)a) == 1 && pad_slot_of(&slots, (__bridge const void *)b) == 0);
    assert(configured[0] == DM_STYLE_RIGHT && configured[1] == DM_STYLE_LEFT && routed[0] == (id)b && routed[1] == (id)a);
    assert(!follows_hold[0] && follows_hold[1]);   // the real Joy-Con and full-size pad took opposite slots
    assert(([routing isEqual:@[@"0:off", @"1:off", @"0:on", @"1:on"]]));
    assert(generations[2] == stableGeneration && c.motion.valueChangedHandler == stableC);
    oldA((id)a.motion); assert(samples[0] == 0);   // the old block still names player 1, but its generation is stale
    a.motion.valueChangedHandler((id)a.motion); b.motion.valueChangedHandler((id)b.motion);
    assert(samples[1] == 1 && samples[0] == 1);
    a.extendedGamepad.valueChangedHandler((id)a.extendedGamepad, (id)a.extendedGamepad); assert(readCalls == 1);
    [routing removeAllObjects];
    assert(!controller_assignment_apply(host, swapped, 1, 8, &affected) && !affected && !routing.count && generations[2] == stableGeneration);
    int before = published;
    assert(controller_assignment_apply(host, @[swapped[0], @{@"id": bid, @"player": @2, @"style": @"right"}], 0, 8, &affected));
    assert(published == before && !depth && !routing.count && held[0] == (id)b);
    assert(controller_assignment_apply(host, swapped, 4, 8, &affected) && !depth && published == before && !routing.count);
    supported = DM_STYLE_PAIR; assert(controller_assignment_apply(host, swapped, 1, 8, &affected) && published == before); supported = 0x1f;
    devices = @[a, c]; assert(controller_assignment_apply(host, swapped, 1, 8, &affected) && published == before && !routing.count);
    NSArray *withoutB = @[swapped[0], initial[2]];
    assert(!controller_assignment_apply(host, withoutB, 1, 8, &affected) && affected == 1 && held[0] == nil && held[1] == (id)a && connected == 6);
    NSDictionary *state = controller_assignment_state(host); assert([state[@"controllers"] count] == 2);
    static const char keyboard = 0;
    slots.owner[0] = &keyboard; own = 1;
    NSArray *automaticChoices = @[@{@"id": aid, @"player": @(-1), @"style": @"auto"},
                                  @{@"id": cid, @"player": @(-1), @"style": @"auto"}];
    NSArray *preview = controller_assignment_resolve(host, automaticChoices);
    assert([preview[0][@"player"] intValue] != 1 && [preview[1][@"player"] intValue] != 1);
    assert(!controller_assignment_apply(host, automaticChoices, 1, 8, &affected));
    assert(slots.owner[0] == &keyboard && own == 1);
    puts("controller assignments: identity, live swaps, stale motion, haptics order, unchanged owners and validation OK");
    test_ios_routes(a, b, c);
    test_ios_automatic_compatibility();
    return 0;
} }
