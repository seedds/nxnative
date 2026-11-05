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
        return started && getenv("NX_CONTROLLER_TEST_START") && !strcmp(getenv("NX_CONTROLLER_TEST_START"), "from-window") ? 0 : 1;
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
    assert(cs_player((id)a, &style) == 1 && style == DM_STYLE_LEFT);
    assert(cs_player((id)b, &style) == 0 && style == DM_STYLE_RIGHT);
    NSDictionary *duplicate = @{@"id": bid, @"player": @2, @"style": @"right"};
    assert(cs_accept(@{@"choices": @[left, duplicate], @"environment": @{@"NX_CONTROLLER_TEST_INVALID": @"not-applied"}, @"explicit": @YES}));
    assert(!getenv("NX_CONTROLLER_TEST_INVALID") && cs_player((id)b, &style) == 0);
    assert(cs_accept(@{@"choices": @[left], @"environment": @{@"bad=key": @"value"}, @"explicit": @YES}));
    cs_update((id)@[b, replacement]);
    assert(cs_player((id)replacement, &style) == -2);
    assert(!cs_identify(aid, signal_device) && signaled == (id)b);
    assert(cs_accept(@{@"choices": @[left], @"environment": @{}, @"explicit": @YES}));
    assert(cs_accept(@{@"choices": @[@{@"id": bid, @"player": @9, @"style": @"pro"}], @"environment": @{}, @"explicit": @YES}));
    assert(cs_accept(@{@"choices": @[@{@"id": bid, @"player": @1, @"style": @"unknown"}], @"environment": @{}, @"explicit": @YES}));
    assert(!cs_accept(@{@"choices": @[@{@"id": bid, @"player": @0, @"style": @"right"}], @"environment": @{}, @"explicit": @YES}));
    assert(cs_player((id)b, &style) == -1);
    puts("controller registry: OK");
    return 0;
} }
