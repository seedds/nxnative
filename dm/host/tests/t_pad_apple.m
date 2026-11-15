// Face and full-gamepad mapping of the shared GameController adapter (pad_apple.h) with fake pads: Switch positions, Xbox/PlayStation labels (DM_PAD_LAYOUT), and DM_PAD_MAP.
#import "../pad_apple.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

@interface TestButton : NSObject
@property BOOL isPressed;
@end
@implementation TestButton
@end
@interface TestController : NSObject
@property NSString *productCategory;
@end
@implementation TestController
@end
@interface TestDpad : NSObject
@property TestButton *left, *up, *right, *down;
@end
@implementation TestDpad
@end
@interface TestPad : NSObject
@property TestController *controller;
@property TestButton *buttonA, *buttonB, *buttonX, *buttonY;
@property TestButton *leftShoulder, *rightShoulder, *leftTrigger, *rightTrigger;
@property TestButton *buttonMenu, *buttonOptions, *leftThumbstickButton, *rightThumbstickButton;
@property TestDpad *dpad;
@end
@implementation TestPad
@end

static TestPad *make_pad(NSString *category) {
    TestPad *g = [TestPad new]; g.controller = [TestController new]; g.controller.productCategory = category;
    g.buttonA = [TestButton new]; g.buttonB = [TestButton new]; g.buttonX = [TestButton new]; g.buttonY = [TestButton new];
    g.leftShoulder = [TestButton new]; g.rightShoulder = [TestButton new]; g.leftTrigger = [TestButton new]; g.rightTrigger = [TestButton new];
    g.buttonMenu = [TestButton new]; g.buttonOptions = [TestButton new]; g.leftThumbstickButton = [TestButton new]; g.rightThumbstickButton = [TestButton new];
    g.dpad = [TestDpad new]; g.dpad.left = [TestButton new]; g.dpad.up = [TestButton new]; g.dpad.right = [TestButton new]; g.dpad.down = [TestButton new];
    return g;
}

// The Switch buttons of a pad of this category with only the named GameController button pressed
static uint64_t face(NSString *category, char pressed) {
    TestPad *g = make_pad(category);
    TestButton *b = pressed == 'A' ? g.buttonA : pressed == 'B' ? g.buttonB : pressed == 'X' ? g.buttonX : g.buttonY;
    b.isPressed = YES;
    return dm_pad_face((GCExtendedGamepad *)(id)g);
}

int main(void) { @autoreleasepool {
    unsetenv("DM_PAD_LAYOUT"); unsetenv("DM_PAD_MAP");
    for (NSString *k in @[@"Xbox One", @"DualSense", @"DualShock 4", @"HID"]) {   // by position: buttonA bottom (B), buttonB right (A), buttonX left (Y), buttonY top (X)
        assert(face(k, 'A') == DM_BTN_B); assert(face(k, 'B') == DM_BTN_A); assert(face(k, 'X') == DM_BTN_Y); assert(face(k, 'Y') == DM_BTN_X);
    }
    assert(face(nil, 'B') == DM_BTN_A);   // no category: by position
    for (NSString *k in @[@"Switch Pro Controller", @"Nintendo Switch Joy-Con (L/R)", @"Nintendo Switch Joy-Con (R)"]) {   // by label
        assert(face(k, 'A') == DM_BTN_A); assert(face(k, 'B') == DM_BTN_B); assert(face(k, 'X') == DM_BTN_X); assert(face(k, 'Y') == DM_BTN_Y);
    }
    TestPad *g = [TestPad new];   // nothing pressed, buttons nil: none
    assert(dm_pad_face((GCExtendedGamepad *)(id)g) == 0);
    assert(dm_pad_buttons((GCExtendedGamepad *)(id)g) == 0);

    // DM_PAD_LAYOUT=xbox / playstation: non-Nintendo pads follow their printed labels (A/Cross bottom, B/Circle right, X/Square left, Y/Triangle top); Switch stays default
    const char *layouts[] = {"xbox", "playstation"};
    for (int i = 0; i < 2; i++) {
        setenv("DM_PAD_LAYOUT", layouts[i], 1);
        for (NSString *k in @[@"Xbox One", @"DualSense", @"DualShock 4"]) {
            assert(face(k, 'A') == DM_BTN_A); assert(face(k, 'B') == DM_BTN_B); assert(face(k, 'X') == DM_BTN_X); assert(face(k, 'Y') == DM_BTN_Y);
        }
        assert(face(@"Switch Pro Controller", 'A') == DM_BTN_A);
    }
    setenv("DM_PAD_LAYOUT", "switch", 1);
    assert(face(@"DualShock 4", 'A') == DM_BTN_B);

    // Full gamepad extraction + DM_PAD_MAP remapping / unbinding
    setenv("DM_PAD_LAYOUT", "playstation", 1);
    TestPad *ds4 = make_pad(@"DualShock 4");
    ds4.buttonA.isPressed = YES; ds4.leftShoulder.isPressed = YES; ds4.rightTrigger.isPressed = YES;
    ds4.buttonMenu.isPressed = YES; ds4.leftThumbstickButton.isPressed = YES; ds4.rightThumbstickButton.isPressed = YES;
    ds4.dpad.up.isPressed = YES;
    assert(dm_pad_buttons((GCExtendedGamepad *)(id)ds4) == (DM_BTN_A | DM_BTN_L | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_LS | DM_BTN_RS | DM_BTN_UP));

    setenv("DM_PAD_MAP", "A = B, B = A, R = ZR, ZR = R, RS=, bogus=A, L=NOPE", 1);
    assert(dm_pad_buttons((GCExtendedGamepad *)(id)ds4) == (DM_BTN_B | DM_BTN_L | DM_BTN_R | DM_BTN_PLUS | DM_BTN_LS | DM_BTN_UP));

    unsetenv("DM_PAD_LAYOUT"); unsetenv("DM_PAD_MAP");
    printf("t_pad_apple OK\n");
} return 0; }
