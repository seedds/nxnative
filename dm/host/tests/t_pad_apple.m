// Face buttons and player lights of the shared GameController adapter (pad_apple.h) with fake pads: by position on most pads, by label on a Nintendo pad; no physical controller is needed.
#import "../pad_apple.h"
#include <assert.h>
#include <stdio.h>

@interface TestButton : NSObject
@property BOOL isPressed;
@end
@implementation TestButton
@end
@interface TestController : NSObject
@property NSString *productCategory;
@property GCControllerPlayerIndex playerIndex;
@end
@implementation TestController
@end
@interface TestPad : NSObject
@property TestController *controller;
@property TestButton *buttonA, *buttonB, *buttonX, *buttonY;
@end
@implementation TestPad
@end

// The Switch buttons of a pad of this category with only the named GameController button pressed
static uint64_t face(NSString *category, char pressed) {
    TestPad *g = [TestPad new]; g.controller = [TestController new]; g.controller.productCategory = category;
    g.buttonA = [TestButton new]; g.buttonB = [TestButton new]; g.buttonX = [TestButton new]; g.buttonY = [TestButton new];
    TestButton *b = pressed == 'A' ? g.buttonA : pressed == 'B' ? g.buttonB : pressed == 'X' ? g.buttonX : g.buttonY;
    b.isPressed = YES;
    return dm_pad_face((GCExtendedGamepad *)(id)g);
}

int main(void) { @autoreleasepool {
    for (NSString *k in @[@"Xbox One", @"DualSense", @"HID"]) {   // by position: buttonA bottom (B), buttonB right (A), buttonX left (Y), buttonY top (X)
        assert(face(k, 'A') == DM_BTN_B); assert(face(k, 'B') == DM_BTN_A); assert(face(k, 'X') == DM_BTN_Y); assert(face(k, 'Y') == DM_BTN_X);
    }
    assert(face(nil, 'B') == DM_BTN_A);   // no category: by position
    for (NSString *k in @[@"Switch Pro Controller", @"Nintendo Switch Joy-Con (L/R)", @"Nintendo Switch Joy-Con (R)"]) {   // by label
        assert(face(k, 'A') == DM_BTN_A); assert(face(k, 'B') == DM_BTN_B); assert(face(k, 'X') == DM_BTN_X); assert(face(k, 'Y') == DM_BTN_Y);
    }
    TestPad *g = [TestPad new];   // nothing pressed, buttons nil: none
    assert(dm_pad_face((GCExtendedGamepad *)(id)g) == 0);
    // Player lights: nil is a no-op, players 1-4 light their own, 5-8 and a free controller clear the index
    assert(dm_pad_lights(nil, 0) == 0);
    TestController *c = [TestController new]; GCController *gc = (GCController *)(id)c;
    c.playerIndex = GCControllerPlayerIndexUnset;
    GCControllerPlayerIndex want[4] = {GCControllerPlayerIndex1, GCControllerPlayerIndex2, GCControllerPlayerIndex3, GCControllerPlayerIndex4};
    for (int p = 0; p < 4; p++) { assert(dm_pad_lights(gc, p) == 1); assert(c.playerIndex == want[p]); }
    for (int p = 4; p < 8; p++) { c.playerIndex = GCControllerPlayerIndex2; assert(dm_pad_lights(gc, p) == 0); assert(c.playerIndex == GCControllerPlayerIndexUnset); }
    c.playerIndex = GCControllerPlayerIndex3; assert(dm_pad_lights(gc, -1) == 0); assert(c.playerIndex == GCControllerPlayerIndexUnset);
    printf("t_pad_apple OK\n");
} return 0; }
