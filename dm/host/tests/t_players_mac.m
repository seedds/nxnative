// Windowless routing tests: duck-typed GameController objects, real router and output callbacks.
#import "../players_mac.h"
#include <assert.h>
#include <stdio.h>

@interface TestButton : NSObject
@property(nonatomic) BOOL isPressed;
@property(nonatomic, copy) void (^pressedChangedHandler)(GCControllerButtonInput *, float, BOOL);
@end
@implementation TestButton
@end
@interface TestAxis : NSObject
@property(nonatomic) float value;
@end
@implementation TestAxis
@end
@interface TestStick : NSObject
@property(nonatomic, strong) TestAxis *xAxis, *yAxis;
@property(nonatomic, strong) TestButton *left, *up, *right, *down;
@property(nonatomic, copy) void (^valueChangedHandler)(GCControllerDirectionPad *, float, float);
@end
@implementation TestStick
- (instancetype)init {
    if ((self = [super init])) {
        _xAxis = [TestAxis new]; _yAxis = [TestAxis new];
        _left = [TestButton new]; _up = [TestButton new]; _right = [TestButton new]; _down = [TestButton new];
    }
    return self;
}
@end
@class TestController;
@interface TestPad : NSObject
@property(nonatomic, weak) TestController *controller;
@property(nonatomic, strong) TestButton *buttonA, *buttonB, *buttonX, *buttonY;
@property(nonatomic, strong) TestButton *leftShoulder, *rightShoulder, *leftTrigger, *rightTrigger;
@property(nonatomic, strong) TestButton *buttonMenu, *buttonOptions, *leftThumbstickButton, *rightThumbstickButton;
@property(nonatomic, strong) TestStick *dpad, *leftThumbstick, *rightThumbstick;
@property(nonatomic, copy) GCExtendedGamepadValueChangedHandler valueChangedHandler;
@end
@implementation TestPad
- (instancetype)init {
    if ((self = [super init])) {
        _buttonA = [TestButton new]; _buttonB = [TestButton new]; _buttonX = [TestButton new]; _buttonY = [TestButton new];
        _leftShoulder = [TestButton new]; _rightShoulder = [TestButton new];
        _leftTrigger = [TestButton new]; _rightTrigger = [TestButton new];
        _buttonMenu = [TestButton new]; _buttonOptions = [TestButton new];
        _leftThumbstickButton = [TestButton new]; _rightThumbstickButton = [TestButton new];
        _dpad = [TestStick new]; _leftThumbstick = [TestStick new]; _rightThumbstick = [TestStick new];
    }
    return self;
}
@end
@interface TestPhysicalPad : NSObject
@property(nonatomic, strong) NSDictionary *buttons, *dpads;
@property(nonatomic, copy) void (^valueDidChangeHandler)(GCPhysicalInputProfile *, GCControllerElement *);
@end
@implementation TestPhysicalPad
@end
@interface TestController : NSObject
@property(nonatomic, copy) NSString *productCategory, *vendorName;
@property(nonatomic, strong) dispatch_queue_t handlerQueue;
@property(nonatomic, strong) TestPad *extendedGamepad;
@property(nonatomic, strong) TestPhysicalPad *physicalInputProfile;
@end
@implementation TestController
@end
static TestController *controller(NSString *category) {
    TestController *c = [TestController new]; c.productCategory = category; c.vendorName = @"Identical model";
    c.extendedGamepad = [TestPad new]; c.extendedGamepad.controller = c;
    return c;
}
static GCController *gc(TestController *c) { return (GCController *)(id)c; }
static TestController *standardController(NSString *category) {
    TestController *c = controller(category); c.extendedGamepad = nil;
    TestPhysicalPad *profile = [TestPhysicalPad new];
    profile.buttons = @{GCInputButtonA: [TestButton new], GCInputButtonB: [TestButton new],
        GCInputButtonX: [TestButton new], GCInputButtonY: [TestButton new],
        GCInputLeftShoulder: [TestButton new], GCInputRightShoulder: [TestButton new], GCInputButtonMenu: [TestButton new]};
    profile.dpads = @{GCInputDirectionPad: [TestStick new]}; c.physicalInputProfile = profile;
    return c;
}

