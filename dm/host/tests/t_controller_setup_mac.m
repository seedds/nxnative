// Device identity and validation in the real engine registry, with same-named fake devices.
#import "../controller_setup_mac.h"
#include <assert.h>

@interface SetupDevice : NSObject
@property NSString *vendorName;
@property NSObject *extendedGamepad;
@end
@implementation SetupDevice
@end

static GCController *signaled;
static BOOL signal_device(GCController *device) { signaled = device; return YES; }

int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 3 && !strcmp(argv[1], "--server")) {
        int started = cs_run(atoi(argv[2]), NULL);
        const char *value=getenv("NX_CONTROLLER_TEST_START");
        return started && value && !strcmp(value, "from-window") ? 0 : 1;
    }
    SetupDevice *a = [SetupDevice new], *b = [SetupDevice new], *replacement = [SetupDevice new];
    for (SetupDevice *d in @[a, b, replacement]) { d.vendorName = @"DualSense"; d.extendedGamepad = [NSObject new]; }
    NSArray *rows = cs_update((id)@[a, b]);
    NSString *aid = rows[0][@"id"], *bid = rows[1][@"id"];
    assert(![aid isEqual:bid]);
    NSArray *reordered = cs_update((id)@[b, a]);
    assert([reordered[1][@"id"] isEqual:aid]);
    assert(cs_identify(bid, signal_device) && signaled == (id)b);
    NSDictionary *left = @{@"id": aid, @"player": @2, @"style": @"left"};
    NSDictionary *right = @{@"id": bid, @"player": @1, @"style": @"right"};
    assert(!cs_accept(@{@"choices": @[left, right], @"environment": @{}, @"explicit": @YES}));
    uint32_t style;
    assert(cs_source_player(nil,&style)==-2 && !style && cs_device_role(nil,@{})==0);
    cs_admit(nil); cs_admit(replacement); cs_press_to_join=YES;
    assert(!cs_waits_for_press(nil,nil) && !cs_waits_for_press(replacement,nil) && !cs_admitted.count);
    cs_press_to_join=NO;
    assert(cs_player((id)a, &style) == 1 && style == DM_STYLE_LEFT);
    assert(cs_player((id)b, &style) == 0 && style == DM_STYLE_RIGHT);
    NSDictionary *duplicate = @{@"id": bid, @"player": @2, @"style": @"right"};
    assert(cs_accept(@{@"choices": @[left, duplicate], @"environment": @{@"NX_CONTROLLER_TEST_INVALID": @"not-applied"}, @"explicit": @YES}));
    assert(!getenv("NX_CONTROLLER_TEST_INVALID") && cs_player((id)b, &style) == 0);
    assert(cs_accept(@{@"choices": @[left], @"environment": @{@"bad=key": @"value"}, @"explicit": @YES}));
    cs_update((id)@[b, replacement]);
    cs_press_to_join=YES; assert(!cs_waits_for_press(a,nil)); cs_press_to_join=NO;
    assert(cs_player((id)replacement, &style) == -2);
    assert(!cs_identify(aid, signal_device) && signaled == (id)b);
    assert(cs_accept(@{@"choices": @[left], @"environment": @{}, @"explicit": @YES}));
    assert(cs_accept(@{@"choices": @[@{@"id": bid, @"player": @9, @"style": @"pro"}], @"environment": @{}, @"explicit": @YES}));
    assert(cs_accept(@{@"choices": @[@{@"id": bid, @"player": @1, @"style": @"unknown"}], @"environment": @{}, @"explicit": @YES}));
    assert(!cs_accept(@{@"choices": @[@{@"id": bid, @"player": @0, @"style": @"right"}], @"environment": @{}, @"explicit": @YES}));
    assert(cs_player((id)b, &style) == -1);
    // Transport owners need no GC profile. Metadata is display-only and the
    // connection object remains the identity through reordering and handoff.
    NSObject *rawLeft=[NSObject new], *rawRight=[NSObject new];
    NSArray *rawSources=@[@{@"owner":rawLeft,@"name":@"Joy-Con (L)",@"physicalSide":@"left"},@{@"owner":rawRight,@"name":@"Joy-Con (R)",@"physicalSide":@"right"}];
    NSArray *rawRows=cs_update_sources(rawSources);
    NSString *rawL=rawRows[0][@"id"], *rawR=rawRows[1][@"id"];
    assert(!rawRows[0][@"owner"] && [rawRows[0][@"physicalSide"] isEqual:@"left"]);
    NSArray *rawReordered=cs_update_sources(@[rawSources[1],rawSources[0]]);
    assert([rawReordered[0][@"id"] isEqual:rawR] && [rawReordered[1][@"id"] isEqual:rawL]);
    NSArray *paired=@[@{@"id":rawL,@"player":@1,@"style":@"pair",@"role":@"left"},@{@"id":rawR,@"player":@1,@"style":@"pair",@"role":@"right"}];
    assert(!cs_accept(@{@"choices":paired,@"environment":@{},@"explicit":@YES}));
    assert(cs_source_player(rawLeft,&style)==0 && style==DM_STYLE_PAIR);
    assert(cs_accept(@{@"choices":@[@{@"id":rawL,@"player":@1,@"style":@"pair",@"role":@"right"}],@"environment":@{},@"explicit":@YES}));
    cs_update_sources(@[rawSources[0]]); assert(!cs_devices[rawR] && cs_devices[rawL]==rawLeft);
    puts("controller registry: OK");
    return 0;
} }
