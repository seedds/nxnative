// Face buttons of the shared GameController adapter (pad_apple.h) with fake pads: by position on most pads, by label on a Nintendo pad; no physical controller is needed.
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
static uint64_t face_ab(NSString *category, char pressed, int ab_xbox) {
    TestPad *g = [TestPad new]; g.controller = [TestController new]; g.controller.productCategory = category;
    g.buttonA = [TestButton new]; g.buttonB = [TestButton new]; g.buttonX = [TestButton new]; g.buttonY = [TestButton new];
    TestButton *b = pressed == 'A' ? g.buttonA : pressed == 'B' ? g.buttonB : pressed == 'X' ? g.buttonX : g.buttonY;
    b.isPressed = YES;
    return dm_pad_face_ab((GCExtendedGamepad *)(id)g, ab_xbox);
}
static uint64_t face(NSString *category, char pressed) {
    return face_ab(category, pressed, 0);
}

int main(void) { @autoreleasepool {
    for (NSString *k in @[@"Xbox One", @"DualSense", @"HID"]) {   // by position: buttonA bottom (B), buttonB right (A), buttonX left (Y), buttonY top (X)
        assert(face(k, 'A') == DM_BTN_B); assert(face(k, 'B') == DM_BTN_A); assert(face(k, 'X') == DM_BTN_Y); assert(face(k, 'Y') == DM_BTN_X);
    }
    assert(face(nil, 'B') == DM_BTN_A);   // no category: by position
    for (NSString *k in @[@"Switch Pro Controller", @"Nintendo Switch Joy-Con (L/R)", @"Nintendo Switch Joy-Con (R)"]) {   // by label
        assert(face(k, 'A') == DM_BTN_A); assert(face(k, 'B') == DM_BTN_B); assert(face(k, 'X') == DM_BTN_X); assert(face(k, 'Y') == DM_BTN_Y);
    }
    // With DM_PAD_AB=xbox (ab_xbox=1): A is bottom, B is right (Xbox / PlayStation layout)
    for (NSString *k in @[@"Xbox One", @"DualSense", @"HID"]) {
        assert(face_ab(k, 'A', 1) == DM_BTN_A); assert(face_ab(k, 'B', 1) == DM_BTN_B); assert(face_ab(k, 'X', 1) == DM_BTN_Y); assert(face_ab(k, 'Y', 1) == DM_BTN_X);
    }
    for (NSString *k in @[@"Switch Pro Controller", @"Nintendo Switch Joy-Con (L/R)", @"Nintendo Switch Joy-Con (R)"]) {
        assert(face_ab(k, 'A', 1) == DM_BTN_B); assert(face_ab(k, 'B', 1) == DM_BTN_A); assert(face_ab(k, 'X', 1) == DM_BTN_X); assert(face_ab(k, 'Y', 1) == DM_BTN_Y);
    }
    TestPad *g = [TestPad new];   // nothing pressed, buttons nil: none
    assert(dm_pad_face((GCExtendedGamepad *)(id)g) == 0);
    printf("t_pad_apple OK\n");
} return 0; }