@interface Output : NSObject {
@public
    DmPlayerState state[DM_PLAYERS];
    unsigned changes[DM_PLAYERS], maskChanges, routeChanges;
    uint32_t mask, styles[DM_PLAYERS];
}
@end
@implementation Output
@end
static DmMacPlayers *router(Output *o) {
    DmMacPlayers *r = [DmMacPlayers new];
    r.output = ^(int p, DmPlayerState s) { assert(p >= 0 && p < DM_PLAYERS); o->state[p] = s; o->changes[p]++; };
    r.players = ^(uint32_t mask) { o->mask = mask; o->maskChanges++; };
    r.routesChanged = ^{ o->routeChanges++; };
    r.styles = ^(int p, uint32_t style) { o->styles[p] = style; };
    r.routeLog = ^(NSString *line) { (void)line; };
    return r;
}
static void expect(Output *o, int p, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    DmPlayerState s = o->state[p];
    assert(s.buttons == buttons); assert(s.lx == lx); assert(s.ly == ly); assert(s.rx == rx); assert(s.ry == ry);
}
static void zero(Output *o, int p) { expect(o, p, 0, 0, 0, 0, 0); }
static void keyboard(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    r.keyboardPlayer = 3; // UI player 4, not a controller enumeration index.
    assert(o->mask == (1u << 3));
    DmPlayerState held = {DM_BTN_A | DM_BTN_PLUS, 123, -456, 789, -987};
    [r keyboard:held]; expect(o, 3, held.buttons, 123, -456, 789, -987);
    for (int p = 0; p < DM_PLAYERS; p++) if (p != 3) { zero(o, p); assert(o->changes[p] == 0); }
    unsigned n = o->changes[3], m = o->maskChanges;
    [r read]; [r keyboard:held]; assert(o->changes[3] == n && o->maskChanges == m);
    r.active = NO; zero(o, 3); assert(o->mask == (1u << 3));
    r.active = YES; zero(o, 3); // Focus loss must not revive a held keyboard key.
    [r keyboard:held]; r.keyboardPlayer = -1;
    assert(o->mask == 0); zero(o, 3);
    [r keyboard:held]; for (int p = 0; p < DM_PLAYERS; p++) zero(o, p);
    r.keyboardPlayer = 0; zero(o, 0); assert(o->mask == 1);
}
static void handoff(void) {
    DmMacPlayers *r = [DmMacPlayers new]; r.routeLog = ^(NSString *line) { (void)line; };
    r.keyboardPlayer = 3;
    [r keyboard:(DmPlayerState){DM_BTN_A, 12, -34, 56, -78}];
    [r read]; // Setup has already populated the change cache before gameplay attaches.
    Output *o = [Output new];
    for (int p = 0; p < DM_PLAYERS; p++) o->state[p] = (DmPlayerState){DM_BTN_B, 1, 2, 3, 4};
    r.output = ^(int p, DmPlayerState s) { o->state[p] = s; o->changes[p]++; };
    r.players = ^(uint32_t mask) { o->mask = mask; o->maskChanges++; };
    [r publish];
    assert(o->mask == (1u << 3) && o->maskChanges == 1);
    for (int p = 0; p < DM_PLAYERS; p++) {
        assert(o->changes[p] == 1);
        if (p == 3) expect(o, p, DM_BTN_A, 12, -34, 56, -78); else zero(o, p);
    }
    [r read];
    for (int p = 0; p < DM_PLAYERS; p++) assert(o->changes[p] == 1);
    assert(o->maskChanges == 1);
    // Force means recompute, not replay the old cache, including an empty connection mask.
    r.output = nil; r.players = nil; r.keyboardPlayer = -1; [r read];
    r.output = ^(int p, DmPlayerState s) { o->state[p] = s; o->changes[p]++; };
    r.players = ^(uint32_t mask) { o->mask = mask; o->maskChanges++; };
    [r publish]; assert(o->mask == 0 && o->maskChanges == 2);
    for (int p = 0; p < DM_PLAYERS; p++) { zero(o, p); assert(o->changes[p] == 2); }
}
static void full(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    TestController *c = controller(@"Xbox One"); TestPad *g = c.extendedGamepad;
    g.buttonA.isPressed = g.buttonB.isPressed = g.buttonX.isPressed = g.buttonY.isPressed = YES;
    g.leftShoulder.isPressed = g.rightShoulder.isPressed = g.leftTrigger.isPressed = g.rightTrigger.isPressed = YES;
    g.buttonMenu.isPressed = g.buttonOptions.isPressed = g.leftThumbstickButton.isPressed = g.rightThumbstickButton.isPressed = YES;
    g.dpad.left.isPressed = g.dpad.up.isPressed = g.dpad.right.isPressed = g.dpad.down.isPressed = YES;
    g.leftThumbstick.xAxis.value = 2; g.leftThumbstick.yAxis.value = -2;
    g.rightThumbstick.xAxis.value = .5; g.rightThumbstick.yAxis.value = -.5;
    [r sync:@[gc(c)]]; assert(r.sources.count == 1); assert(r.sources[0].player == -1);
    assert(o->mask == 0); zero(o, 0); assert([r wholeControllerForPlayer:0] == nil);
    r.sources[0].player = 0; [r read];
    expect(o, 0, 0xffff, DM_STICK_MAX, -DM_STICK_MAX, 16383, -16383);
    assert(o->mask == 1); assert([r wholeControllerForPlayer:0] == gc(c));
    // Positional mapping for non-Nintendo pads, not just the all-buttons union.
    g.buttonB.isPressed = g.buttonX.isPressed = g.buttonY.isPressed = NO;
    [r read]; assert((o->state[0].buttons & 15) == DM_BTN_B);
    r.active = NO; zero(o, 0); assert(o->mask == 1);
    r.active = YES; assert((o->state[0].buttons & 15) == DM_BTN_B);
    r.sources[0].player = -1; [r read]; zero(o, 0); assert(o->mask == 0);
}
static void split(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = 3;
    TestController *a = controller(@"Nintendo Switch Joy-Con (L/R)");
    TestController *b = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(a), gc(b)]];
    assert(r.sources.count == 2); assert(r.sources[0].controller == gc(a)); assert(r.sources[1].controller == gc(b));
    DmPlayerSource *s = r.sources[0]; s.split = YES; s.player = 1; s.rightPlayer = 2;
    DmPlayerSource *t = r.sources[1]; t.split = YES; t.player = 4; t.rightPlayer = 5;
    TestPad *g = a.extendedGamepad;
    g.dpad.right.isPressed = g.dpad.down.isPressed = g.dpad.up.isPressed = g.dpad.left.isPressed = YES;
    g.leftShoulder.isPressed = g.leftTrigger.isPressed = g.buttonOptions.isPressed = g.leftThumbstickButton.isPressed = YES;
    g.buttonA.isPressed = g.rightShoulder.isPressed = g.rightTrigger.isPressed = g.buttonMenu.isPressed = g.rightThumbstickButton.isPressed = YES;
    g.leftThumbstick.xAxis.value = .5; g.leftThumbstick.yAxis.value = -1;
    g.rightThumbstick.xAxis.value = -1; g.rightThumbstick.yAxis.value = .5;
    b.extendedGamepad.dpad.up.isPressed = YES; b.extendedGamepad.buttonY.isPressed = YES;
    b.extendedGamepad.leftThumbstick.yAxis.value = 1; b.extendedGamepad.rightThumbstick.xAxis.value = 1;
    [r read];
    uint64_t extras = DM_BTN_L | DM_BTN_R | DM_BTN_PLUS | DM_BTN_LS;
    expect(o, 1, 15 | extras, DM_STICK_MAX, 16383, 0, 0);
    expect(o, 2, DM_BTN_B | extras, 16383, DM_STICK_MAX, 0, 0);
    expect(o, 4, DM_BTN_Y, -DM_STICK_MAX, 0, 0, 0);
    expect(o, 5, DM_BTN_X, 0, -DM_STICK_MAX, 0, 0);
    zero(o, 0); zero(o, 3); assert(o->mask == 0x3e);
    assert([r wholeControllerForPlayer:1] == nil && [r wholeControllerForPlayer:2] == nil);
    // Check each left D-pad direction separately (the union alone cannot detect swaps).
    NSArray<TestButton *> *directions = @[g.dpad.down, g.dpad.left, g.dpad.right, g.dpad.up];
    for (TestButton *button in directions) button.isPressed = NO;
    for (NSUInteger i = 0; i < directions.count; i++) {
        directions[i].isPressed = YES; [r read]; assert(o->state[1].buttons == ((1u << i) | extras));
        directions[i].isPressed = NO;
    }
    NSArray<TestButton *> *faces = @[g.buttonX, g.buttonA, g.buttonY, g.buttonB];
    for (TestButton *button in faces) button.isPressed = NO;
    for (NSUInteger i = 0; i < faces.count; i++) {
        faces[i].isPressed = YES; [r read]; assert(o->state[2].buttons == ((1u << i) | extras));
        faces[i].isPressed = NO;
    }
    NSArray<TestButton *> *leftExtras = @[g.leftShoulder, g.leftTrigger, g.buttonOptions, g.leftThumbstickButton];
    NSArray<TestButton *> *rightExtras = @[g.rightShoulder, g.rightTrigger, g.buttonMenu, g.rightThumbstickButton];
    const uint64_t extraBits[] = {DM_BTN_L, DM_BTN_R, DM_BTN_PLUS, DM_BTN_LS};
    for (TestButton *button in leftExtras) button.isPressed = NO;
    for (TestButton *button in rightExtras) button.isPressed = NO;
    for (NSUInteger i = 0; i < leftExtras.count; i++) {
        leftExtras[i].isPressed = YES; [r read];
        assert(o->state[1].buttons == extraBits[i] && o->state[2].buttons == 0);
        leftExtras[i].isPressed = NO;
        rightExtras[i].isPressed = YES; [r read];
        assert(o->state[2].buttons == extraBits[i] && o->state[1].buttons == 0);
        rightExtras[i].isPressed = NO;
    }
    for (TestButton *button in leftExtras) button.isPressed = YES;
    for (TestButton *button in rightExtras) button.isPressed = YES;
    // Test each cardinal axis independently, plus center and partial deflection.
    const float axes[][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 0}, {.5, -.5}};
    for (NSUInteger i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        g.leftThumbstick.xAxis.value = g.rightThumbstick.xAxis.value = axes[i][0];
        g.leftThumbstick.yAxis.value = g.rightThumbstick.yAxis.value = axes[i][1];
        [r read];
        int32_t x = (int32_t)(axes[i][0] * DM_STICK_MAX), y = (int32_t)(axes[i][1] * DM_STICK_MAX);
        expect(o, 1, extras, -y, x, 0, 0); expect(o, 2, extras, y, -x, 0, 0);
    }
    [r sync:@[gc(b)]];
    zero(o, 1); zero(o, 2); assert(o->mask == 0x38);
    expect(o, 4, DM_BTN_Y, -DM_STICK_MAX, 0, 0, 0);
    assert(a.extendedGamepad.valueChangedHandler == nil);
    [r sync:@[]]; zero(o, 4); zero(o, 5); assert(o->mask == (1u << 3));
}
static void hotplug(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    TestController *a = controller(@"Switch Pro Controller"), *b = controller(@"Switch Pro Controller");
    a.extendedGamepad.buttonA.isPressed = YES; a.extendedGamepad.leftThumbstick.xAxis.value = 1;
    b.extendedGamepad.buttonB.isPressed = YES; b.extendedGamepad.leftThumbstick.yAxis.value = -.5;
    [r sync:@[gc(a), gc(b)]]; DmPlayerSource *sa = r.sources[0], *sb = r.sources[1];
    assert(sa.player == -1 && sb.player == -1); sa.player = 0; sb.player = 0; sb.role = DmPlayerShare;
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 123, 0, 0}];
    expect(o, 0, DM_BTN_A | DM_BTN_B | DM_BTN_X, DM_STICK_MAX, 0, 0, 0);
    assert([r wholeControllerForPlayer:0] == nil); // Ambiguous whole-pad motion source.
    [r sync:@[gc(b), gc(a)]]; // Enumeration order is never an identity.
    assert(r.sources[0] == sa && r.sources[1] == sb);
    [r sync:@[gc(b)]];
    expect(o, 0, DM_BTN_B | DM_BTN_X, 0, -16383, 0, 0);
    assert([r wholeControllerForPlayer:0] == gc(b)); assert(a.extendedGamepad.valueChangedHandler == nil);
    [r sync:@[]]; expect(o, 0, DM_BTN_X, 0, 123, 0, 0);
    [r keyboard:(DmPlayerState){0}]; zero(o, 0);
    // Even the identical object, after disconnect, has no claim to its old assignment.
    [r sync:@[gc(a)]]; assert(r.sources[0] != sa && r.sources[0].player == -1); zero(o, 0);
    TestController *replacement = controller(@"Switch Pro Controller"); replacement.extendedGamepad.buttonA.isPressed = YES;
    [r sync:@[gc(replacement), gc(b)]];
    assert(r.sources.count == 2);
    for (DmPlayerSource *s in r.sources) { assert(s.player == -1 && s.rightPlayer == -1 && !s.split); }
    zero(o, 0); assert(o->mask == 1); assert(o->routeChanges == 6);
}
// Control-shaped senders exercise the real target/action method without AppKit views.
@interface DmMacPlayers (TestActions)
- (void)chosen:(id)sender;
- (NSArray<NSNumber *> *)playerChoices;
- (BOOL)selectPlayer:(NSInteger)p source:(DmPlayerSource *)source field:(DmPlayerAssignment)field;
@end
@interface TestChoiceItem : NSObject
@property(nonatomic, strong) NSNumber *representedObject;
@end
@implementation TestChoiceItem
@end
@interface TestChoice : NSObject
@property(nonatomic, strong) DmPlayerSource *source;
@property(nonatomic) NSInteger tag, indexOfSelectedItem, state;
@property(nonatomic, strong) TestChoiceItem *selectedItem;
@end
@implementation TestChoice
@synthesize selectedItem = _selectedItem;
- (TestChoiceItem *)selectedItem {
    if (_selectedItem) return _selectedItem;
    TestChoiceItem *item = [TestChoiceItem new];
    item.representedObject = @(_indexOfSelectedItem - (_tag == DmPlayerAssignmentSplit ? 0 : 1));
    return item;
}
@end
static TestChoice *actualChoice(DmPlayerSource *source, DmPlayerAssignment field, NSInteger player) {
    TestChoice *choice = [TestChoice new]; choice.source = source; choice.tag = field;
    // Deliberately unrelated to the actual player ID, including for split selections.
    choice.indexOfSelectedItem = 0;
    choice.selectedItem = [TestChoiceItem new]; choice.selectedItem.representedObject = @(player);
    return choice;
}
static void assignmentActions(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = 3;
    TestController *a = controller(@"HID"), *b = controller(@"HID"), *c = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(a), gc(b), gc(c)]];
    DmPlayerSource *sb = r.sources[1], *sc = r.sources[2];
    TestChoice *pending = [TestChoice new]; pending.source = sb;
    pending.tag = DmPlayerAssignmentWhole; pending.indexOfSelectedItem = 3; // Player 3
    [r chosen:pending]; assert(sb.player == 2 && o->mask == ((1u << 2) | (1u << 3)));
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 0, 0, 0}];
    c.extendedGamepad.buttonA.isPressed = YES;
    [r sync:@[gc(c), gc(b)]]; // A removed: B's old row now belongs to C.
    assert(r.sources[0] == sb && r.sources[1] == sc);
    pending.indexOfSelectedItem = 5;
    [r chosen:pending]; assert(sb.player == 4 && sc.player == -1);
    [r sync:@[gc(c)]]; // B removed while its action is still retained.
    [r sync:@[gc(b), gc(a), gc(c)]]; // Reordering and even the same controller object reconnecting.
    assert(r.sources[0] == sc && r.sources[1] != sb);
    unsigned routes = o->routeChanges, masks = o->maskChanges;
    for (NSNumber *field in @[@(DmPlayerAssignmentWhole), @(DmPlayerAssignmentRight), @(DmPlayerAssignmentSplit)]) {
        pending.tag = field.integerValue; pending.state = NSControlStateValueOn;
        [r chosen:pending];
    }
    assert(sb.player == 4 && sb.rightPlayer == -1 && !sb.split);
    for (DmPlayerSource *s in r.sources) assert(s.player == -1 && s.rightPlayer == -1 && !s.split);
    assert(r.keyboardPlayer == 3 && o->mask == (1u << 3));
    expect(o, 3, DM_BTN_X, 0, 0, 0, 0);
    assert(o->routeChanges == routes && o->maskChanges == masks);
    // A missing source for a device action is not a keyboard action either.
    pending.source = nil; pending.tag = DmPlayerAssignmentWhole; [r chosen:pending];
    assert(r.keyboardPlayer == 3 && o->routeChanges == routes);
    pending.source = sc; [r chosen:pending];
    assert(sc.player == 4 && o->routeChanges == routes + 1);
    expect(o, 4, DM_BTN_A, 0, 0, 0, 0); expect(o, 3, DM_BTN_X, 0, 0, 0, 0);
    assert(o->mask == ((1u << 3) | (1u << 4)));
    pending.tag = DmPlayerAssignmentSplit; pending.indexOfSelectedItem = 1; [r chosen:pending];
    pending.tag = DmPlayerAssignmentRight; pending.indexOfSelectedItem = 6; [r chosen:pending];
    assert(sc.rightPlayer == 5 && sc.split);
    pending.source = nil; pending.tag = DmPlayerAssignmentKeyboard; pending.indexOfSelectedItem = 2;
    [r chosen:pending]; assert(r.keyboardPlayer == 1); zero(o, 3);
}
// A label-shaped object exercises identification without constructing any AppKit views.
@interface TestLabel : NSObject
@property(nonatomic, copy) NSString *stringValue;
@end
@implementation TestLabel
@end
static void identification(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    TestController *a = controller(@"HID"), *b = controller(@"HID");
    assert([r beginConfiguration]); [r sync:@[gc(a), gc(b)]];
    TestLabel *label = [TestLabel new]; label.stringValue = @"Press a button";
    [r setValue:label forKey:@"activity"];
    TestPad *g = a.extendedGamepad;
    g.buttonA.isPressed = YES;
    g.valueChangedHandler((id)g, (id)g.buttonA);
    assert([label.stringValue isEqualToString:@"Input: Identical model #1"]);
    NSString *identified = label.stringValue;
    TestPad *other = b.extendedGamepad;
    for (NSNumber *value in @[@0, @.001, @(-.001), @1, @0]) {
        other.leftThumbstick.xAxis.value = value.floatValue;
        other.valueChangedHandler((id)other, (id)other.leftThumbstick.xAxis);
        assert([label.stringValue isEqualToString:identified]);
    }
    other.buttonB.isPressed = NO;
    other.valueChangedHandler((id)other, (id)other.buttonB);
    assert([label.stringValue isEqualToString:identified]);
    other.buttonB.isPressed = YES;
    other.valueChangedHandler((id)other, (id)other.buttonB);
    assert([label.stringValue isEqualToString:@"Input: Identical model #2"]);
    for (int p = 0; p < DM_PLAYERS; p++) zero(o, p);
    [r setValue:nil forKey:@"activity"]; [r endConfiguration];
}
static void unavailable(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    TestController *a = controller(@"HID"), *b = controller(@"HID"); a.extendedGamepad = nil;
    assert([r beginConfiguration]); [r sync:@[gc(a), gc(b)]];
    assert(r.sources.count == 2 && r.sources[0].controller == gc(a));
    assert(r.sources[0].player == -1 && r.sources[1].player == 0);
    [r endConfiguration]; assert(o->mask == 1);
    [r sync:@[gc(a)]]; assert(r.sources.count == 1 && o->mask == 0);
}
static void configuration(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = 3;
    TestController *a = controller(@"HID"), *b = controller(@"HID"), *c = controller(@"HID");
    assert([r beginConfiguration]); assert(![r beginConfiguration]);
    [r sync:@[gc(a), gc(b), gc(c)]];
    for (int p = 0; p < 3; p++) assert(r.sources[p].player == p);
    [r endConfiguration];
    a.extendedGamepad.buttonB.isPressed = YES; [r read]; expect(o, 0, DM_BTN_A, 0, 0, 0, 0);
    // Reassignment releases the old slot as well as publishing the new one.
    r.sources[0].player = 6; [r read]; zero(o, 0); expect(o, 6, DM_BTN_A, 0, 0, 0, 0);
    [r keyboard:(DmPlayerState){DM_BTN_X, 1, 2, 3, 4}];
    assert([r beginConfiguration]); zero(o, 6); zero(o, 3);
    r.active = NO; r.active = YES; // Focus notifications cannot reopen the input gate.
    [r keyboard:(DmPlayerState){DM_BTN_A, 9, 8, 7, 6}]; [r read];
    zero(o, 3); zero(o, 6);
    r.active = NO; [r endConfiguration];
    assert(!r.active); zero(o, 6); zero(o, 3);
    r.active = YES; expect(o, 6, DM_BTN_A, 0, 0, 0, 0); zero(o, 3);
    assert([r beginConfiguration]);
    r.active = NO; r.active = YES;
    [r keyboard:(DmPlayerState){DM_BTN_A, 0, 0, 0, 0}];
    [r endConfiguration]; assert(r.active); zero(o, 3);
    expect(o, 6, DM_BTN_A, 0, 0, 0, 0);
    assert([r beginConfiguration]); zero(o, 6);
    // A reconnect while the panel is reopened during play must NOT acquire an automatic slot.
    [r sync:@[gc(b), gc(c)]]; [r sync:@[gc(a), gc(b), gc(c)]];
    assert(r.sources.lastObject.controller == gc(a)); assert(r.sources.lastObject.player == -1);
    [r endConfiguration]; zero(o, 0); zero(o, 6);
}
// Drain only the main dispatch queue/run loop; never instantiate NSApplication or a window.
static void drainCallbacks(void) {
    __block BOOL drained = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ drained = YES; });
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:1];
    while (!drained && deadline.timeIntervalSinceNow > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.001]];
    assert(drained);
}
static void callbacks(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    TestController *c = controller(@"HID");
    TestController *unsupported = controller(@"HID"); unsupported.extendedGamepad = nil;
    [r sync:@[gc(c), gc(unsupported)]]; assert(r.sources.count == 2);
    assert(r.sources[1].controller == gc(unsupported) && r.sources[1].player == -1);
    // Unsupported sources cannot publish a route, even with a programmatic assignment.
    r.sources[1].player = 6; r.sources[1].rightPlayer = 5; r.sources[1].split = YES;
    [r read]; assert(o->mask == 0); assert([r wholeControllerForPlayer:6] == nil);
    r.sources[1].split = NO; [r read]; assert(o->mask == 0);
    assert([r wholeControllerForPlayer:6] == nil);
    r.sources[0].player = 7; [r read];
    TestPad *g = c.extendedGamepad; assert(g.valueChangedHandler != nil);
    assert(c.handlerQueue == dispatch_get_main_queue());
    // Both callbacks before a run-loop turn must publish both edges, not just the final state.
    unsigned before = o->changes[7];
    g.buttonB.isPressed = YES;
    g.valueChangedHandler((GCExtendedGamepad *)(id)g, (GCControllerElement *)(id)g.buttonB);
    expect(o, 7, DM_BTN_A, 0, 0, 0, 0);
    g.buttonB.isPressed = NO;
    g.valueChangedHandler((GCExtendedGamepad *)(id)g, (GCControllerElement *)(id)g.buttonB);
    zero(o, 7); assert(o->changes[7] == before + 2);
    g.buttonB.isPressed = YES;
    g.valueChangedHandler((GCExtendedGamepad *)(id)g, (GCControllerElement *)(id)g.buttonB);
    drainCallbacks(); expect(o, 7, DM_BTN_A, 0, 0, 0, 0);
    g.buttonB.isPressed = NO;
    g.valueChangedHandler((GCExtendedGamepad *)(id)g, (GCControllerElement *)(id)g.buttonB);
    drainCallbacks(); zero(o, 7);
    // A framework callback already queued at unplug cannot resurrect the removed source.
    g.buttonB.isPressed = YES;
    GCExtendedGamepadValueChangedHandler pending = g.valueChangedHandler;
    dispatch_async(dispatch_get_main_queue(), ^{ pending((GCExtendedGamepad *)(id)g, (GCControllerElement *)(id)g.buttonB); });
    [r sync:@[]]; unsigned changes = o->changes[7];
    drainCallbacks(); zero(o, 7); assert(o->mask == 0 && o->changes[7] == changes);
    assert(g.valueChangedHandler == nil);
}
static void lifetimes(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    __weak TestController *weakController;
    __weak TestPad *weakPad;
    @autoreleasepool {
        TestController *c = controller(@"HID"); weakController = c; weakPad = c.extendedGamepad;
        c.extendedGamepad.buttonB.isPressed = YES;
        [r sync:@[gc(c)]]; r.sources[0].player = 6;
    }
    assert(weakController != nil && weakPad != nil); [r read]; expect(o, 6, DM_BTN_A, 0, 0, 0, 0);
    @autoreleasepool { [r sync:@[]]; }
    assert(weakController == nil && weakPad == nil); zero(o, 6);
    // Installed valueChangedHandler must not keep the router alive.
    TestController *c = controller(@"HID"); __weak DmMacPlayers *weakRouter;
    @autoreleasepool { DmMacPlayers *owner = [DmMacPlayers new]; weakRouter = owner; [owner sync:@[gc(c)]]; }
    assert(weakRouter == nil); assert(c.extendedGamepad.valueChangedHandler != nil);
}

// Route every write into an isolated volatile domain, including setInteger: used by the module.
// No standardUserDefaults, persistent-domain mutation, synchronize, or real application preferences.
@interface VolatileDefaults : NSUserDefaults
@property(nonatomic, copy) NSString *domain;
@end
@implementation VolatileDefaults
- (id)objectForKey:(NSString *)key { return [self volatileDomainForName:self.domain][key]; }
- (void)setObject:(id)value forKey:(NSString *)key {
    NSMutableDictionary *values = [[self volatileDomainForName:self.domain] mutableCopy];
    values[key] = value; [self setVolatileDomain:values forName:self.domain];
}
- (void)setInteger:(NSInteger)value forKey:(NSString *)key { [self setObject:@(value) forKey:key]; }
- (void)removeObjectForKey:(NSString *)key {
    NSMutableDictionary *values = [[self volatileDomainForName:self.domain] mutableCopy];
    [values removeObjectForKey:key]; [self setVolatileDomain:values forName:self.domain];
}
@end
static NSDictionary *keyboardPreferences(VolatileDefaults *d) {
    NSMutableDictionary *values = [[d volatileDomainForName:d.domain] mutableCopy];
    [values removeObjectForKey:@"players.controllers.v1"];
    return values;
}
static void preferences(void) {
    NSString *suite = [@"nxnative.tests.players." stringByAppendingString:NSUUID.UUID.UUIDString];
    VolatileDefaults *d = [[VolatileDefaults alloc] initWithSuiteName:suite]; d.domain = suite;
    DmMacPlayers *r = [DmMacPlayers new];
    for (id value in @[@"3", @3.5, @(-2), @(DM_PLAYERS), @[], @{}, [NSData data]]) {
        [d setVolatileDomain:@{@"players.keyboard.v1": value} forName:suite];
        r.keyboardPlayer = 2; [r loadPreferences:d]; assert(r.keyboardPlayer == 2);
    }
    [d setVolatileDomain:@{} forName:suite]; r.keyboardPlayer = 2; [r loadPreferences:d]; assert(r.keyboardPlayer == 2);
    for (NSInteger p = -1; p < DM_PLAYERS; p++) {
        [d setVolatileDomain:@{@"players.keyboard.v1": @(p)} forName:suite];
        [r loadPreferences:d]; assert(r.keyboardPlayer == p);
    }
    // Startup must publish a saved keyboard-only slot through the real connection-mask seam.
    Output *o = [Output new]; DmMacPlayers *loaded = router(o);
    [d setVolatileDomain:@{@"players.keyboard.v1": @3} forName:suite];
    [loaded loadPreferences:d]; assert(o->mask == (1u << 3));
    assert([[loaded valueForKey:@"rememberSetup"] boolValue]); // valid legacy keyboard choices keep Remember checked
    [loaded keyboard:(DmPlayerState){DM_BTN_A, 0, 0, 0, 0}];
    expect(o, 3, DM_BTN_A, 0, 0, 0, 0);
    TestController *c = controller(@"Nintendo Switch Joy-Con (L/R)"); [r sync:@[gc(c)]];
    r.sources[0].split = YES; r.sources[0].player = 1; r.sources[0].rightPlayer = 2;
    [d setVolatileDomain:@{} forName:suite]; r.keyboardPlayer = 3; [r savePreferences:d];
    assert(([keyboardPreferences(d) isEqualToDictionary:@{@"players.keyboard.v1": @3, @"players.keyboard.mode.v1": @(DmPlayerShare)}]));
    DmMacPlayers *fresh = [DmMacPlayers new]; [fresh loadPreferences:d]; [fresh sync:@[gc(c)]];
    assert(fresh.keyboardPlayer == 3 && fresh.sources.count == 1);
    assert(fresh.sources[0].player == -1 && fresh.sources[0].rightPlayer == -1 && !fresh.sources[0].split);
    for (NSNumber *invalid in @[@(-2), @(DM_PLAYERS)]) {
        r.keyboardPlayer = invalid.integerValue; [r savePreferences:d];
        assert(([keyboardPreferences(d) isEqualToDictionary:@{@"players.keyboard.v1": @3, @"players.keyboard.mode.v1": @(DmPlayerShare)}]));
    }
    for (id mode in @[@"0", @0.5, @(-1), @2, @[], @{}, [NSData data]]) {
        [d setVolatileDomain:@{@"players.keyboard.v1": @4, @"players.keyboard.mode.v1": mode} forName:suite];
        [fresh loadPreferences:d]; assert(fresh.keyboardPlayer == 3 && fresh.keyboardRole == DmPlayerShare);
    }
    [d setVolatileDomain:@{@"players.keyboard.v1": @3, @"players.keyboard.mode.v1": @(DmPlayerOwn)} forName:suite];
    [fresh loadPreferences:d]; assert(fresh.keyboardPlayer == 3 && fresh.keyboardRole == DmPlayerOwn);
    [d setVolatileDomain:@{} forName:suite]; [fresh savePreferences:d];
    assert(([keyboardPreferences(d) isEqualToDictionary:@{@"players.keyboard.v1": @3, @"players.keyboard.mode.v1": @(DmPlayerOwn)}]));
    [d removeObjectForKey:@"players.controllers.v1"]; // legacy keyboard-only preferences retain automatic controller slots
    DmMacPlayers *owned = router([Output new]); [owned loadPreferences:d];
    assert(owned.keyboardPlayer == 3 && owned.keyboardRole == DmPlayerOwn);
    assert([owned beginConfiguration]);
    NSMutableArray *devices = [NSMutableArray new];
    for (int i = 0; i < 4; i++) [devices addObject:gc(controller(@"HID"))];
    [owned sync:devices];
    assert(owned.sources[0].player == 0 && owned.sources[1].player == 1 && owned.sources[2].player == 2 && owned.sources[3].player == 4);
    [owned endConfiguration];
    // Fake checkbox and volatile defaults exercise confirmation without any AppKit UI.
    TestChoice *remember = [TestChoice new]; remember.state = NSControlStateValueOn;
    [fresh setValue:remember forKey:@"remember"];
    NSDictionary *before = [[d volatileDomainForName:suite] copy];
    assert([fresh beginConfiguration]); fresh.keyboardPlayer = 6;
    [fresh endConfiguration]; // cancellation/cleanup must not save, even with Remember checked
    [fresh endConfiguration];
    assert([[d volatileDomainForName:suite] isEqualToDictionary:before]);
    remember.state = NSControlStateValueOff; [fresh commitPreferences];
    assert([[d volatileDomainForName:suite] count] == 0); // Play with Remember off forgets the saved setup
    remember.state = NSControlStateValueOn; [fresh commitPreferences];
    assert(([keyboardPreferences(d) isEqualToDictionary:@{@"players.keyboard.v1": @6, @"players.keyboard.mode.v1": @(DmPlayerOwn)}]));
    DmMacPlayers *noDefaults = router([Output new]);
    [noDefaults setValue:remember forKey:@"remember"]; [noDefaults commitPreferences];
    assert(([keyboardPreferences(d) isEqualToDictionary:@{@"players.keyboard.v1": @6, @"players.keyboard.mode.v1": @(DmPlayerOwn)}]));
    [d removeVolatileDomainForName:suite];
}
static void controllerPreferences(void) {
    NSString *suite = [@"nxnative.tests.controller-layout." stringByAppendingString:NSUUID.UUID.UUIDString];
    VolatileDefaults *d = [[VolatileDefaults alloc] initWithSuiteName:suite]; d.domain = suite;
    [d setVolatileDomain:@{} forName:suite];
    TestController *pro = controller(@"Switch Pro Controller"), *pair = controller(@"Nintendo Switch Joy-Con (L/R)");
    pro.vendorName = @"Pro"; pair.vendorName = @"Pair";
    DmMacPlayers *r = router([Output new]); r.keyboardPlayer = 3;
    assert([r beginConfiguration]); [r sync:@[gc(pro), gc(pair)]];
    [r assignSource:r.sources[1] field:DmPlayerAssignmentSplit value:1];
    assert([r assignRoute:r.sources[1] field:DmPlayerAssignmentRight player:2 role:DmPlayerOwn]);
    [r savePreferences:d];
    NSArray *saved = [d objectForKey:@"players.controllers.v1"];
    assert(saved.count == 2);
    // Fresh objects and reversed discovery order still restore by unique model description.
    TestController *p2 = controller(@"Switch Pro Controller"), *j2 = controller(@"Nintendo Switch Joy-Con (L/R)");
    p2.vendorName = @"Pro"; j2.vendorName = @"Pair";
    Output *o = [Output new]; DmMacPlayers *fresh = router(o); [fresh loadPreferences:d];
    assert([fresh beginConfiguration]); [fresh sync:@[gc(j2), gc(p2)]];
    assert(fresh.sources[0].split && fresh.sources[0].player == 1 && fresh.sources[0].rightPlayer == 2);
    assert(fresh.sources[1].player == 0 && fresh.keyboardPlayer == 3 && !fresh.pendingLayoutIndexes.count);
    for (int p = 0; p < DM_PLAYERS; p++) zero(o, p);
    [fresh sync:@[gc(p2)]]; assert(fresh.pendingLayoutIndexes.count == 1);
    [fresh savePreferences:d]; assert([[d objectForKey:@"players.controllers.v1"] count] == 2); // absent pair retained
    [fresh sync:@[gc(p2), gc(j2)]]; assert(fresh.sources[1].split && fresh.sources[1].rightPlayer == 2);
    [fresh endConfiguration]; [fresh sync:@[gc(p2)]]; [fresh sync:@[gc(p2), gc(j2)]];
    assert(fresh.sources[1].player == -1 && !fresh.sources[1].split); // never auto-assign during play

    // Duplicate models remain disabled, even if discovery order changes. A chosen saved
    // route is bound only by an actual button press, never stick jitter or a release.
    DmMacPlayers *twins = router([Output new]); twins.keyboardPlayer = -1;
    TestController *a = controller(@"Twin"), *b = controller(@"Twin");
    assert([twins beginConfiguration]); [twins sync:@[gc(a), gc(b)]]; [twins savePreferences:d];
    NSArray *twinsSaved = [d objectForKey:@"players.controllers.v1"];
    Output *out = [Output new]; DmMacPlayers *next = router(out); [next loadPreferences:d];
    TestController *first = controller(@"Twin"), *second = controller(@"Twin"), *wrong = controller(@"Different");
    assert([next beginConfiguration]); [next sync:@[gc(second), gc(first), gc(wrong)]];
    assert(next.sources[0].player == -1 && next.sources[1].player == -1 && next.pendingLayoutIndexes.count == 2);
    [next savePreferences:d]; assert([[d objectForKey:@"players.controllers.v1"] count] == 3); // no duplicate saved previews
    assert([next armSavedLayout:1]); assert([next armSavedLayout:1]); // same button cancels arming
    first.extendedGamepad.buttonA.isPressed = YES;
    first.extendedGamepad.valueChangedHandler((id)first.extendedGamepad, (id)first.extendedGamepad.buttonA);
    assert(next.sources[1].player == -1);
    assert([next armSavedLayout:1]);
    first.extendedGamepad.valueChangedHandler((id)first.extendedGamepad, (id)first.extendedGamepad.leftThumbstick.xAxis);
    first.extendedGamepad.buttonA.isPressed = NO;
    first.extendedGamepad.valueChangedHandler((id)first.extendedGamepad, (id)first.extendedGamepad.buttonA);
    assert(next.sources[1].player == -1);
    wrong.extendedGamepad.buttonA.isPressed = YES;
    wrong.extendedGamepad.valueChangedHandler((id)wrong.extendedGamepad, (id)wrong.extendedGamepad.buttonA);
    assert(next.pendingLayoutIndexes.count == 2 && next.sources[2].player == -1);
    first.extendedGamepad.buttonA.isPressed = YES;
    first.extendedGamepad.valueChangedHandler((id)first.extendedGamepad, (id)first.extendedGamepad.buttonA);
    assert(next.sources[1].player == 1 && next.pendingLayoutIndexes.count == 1);
    assert([next armSavedLayout:0]);
    second.extendedGamepad.buttonA.isPressed = YES;
    second.extendedGamepad.valueChangedHandler((id)second.extendedGamepad, (id)second.extendedGamepad.buttonA);
    assert(next.sources[0].player == 0 && !next.pendingLayoutIndexes.count);
    for (int p = 0; p < DM_PLAYERS; p++) zero(out, p); // identification never leaks guest input during setup
    [next endConfiguration]; expect(out, 0, DM_BTN_B, 0, 0, 0, 0); expect(out, 1, DM_BTN_B, 0, 0, 0, 0);
    assert(![next armSavedLayout:0]);

    // One connected twin is still ambiguous when two matching saved profiles exist.
    [d setObject:twinsSaved forKey:@"players.controllers.v1"];
    DmMacPlayers *one = router([Output new]); [one loadPreferences:d];
    assert([one beginConfiguration]); [one sync:@[gc(controller(@"Twin"))]];
    assert(one.sources[0].player == -1 && one.pendingLayoutIndexes.count == 2);
    [one savePreferences:d]; assert([[d objectForKey:@"players.controllers.v1"] isEqual:twinsSaved]);
    [one assignSource:one.sources[0] field:DmPlayerAssignmentWhole value:4]; [one savePreferences:d];
    assert([[d objectForKey:@"players.controllers.v1"] count] == 3); // manual edits do not erase absent twins
    assert([[[d objectForKey:@"players.controllers.v1"] firstObject][@"player"] integerValue] == 4);
    assert([[d objectForKey:@"players.controllers.v1"] containsObject:twinsSaved[1]]);
    assert([one forgetSavedLayout:0]); assert(![one forgetSavedLayout:0]);
    [one savePreferences:d]; assert([[d objectForKey:@"players.controllers.v1"] count] == 2);
    assert([[d objectForKey:@"players.controllers.v1"] containsObject:twinsSaved[1]]);

    // A second identical device arriving later invalidates only automatic matches.
    [d setObject:@[twinsSaved[0]] forKey:@"players.controllers.v1"];
    DmMacPlayers *late = router([Output new]); [late loadPreferences:d];
    TestController *earlyDevice = controller(@"Twin"), *lateDevice = controller(@"Twin");
    assert([late beginConfiguration]); [late sync:@[gc(earlyDevice)]]; assert(late.sources[0].player == 0);
    [late sync:@[gc(earlyDevice), gc(lateDevice)]];
    assert(late.sources[0].player == -1 && late.sources[1].player == -1 && late.pendingLayoutIndexes.count == 1);
    [late sync:@[gc(earlyDevice)]]; assert(late.sources[0].player == -1); // no guessing when the twin disconnects
    assert([late armSavedLayout:0]);
    earlyDevice.extendedGamepad.buttonA.isPressed = YES;
    earlyDevice.extendedGamepad.valueChangedHandler((id)earlyDevice.extendedGamepad, (id)earlyDevice.extendedGamepad.buttonA);
    assert(late.sources[0].player == 0);
    [late sync:@[gc(earlyDevice), gc(lateDevice)]]; assert(late.sources[0].player == 0); // explicit confirmation retained
    [late endConfiguration];

    // Restoring a different unique model must not cancel an armed ambiguous model.
    NSMutableDictionary *latePro = [saved[0] mutableCopy]; latePro[@"player"] = @5;
    [d setObject:@[twinsSaved[0], latePro] forKey:@"players.controllers.v1"];
    DmMacPlayers *arming = router([Output new]); [arming loadPreferences:d];
    assert([arming beginConfiguration]); [arming sync:@[gc(earlyDevice), gc(lateDevice)]];
    assert([arming armSavedLayout:0]);
    [arming sync:@[gc(earlyDevice), gc(lateDevice), gc(p2)]];
    assert(arming.sources[2].player == 5);
    earlyDevice.extendedGamepad.valueChangedHandler((id)earlyDevice.extendedGamepad, (id)earlyDevice.extendedGamepad.buttonA);
    assert(arming.sources[0].player == 0 && !arming.pendingLayoutIndexes.count);
    [arming endConfiguration];

    NSMutableDictionary *shared = [twinsSaved[0] mutableCopy]; shared[@"role"] = @(DmPlayerShare);
    [d setObject:@[twinsSaved[0], shared] forKey:@"players.controllers.v1"];
    DmMacPlayers *descriptions = router([Output new]); [descriptions loadPreferences:d];
    assert(![[descriptions savedLayoutDescription:0] isEqual:[descriptions savedLayoutDescription:1]]);

    // Incomplete descriptions require a press, even with one currently connected device.
    NSMutableDictionary *unknown = [twinsSaved[0] mutableCopy]; unknown[@"vendor"] = @"";
    [d setObject:@[unknown] forKey:@"players.controllers.v1"];
    DmMacPlayers *unclear = router([Output new]); [unclear loadPreferences:d];
    TestController *unnamed = controller(@"Twin"); unnamed.vendorName = nil;
    assert([unclear beginConfiguration]); [unclear sync:@[gc(unnamed)]];
    assert(unclear.sources[0].player == -1 && unclear.pendingLayoutIndexes.count == 1);
    [unclear endConfiguration];

    // Forget and reassignment during preview cannot write preferences on cancellation.
    NSDictionary *unchanged = [[d volatileDomainForName:suite] copy];
    DmMacPlayers *cancel = router([Output new]); [cancel loadPreferences:d]; assert([cancel beginConfiguration]);
    assert([cancel forgetSavedLayout:0]); [cancel endConfiguration];
    assert([[d volatileDomainForName:suite] isEqual:unchanged]);
    TestChoice *remember = [TestChoice new]; remember.state = NSControlStateValueOff;
    [cancel setValue:remember forKey:@"remember"]; [cancel commitPreferences];
    assert(![d objectForKey:@"players.controllers.v1"] && ![d objectForKey:@"players.keyboard.v1"]);

    // Saved split assignment is atomic: a conflicting right half cannot partially restore left.
    NSMutableDictionary *conflict = [saved[1] mutableCopy]; conflict[@"rightPlayer"] = @3;
    [d setObject:@[conflict] forKey:@"players.controllers.v1"];
    [d setInteger:3 forKey:@"players.keyboard.v1"]; [d setInteger:DmPlayerOwn forKey:@"players.keyboard.mode.v1"];
    DmMacPlayers *blocked = router([Output new]); [blocked loadPreferences:d];
    TestController *j3 = controller(@"Nintendo Switch Joy-Con (L/R)"); j3.vendorName = @"Pair";
    assert([blocked beginConfiguration]); [blocked sync:@[gc(j3)]];
    assert(!blocked.sources[0].split && blocked.sources[0].player == -1 && blocked.pendingLayoutIndexes.count == 1);

    // Malformed data is ignored, not coerced into players, split states or models.
    for (id invalid in @[@"string", @{}, @[@{}], @[@1]]) {
        [d setObject:invalid forKey:@"players.controllers.v1"];
        DmMacPlayers *bad = router([Output new]); [bad loadPreferences:d];
        assert(!bad.pendingLayoutIndexes.count);
    }
    for (NSString *key in @[@"player", @"rightPlayer", @"role", @"rightRole", @"split", @"category", @"vendor"]) {
        NSMutableDictionary *invalid = [saved[0] mutableCopy]; invalid[key] = @3.5;
        [d setObject:@[invalid] forKey:@"players.controllers.v1"];
        DmMacPlayers *bad = router([Output new]); [bad loadPreferences:d]; assert(!bad.pendingLayoutIndexes.count);
    }
    [r endConfiguration]; [twins endConfiguration]; [one endConfiguration]; [blocked endConfiguration];
    [d removeVolatileDomainForName:suite];
}
static void nativeEventsUntil(BOOL (^ready)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!ready() && deadline.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    assert(ready());
}
static void standardNativeEvents(void) {
    // Native snapshot controls dispatch element callbacks, but not profile callbacks
    // when updated through their public setters. The router must still publish input.
    GCController *native = [GCController controllerWithExtendedGamepad];
    TestController *c = standardController(@"Nintendo Switch Joy-Con (R)");
    c.physicalInputProfile.buttons = native.physicalInputProfile.buttons;
    c.physicalInputProfile.dpads = native.physicalInputProfile.dpads;
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    assert([r beginConfiguration]); [r sync:@[gc(c)]]; [r endConfiguration];
    [native.physicalInputProfile.buttons[GCInputButtonB] setValue:1];
    nativeEventsUntil(^BOOL { return o->state[0].buttons == DM_BTN_A; });
    expect(o, 0, DM_BTN_A, 0, 0, 0, 0);
    [native.physicalInputProfile.dpads[GCInputDirectionPad] setValueForXAxis:0.5f yAxis:-1.0f];
    nativeEventsUntil(^BOOL { return o->state[0].lx == DM_STICK_MAX / 2 && o->state[0].ly == -DM_STICK_MAX; });
    expect(o, 0, DM_BTN_A, DM_STICK_MAX / 2, -DM_STICK_MAX, 0, 0);
    [native.physicalInputProfile.buttons[GCInputButtonB] setValue:0];
    nativeEventsUntil(^BOOL { return o->state[0].buttons == 0; });
    expect(o, 0, 0, DM_STICK_MAX / 2, -DM_STICK_MAX, 0, 0);
    [r sync:@[]]; zero(o, 0);
    assert(!native.physicalInputProfile.buttons[GCInputButtonB].pressedChangedHandler);
    assert(!native.physicalInputProfile.dpads[GCInputDirectionPad].valueChangedHandler);
}
static void standardProfiles(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    TestController *right = standardController(@"Nintendo Switch Joy-Con (R)"), *left = standardController(@"Nintendo Switch Joy-Con (L)");
    assert([r beginConfiguration]); [r sync:@[gc(right), gc(left)]];
    assert(r.sources[0].player == 0 && r.sources[1].player == 1);
    assert(r.assignedRoutes.count == 2);
    for (DmPlayerRoute *route in r.assignedRoutes) assert(route.supported);
    TestPhysicalPad *p = right.physicalInputProfile;
    TestButton *b = p.buttons[GCInputButtonB]; b.isPressed = YES;
    assert(b.pressedChangedHandler);
    b.pressedChangedHandler((id)b, 1, YES);
    zero(o, 0); // Setup consumes identification without publishing gameplay input.
    [r endConfiguration]; expect(o, 0, DM_BTN_A, 0, 0, 0, 0);
    b.isPressed = NO;
    ((TestButton *)p.buttons[GCInputButtonA]).isPressed = YES;
    ((TestButton *)p.buttons[GCInputButtonX]).isPressed = YES;
    ((TestButton *)p.buttons[GCInputButtonY]).isPressed = YES;
    ((TestButton *)p.buttons[GCInputLeftShoulder]).isPressed = YES;
    ((TestButton *)p.buttons[GCInputRightShoulder]).isPressed = YES;
    ((TestButton *)p.buttons[GCInputButtonMenu]).isPressed = YES;
    TestStick *stick = p.dpads[GCInputDirectionPad]; stick.xAxis.value = 0.5f; stick.yAxis.value = -1.0f;
    assert(stick.valueChangedHandler);
    stick.valueChangedHandler((id)stick, 0.5f, -1.0f);
    expect(o, 0, DM_BTN_B | DM_BTN_X | DM_BTN_Y | DM_BTN_L | DM_BTN_R | DM_BTN_PLUS, DM_STICK_MAX / 2, -DM_STICK_MAX, 0, 0);
    // Apple standard profiles already normalize sideways input: do not rotate it again.
    assert([r assignRoute:r.sources[0] field:DmPlayerAssignmentWhole player:2 role:DmPlayerOwn]);
    zero(o, 0); assert(o->state[2].lx == DM_STICK_MAX / 2);
    r.active = NO; zero(o, 2); r.active = YES; assert(o->state[2].lx == DM_STICK_MAX / 2);
    [r assignSource:r.sources[0] field:DmPlayerAssignmentSplit value:1]; assert(!r.sources[0].split); // already an individual device
    [r sync:@[gc(left)]]; zero(o, 2); assert(!b.pressedChangedHandler && !stick.valueChangedHandler);

    NSString *suite = [@"nxnative.tests.standard-profile." stringByAppendingString:NSUUID.UUID.UUIDString];
    VolatileDefaults *d = [[VolatileDefaults alloc] initWithSuiteName:suite]; d.domain = suite;
    [d setVolatileDomain:@{} forName:suite]; [r savePreferences:d];
    assert([[d objectForKey:@"players.controllers.v1"] count] == 1);
    DmMacPlayers *fresh = router([Output new]); [fresh loadPreferences:d]; assert([fresh beginConfiguration]);
    [fresh sync:@[gc(standardController(@"Nintendo Switch Joy-Con (L)"))]];
    assert(fresh.sources[0].player == 1 && !fresh.pendingLayoutIndexes.count);
    [fresh endConfiguration];
    TestController *incomplete = standardController(@"Unknown");
    NSMutableDictionary *buttons = [incomplete.physicalInputProfile.buttons mutableCopy];
    [buttons removeObjectForKey:GCInputButtonX]; incomplete.physicalInputProfile.buttons = buttons;
    DmMacPlayers *unsupported = router([Output new]); unsupported.keyboardPlayer = -1;
    assert([unsupported beginConfiguration]); [unsupported sync:@[gc(incomplete)]];
    assert(unsupported.sources[0].player == -1 && !((DmPlayerRoute *)unsupported.availableRoutes.firstObject).supported);
    [unsupported endConfiguration]; [d removeVolatileDomainForName:suite];
}
static void explicitAssignments(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    NSMutableArray<NSString *> *logs = [NSMutableArray new]; r.routeLog = ^(NSString *line) { [logs addObject:line]; };
    assert([r assignRoute:nil field:DmPlayerAssignmentKeyboard player:3 role:DmPlayerOwn]);
    assert([r beginConfiguration]);
    TestController *pro = controller(@"Switch Pro Controller"), *pair = controller(@"Nintendo Switch Joy-Con (L/R)");
    pro.vendorName = @"Pro Controller"; pair.vendorName = @"Joy-Con pair";
    [r sync:@[gc(pro), gc(pair)]];
    DmPlayerSource *p = r.sources[0], *j = r.sources[1];
    assert(p.player == 0 && j.player == 1);
    [r assignSource:j field:DmPlayerAssignmentSplit value:1];
    assert([r assignRoute:j field:DmPlayerAssignmentRight player:2 role:DmPlayerOwn]);
    NSArray *summary = [r playerSummaries];
    assert(summary.count == 4);
    assert([summary[0] isEqualToString:@"Player 1: Pro Controller #1"]);
    assert([summary[1] isEqualToString:@"Player 2: Left Joy-Con #2"]);
    assert([summary[2] isEqualToString:@"Player 3: Right Joy-Con #2"]);
    assert([summary[3] isEqualToString:@"Player 4: Keyboard"]);
    assert(([r.routeDescriptions isEqualToArray:@[@"Pro Controller #1: Player 1 (own)", @"Left Joy-Con #2: Player 2 (own)",
        @"Right Joy-Con #2: Player 3 (own)", @"Keyboard: Player 4 (own)"]]));
    // Identification names the actual half and ignores release/jitter. No AppKit is instantiated.
    TestLabel *label = [TestLabel new]; [r setValue:label forKey:@"activity"];
    pair.extendedGamepad.buttonA.isPressed = YES;
    pair.extendedGamepad.valueChangedHandler((id)pair.extendedGamepad, (id)pair.extendedGamepad.buttonA);
    assert([label.stringValue isEqualToString:@"Input: Right Joy-Con #2"]);
    pair.extendedGamepad.leftShoulder.isPressed = YES;
    pair.extendedGamepad.valueChangedHandler((id)pair.extendedGamepad, (id)pair.extendedGamepad.leftShoulder);
    assert([label.stringValue isEqualToString:@"Input: Left Joy-Con #2"]);
    [r setValue:nil forKey:@"activity"];
    [r endConfiguration];
    pro.extendedGamepad.buttonA.isPressed = YES;
    pair.extendedGamepad.buttonA.isPressed = pair.extendedGamepad.leftShoulder.isPressed = NO;
    pair.extendedGamepad.dpad.down.isPressed = YES; pair.extendedGamepad.buttonX.isPressed = YES;
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 0, 0, 0}];
    expect(o, 0, DM_BTN_A, 0, 0, 0, 0); expect(o, 1, DM_BTN_A, 0, 0, 0, 0);
    expect(o, 2, DM_BTN_A, 0, 0, 0, 0); expect(o, 3, DM_BTN_X, 0, 0, 0, 0);
    unsigned changes = o->changes[3], routes = o->routeChanges;
    assert(![r assignRoute:nil field:DmPlayerAssignmentKeyboard player:0 role:DmPlayerOwn]);
    assert(r.keyboardPlayer == 3 && r.keyboardRole == DmPlayerOwn && o->changes[3] == changes && o->routeChanges == routes);
    assert([r.assignmentError containsString:@"Previous assignment kept"]);
    expect(o, 3, DM_BTN_X, 0, 0, 0, 0); expect(o, 0, DM_BTN_A, 0, 0, 0, 0);
    assert(![r assignRoute:j field:DmPlayerAssignmentRight player:1 role:DmPlayerOwn]);
    assert(j.rightPlayer == 2); expect(o, 2, DM_BTN_A, 0, 0, 0, 0);
    assert([r assignRoute:nil field:DmPlayerAssignmentKeyboard player:0 role:DmPlayerShare]);
    assert(r.assignmentError == nil); zero(o, 3);
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 0, 0, 0}]; expect(o, 0, DM_BTN_A | DM_BTN_X, 0, 0, 0, 0);
    assert([[r playerSummaries][0] isEqualToString:@"Player 1: Pro Controller #1 + Keyboard"]);
    // Invalid programmatic state cannot silently merge own routes (or shared keyboard input).
    j.player = 0; [r read]; zero(o, 0); assert([r wholeControllerForPlayer:0] == nil);
    assert([[r playerSummaries][0] containsString:@"input blocked"]);
    j.player = 1; [r read]; expect(o, 0, DM_BTN_A | DM_BTN_X, 0, 0, 0, 0);
    NSUInteger count = logs.count; [r read]; [r read]; assert(logs.count == count); // no frame logging
    assert([r beginConfiguration]); [r endConfiguration];
    assert([logs containsObject:@"confirm — Keyboard: Player 1 (shared)"]);
    assert([logs containsObject:@"confirm — Left Joy-Con #2: Player 2 (own)"]);
    assert([logs containsObject:@"summary — Player 1: Pro Controller #1 + Keyboard"]);
    assert([logs containsObject:@"connect — Pro Controller #1: Player 1 (own)"]);
    assert([logs containsObject:@"assignment — Right Joy-Con #2: Player 3 (own)"]);
    TestController *extra = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(pro), gc(pair), gc(extra)]]; DmPlayerSource *e = r.sources.lastObject;
    assert(e.player == -1); // new during gameplay stays off, even on reopening
    assert([r beginConfiguration]); [r sync:@[gc(pro), gc(pair), gc(extra)]]; assert(e.player == -1);
    [r assignSource:e field:DmPlayerAssignmentSplit value:1];
    assert([r assignRoute:e field:DmPlayerAssignmentWhole player:4 role:DmPlayerOwn]);
    assert([r assignRoute:e field:DmPlayerAssignmentRight player:5 role:DmPlayerOwn]);
    assert([r playerSummaries].count == 5);
    assert([[r playerSummaries][3] isEqualToString:@"Player 5: Left Joy-Con #3"]);
    assert([[r playerSummaries][4] isEqualToString:@"Player 6: Right Joy-Con #3"]);
    [r endConfiguration];
    [r sync:@[gc(pro), gc(extra)]];
    assert([logs containsObject:@"disconnect — Left Joy-Con #2: Player 2 (own)"]);
    assert([logs containsObject:@"disconnect — Right Joy-Con #2: Player 3 (own)"]);
    count = logs.count; routes = o->routeChanges;
    for (NSNumber *field in @[@(DmPlayerAssignmentWhole), @(DmPlayerAssignmentRight), @(DmPlayerAssignmentSplit),
                              @(DmPlayerAssignmentWholeMode), @(DmPlayerAssignmentRightMode)])
        [r assignSource:j field:field.integerValue value:1];
    assert(![r assignRoute:j field:DmPlayerAssignmentWhole player:6 role:DmPlayerShare]);
    assert(logs.count == count && o->routeChanges == routes);
}
static void playerSelection(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    TestController *pro = controller(@"Switch Pro Controller"); pro.vendorName = @"Pro Controller";
    assert([r beginConfiguration]); [r sync:@[gc(pro)]];
    DmPlayerSource *s = r.sources[0];
    // Default setup is one player: Pro owns Player 1, keyboard contributes to it.
    assert(s.player == 0 && s.role == DmPlayerOwn);
    assert(r.keyboardPlayer == 0 && r.keyboardRole == DmPlayerShare);
    assert(r.playerSummaries.count == 1 && o->mask == 1);
    assert([r.playerSummaries[0] isEqualToString:@"Player 1: Pro Controller #1 + Keyboard"]);
    [r endConfiguration];
    pro.extendedGamepad.buttonA.isPressed = YES; [r read];
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 0, 0, 0}];
    expect(o, 0, DM_BTN_A | DM_BTN_X, 0, 0, 0, 0);
    // New is a separate player, not a visible Own/Share mode selection.
    assert([r selectPlayer:-2 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.keyboardPlayer == 1 && r.keyboardRole == DmPlayerOwn);
    assert(r.playerSummaries.count == 2 && o->mask == 3);
    expect(o, 0, DM_BTN_A, 0, 0, 0, 0); zero(o, 1);
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 0, 0, 0}]; expect(o, 1, DM_BTN_X, 0, 0, 0, 0);
    // The numeric UI route must use the same policy, including an occupied destination.
    [r assignSource:nil field:DmPlayerAssignmentKeyboard value:0];
    assert(r.keyboardPlayer == 0 && r.keyboardRole == DmPlayerShare && r.assignmentError == nil);
    zero(o, 1); assert(r.playerSummaries.count == 1 && o->mask == 1);
    [r keyboard:(DmPlayerState){DM_BTN_X, 0, 0, 0, 0}];
    expect(o, 0, DM_BTN_A | DM_BTN_X, 0, 0, 0, 0);
    // Reselecting its own player must not turn the sole owner into a shared route.
    assert([r selectPlayer:0 source:s field:DmPlayerAssignmentWhole]);
    assert(s.role == DmPlayerOwn && r.keyboardRole == DmPlayerShare);
    assert([r selectPlayer:-2 source:s field:DmPlayerAssignmentWhole]);
    assert(s.player == 1 && s.role == DmPlayerOwn && o->mask == 3);
    expect(o, 0, DM_BTN_X, 0, 0, 0, 0); expect(o, 1, DM_BTN_A, 0, 0, 0, 0);
    assert([r selectPlayer:0 source:s field:DmPlayerAssignmentWhole]);
    assert(s.role == DmPlayerOwn); zero(o, 1);
    expect(o, 0, DM_BTN_A | DM_BTN_X, 0, 0, 0, 0);
    assert([r selectPlayer:6 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.keyboardPlayer == 6 && r.keyboardRole == DmPlayerOwn);
    assert([r selectPlayer:6 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.keyboardRole == DmPlayerOwn && o->mask == ((1u << 6) | 1));
    assert([r selectPlayer:-1 source:s field:DmPlayerAssignmentWhole]);
    zero(o, 0); assert(o->mask == (1u << 6));
    // A keyboard primary on Player 1 stays there when Pro creates a new player.
    assert([r selectPlayer:0 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.keyboardRole == DmPlayerOwn);
    assert([r selectPlayer:-2 source:s field:DmPlayerAssignmentWhole]);
    assert(s.player == 1 && s.role == DmPlayerOwn);
    assert(r.keyboardPlayer == 0 && r.keyboardRole == DmPlayerOwn);
    assert(r.playerSummaries.count == 2 && o->mask == 3);
}
static void splitSelection(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    TestController *pair = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(pair)]]; DmPlayerSource *s = r.sources[0];
    [r assignSource:s field:DmPlayerAssignmentSplit value:1]; assert(s.split);
    assert([r selectPlayer:-2 source:s field:DmPlayerAssignmentWhole]);
    assert([r selectPlayer:-2 source:s field:DmPlayerAssignmentRight]);
    assert(s.player == 1 && s.rightPlayer == 2 && s.role == DmPlayerOwn && s.rightRole == DmPlayerOwn);
    pair.extendedGamepad.dpad.down.isPressed = YES; pair.extendedGamepad.buttonY.isPressed = YES; [r read];
    expect(o, 1, DM_BTN_A, 0, 0, 0, 0); expect(o, 2, DM_BTN_X, 0, 0, 0, 0);
    [r chosen:actualChoice(s, DmPlayerAssignmentRight, 1)];
    assert(s.rightPlayer == 1 && s.rightRole == DmPlayerShare && s.role == DmPlayerOwn);
    expect(o, 1, DM_BTN_A | DM_BTN_X, 0, 0, 0, 0); zero(o, 2);
    assert(r.playerSummaries.count == 2 && o->mask == 3);
    assert([r selectPlayer:-2 source:s field:DmPlayerAssignmentRight]);
    assert(s.rightPlayer == 2 && s.rightRole == DmPlayerOwn);
    expect(o, 1, DM_BTN_A, 0, 0, 0, 0); expect(o, 2, DM_BTN_X, 0, 0, 0, 0);
    [r sync:@[]]; unsigned routes = o->routeChanges, masks = o->maskChanges;
    assert(![r selectPlayer:-2 source:s field:DmPlayerAssignmentWhole]);
    assert(![r selectPlayer:0 source:s field:DmPlayerAssignmentRight]);
    assert(o->routeChanges == routes && o->maskChanges == masks);
    [r sync:@[gc(pair)]]; DmPlayerSource *replacement = r.sources[0];
    assert(replacement != s && !replacement.split && replacement.player == -1);
    routes = o->routeChanges; masks = o->maskChanges;
    assert(![r selectPlayer:0 source:s field:DmPlayerAssignmentWhole]);
    [r chosen:actualChoice(s, DmPlayerAssignmentRight, -2)];
    assert(o->routeChanges == routes && o->maskChanges == masks && replacement.player == -1);
    assert(![r selectPlayer:0 source:nil field:DmPlayerAssignmentWhole]);
    assert(![r selectPlayer:0 source:replacement field:DmPlayerAssignmentKeyboard]);
    assert(![r selectPlayer:DM_PLAYERS source:nil field:DmPlayerAssignmentKeyboard]);
    assert(![r selectPlayer:-3 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.keyboardPlayer == 0 && o->mask == 1);
    TestController *unsupported = controller(@"Unsupported"); unsupported.extendedGamepad = nil;
    [r sync:@[gc(pair), gc(unsupported)]];
    routes = o->routeChanges; masks = o->maskChanges;
    assert(![r selectPlayer:-2 source:r.sources.lastObject field:DmPlayerAssignmentWhole]);
    assert(r.sources.lastObject.player == -1 && o->routeChanges == routes && o->maskChanges == masks);
}
static void playerChoices(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    TestController *pro = controller(@"Switch Pro Controller"), *pair = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(pro), gc(pair)]];
    DmPlayerSource *p = r.sources[0], *j = r.sources[1];
    assert([r assignRoute:p field:DmPlayerAssignmentWhole player:0 role:DmPlayerOwn]);
    // A shared keyboard does not create another player or a placeholder choice.
    assert(r.keyboardPlayer == 0 && r.keyboardRole == DmPlayerShare);
    assert(([[r playerChoices] isEqualToArray:@[@0, @(-2)]]));
    assert([r assignRoute:p field:DmPlayerAssignmentWhole player:1 role:DmPlayerOwn]);
    assert([r assignRoute:nil field:DmPlayerAssignmentKeyboard player:0 role:DmPlayerOwn]);
    assert(([[r playerChoices] isEqualToArray:@[@0, @1, @(-2)]]));
    // Sparse IDs remain actual IDs, sorted independently of device enumeration.
    assert([r assignRoute:j field:DmPlayerAssignmentWhole player:6 role:DmPlayerOwn]);
    assert(([[r playerChoices] isEqualToArray:@[@0, @1, @6, @(-2)]]));
    TestChoice *pending = actualChoice(nil, DmPlayerAssignmentKeyboard, 6);
    assert([r assignRoute:nil field:DmPlayerAssignmentKeyboard player:0 role:DmPlayerShare]);
    [r sync:@[gc(pair)]]; // Player 2 disappears while the Player 7 action is pending.
    assert(([[r playerChoices] isEqualToArray:@[@0, @6, @(-2)]]));
    [r chosen:pending];
    assert(r.keyboardPlayer == 6 && r.keyboardRole == DmPlayerShare && j.player == 6);
    assert(o->mask == (1u << 6));
    assert(([[r playerChoices] isEqualToArray:@[@6, @(-2)]]));
    // Split uses represented 0/1 too, not the popup's (deliberately zero) index.
    TestChoice *splitChoice = actualChoice(j, DmPlayerAssignmentSplit, 1);
    [r chosen:splitChoice]; assert(j.split);
    splitChoice.selectedItem.representedObject = @0;
    [r chosen:splitChoice]; assert(!j.split);
    [r sync:@[]];
    assert(([[r playerChoices] isEqualToArray:@[@6, @(-2)]])); // keyboard still uses Player 7
    assert([r selectPlayer:-1 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(([[r playerChoices] isEqualToArray:@[@(-2)]]));
}
static void newPlayerChoices(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    NSMutableArray *devices = [NSMutableArray new];
    for (int i = 0; i < DM_PLAYERS; i++) [devices addObject:gc(controller(@"HID"))];
    [r sync:devices];
    // Slot 0 has no owner but IS used by the shared keyboard. New must skip it.
    TestChoice *create = actualChoice(r.sources[0], DmPlayerAssignmentWhole, -2);
    [r chosen:create]; assert(r.sources[0].player == 1);
    assert(([[r playerChoices] isEqualToArray:@[@0, @1, @(-2)]]));
    for (int i = 1; i < DM_PLAYERS - 1; i++) {
        create.source = r.sources[i]; [r chosen:create];
        assert(r.sources[i].player == i + 1);
    }
    NSArray<NSNumber *> *full = @[@0, @1, @2, @3, @4, @5, @6, @7];
    assert([[r playerChoices] isEqualToArray:full]);
    assert(o->mask == 0xff);
    create.source = r.sources.lastObject;
    unsigned changes = o->routeChanges;
    [r chosen:create]; // A queued New action cannot allocate a ninth player.
    assert(![r selectPlayer:-2 source:create.source field:DmPlayerAssignmentWhole]);
    assert(create.source.player == -1 && o->mask == 0xff && o->routeChanges == changes);
    assert([[r playerChoices] isEqualToArray:full]);
    // Keyboard New also fails atomically: no ninth player and no role/target change.
    assert(![r selectPlayer:-2 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.keyboardPlayer == 0 && r.keyboardRole == DmPlayerShare);
    assert(o->mask == 0xff && o->routeChanges == changes);
    // Even an already-assigned route must not be removed/reallocated at the cap.
    create.source = r.sources[0]; [r chosen:create];
    assert(create.source.player == 1 && o->mask == 0xff && o->routeChanges == changes);
    [devices removeObjectAtIndex:2]; [r sync:devices]; // Leave a hole at actual ID 3.
    assert(([[r playerChoices] isEqualToArray:@[@0, @1, @2, @4, @5, @6, @7, @(-2)]]));
    assert(r.sources[2].player == 4 && r.sources[5].player == 7); // no renumbering
    create.source = r.sources.lastObject; [r chosen:create];
    assert(create.source.player == 3 && [[r playerChoices] isEqualToArray:full]);
    assert(o->mask == 0xff);
}
static void ownedKeyboardHotplug(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o);
    // Player 1 is not implicitly shared: an owned keyboard there must also reserve its slot.
    assert([r assignRoute:nil field:DmPlayerAssignmentKeyboard player:0 role:DmPlayerOwn]);
    assert([r beginConfiguration]);
    TestController *a = controller(@"HID"), *b = controller(@"HID");
    [r sync:@[gc(a)]]; assert(r.sources[0].player == 1);
    [r sync:@[gc(a), gc(b)]]; assert(r.sources[1].player == 2);
    assert(r.keyboardPlayer == 0 && r.keyboardRole == DmPlayerOwn);
    [r endConfiguration];
}
static void routeChooser(void) {
    // These are the exact snapshots the embedded overview and inline chooser render.
    // No NSApplication, windows, controls, event monitors or real preferences are used.
    DmMacPlayers *r = router([Output new]);
    assert(r.assignedRoutes.count == 1 && r.availableRoutes.count == 0);
    assert(r.assignedRoutes[0].source == nil && r.assignedRoutes[0].field == DmPlayerAssignmentKeyboard);
    TestController *a = controller(@"Nintendo Switch Joy-Con (L/R)");
    TestController *b = controller(@"Nintendo Switch Joy-Con (L/R)");
    TestController *unsupported = controller(@"Unsupported"); unsupported.extendedGamepad = nil;
    [r sync:@[gc(a), gc(b), gc(unsupported)]];
    assert(r.assignedRoutes.count == 1 && r.availableRoutes.count == 3);
    assert(!r.availableRoutes.lastObject.supported);
    DmPlayerSource *leftPair = r.sources[0], *rightPair = r.sources[1];
    [r assignSource:leftPair field:DmPlayerAssignmentSplit value:1];
    [r assignSource:rightPair field:DmPlayerAssignmentSplit value:1];
    assert(r.availableRoutes.count == 5); // four independent halves plus unavailable device
    assert([r assignRoute:leftPair field:DmPlayerAssignmentWhole player:0 role:DmPlayerOwn]);
    assert([r assignRoute:leftPair field:DmPlayerAssignmentRight player:1 role:DmPlayerOwn]);
    assert([r assignRoute:rightPair field:DmPlayerAssignmentWhole player:2 role:DmPlayerOwn]);
    assert([r assignRoute:rightPair field:DmPlayerAssignmentRight player:3 role:DmPlayerOwn]);
    assert(r.assignedRoutes.count == 5 && r.availableRoutes.count == 1);
    assert(r.assignedRoutes[0].source == leftPair && r.assignedRoutes[1].field == DmPlayerAssignmentRight);
    DmPlayerRoute *stale = r.assignedRoutes[1];
    assert([r selectPlayer:-1 source:leftPair field:DmPlayerAssignmentRight]);
    assert(r.assignedRoutes.count == 4 && r.availableRoutes.count == 2);
    assert(r.availableRoutes[0].source == leftPair && r.availableRoutes[0].field == DmPlayerAssignmentRight);
    [r assignSource:leftPair field:DmPlayerAssignmentRight value:0];
    assert(leftPair.rightPlayer == 0 && leftPair.rightRole == DmPlayerShare);
    [r sync:@[gc(b), gc(unsupported)]];
    assert(r.assignedRoutes.count == 3 && r.availableRoutes.count == 1);
    [r sync:@[gc(a), gc(b), gc(unsupported)]];
    assert(r.availableRoutes.count == 2); // reconnect is unassigned, together, a fresh identity
    assert(![r assignRoute:stale.source field:stale.field player:4 role:DmPlayerOwn]);
    assert(r.assignedRoutes.count == 3);
    assert([r selectPlayer:-1 source:nil field:DmPlayerAssignmentKeyboard]);
    assert(r.assignedRoutes.count == 2 && r.availableRoutes.lastObject.source == nil);
    assert(([r.playerSummaries isEqualToArray:@[@"Player 3: Left Joy-Con #2", @"Player 4: Right Joy-Con #2"]]));
    assert([r selectPlayer:-1 source:rightPair field:DmPlayerAssignmentWhole]);
    assert([r selectPlayer:-1 source:rightPair field:DmPlayerAssignmentRight]);
    assert(r.assignedRoutes.count == 0 && r.playerSummaries.count == 0);
    assert([r beginConfiguration]); assert(![r beginConfiguration]);
    [r endConfiguration]; [r endConfiguration]; // embedding can close without ever constructing a view
}
// macOS owns the user's button remapping. Keep the existing Nintendo-profile
// contract; do not add another swap after the user corrects the macOS profile.
static void pro_controller_keeps_macos_button_names(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    TestController *c = controller(@"Switch Pro Controller");
    [r sync:@[gc(c)]];
    assert([r assignRoute:r.sources[0] field:DmPlayerAssignmentWhole player:0 role:DmPlayerOwn]);
    NSArray<TestButton *> *buttons = @[c.extendedGamepad.buttonA, c.extendedGamepad.buttonB,
                                       c.extendedGamepad.buttonX, c.extendedGamepad.buttonY];
    const uint64_t expected[] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y};
    for (NSUInteger i = 0; i < buttons.count; i++) {
        buttons[i].isPressed = YES; [r read]; expect(o, 0, expected[i], 0, 0, 0, 0);
        buttons[i].isPressed = NO; [r read]; zero(o, 0);
    }
}
static void marioKartRails(void) {
    Output *o = [Output new]; DmMacPlayers *r = router(o); r.keyboardPlayer = -1;
    TestController *c = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(c)]];
    DmPlayerSource *s = r.sources[0]; s.split = YES; s.player = 0; s.rightPlayer = 1;
    __block uint8_t left = 0, right = 0; __block BOOL available = YES;
    r.joyConRails = ^BOOL(int side, uint8_t *bits) { *bits = side ? right : left; return available; };
    c.extendedGamepad.leftShoulder.isPressed = c.extendedGamepad.leftTrigger.isPressed = YES;
    c.extendedGamepad.rightShoulder.isPressed = c.extendedGamepad.rightTrigger.isPressed = YES;
    [r read]; // Default keeps every other game's legacy sideways shoulders.
    expect(o, 0, DM_BTN_L | DM_BTN_R, 0, 0, 0, 0); expect(o, 1, DM_BTN_L | DM_BTN_R, 0, 0, 0, 0);
    r.nativeSingleJoyCons = YES; [r read]; zero(o, 0); zero(o, 1);
    assert(o->styles[0] == 8 && o->styles[1] == 16);
    left = 0x20; right = 0x10; [r read];
    expect(o, 0, 1ull << 24, 0, 0, 0, 0); expect(o, 1, 1ull << 25, 0, 0, 0, 0);
    left = 0; [r read]; zero(o, 0); expect(o, 1, 1ull << 25, 0, 0, 0, 0);
    c.extendedGamepad.buttonOptions.isPressed = YES; [r read];
    expect(o, 0, DM_BTN_PLUS, 0, 0, 0, 0); assert(o->styles[0] == 8); // HLE converts logical menu to native Minus
    c.extendedGamepad.buttonOptions.isPressed = NO; [r read]; zero(o, 0);
    right = 0x30; [r read]; expect(o, 1, 3ull << 24, 0, 0, 0, 0);
    available = NO; [r read]; zero(o, 1); assert(o->styles[1] == 1); // no outer-shoulder fallback
    available = YES; r.active = NO; zero(o, 0); zero(o, 1);
    right = 0; r.active = YES; zero(o, 0); zero(o, 1);
    left = right = 0x20; [r read];
    TestController *other = controller(@"Nintendo Switch Joy-Con (L/R)");
    [r sync:@[gc(c), gc(other)]]; zero(o, 0); zero(o, 1); // no guessed physical identity
    assert(o->styles[0] == 1 && o->styles[1] == 1);
    [r sync:@[gc(c)]]; expect(o, 0, 1ull << 24, 0, 0, 0, 0);
    r.keyboardPlayer = 0; [r read]; assert(o->styles[0] == 1); // shared route uses FullKey fallback
    r.keyboardPlayer = -1;
    s.split = NO; [r read];
    expect(o, 0, DM_BTN_L | DM_BTN_R | DM_BTN_ZL | DM_BTN_ZR, 0, 0, 0, 0);
    assert(o->styles[0] == 1); zero(o, 1); // Together stays normal
    [r sync:@[]]; zero(o, 0); zero(o, 1);
    TestController *individual = standardController(@"Nintendo Switch Joy-Con (R)");
    [r sync:@[gc(individual)]]; r.sources[0].player = 2;
    ((TestButton *)individual.physicalInputProfile.buttons[GCInputLeftShoulder]).isPressed = YES;
    ((TestButton *)individual.physicalInputProfile.buttons[GCInputRightShoulder]).isPressed = YES;
    right = 0x10; [r read]; expect(o, 2, 1ull << 25, 0, 0, 0, 0); assert(o->styles[2] == 16);
    r.nativeSingleJoyCons = NO; [r read]; expect(o, 2, DM_BTN_L | DM_BTN_R, 0, 0, 0, 0);
    r.nativeSingleJoyCons = YES;
    TestController *pro = controller(@"Switch Pro Controller");
    [r sync:@[gc(pro)]]; r.sources[0].player = 3;
    pro.extendedGamepad.leftShoulder.isPressed = pro.extendedGamepad.leftTrigger.isPressed = YES;
    [r read]; expect(o, 3, DM_BTN_L | DM_BTN_ZL, 0, 0, 0, 0); assert(o->styles[3] == 1);
}
int main(void) { @autoreleasepool {
    marioKartRails();
    pro_controller_keeps_macos_button_names();
    keyboard(); handoff(); full(); split(); hotplug(); assignmentActions(); identification(); unavailable(); configuration(); callbacks(); lifetimes(); preferences(); controllerPreferences(); standardNativeEvents(); standardProfiles();
    explicitAssignments(); ownedKeyboardHotplug(); playerSelection(); splitSelection(); routeChooser(); playerChoices(); newPlayerChoices();
    puts("t_players_mac OK");
} return 0; }
