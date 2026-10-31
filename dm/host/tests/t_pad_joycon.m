// A Joy-Con pair as one pad (pad_joycon.h) with fake halves: which half a controller is by its names, and the state a pair, or one half alone,
// reports as a standard pad; no physical controller is needed.
#import "../pad_joycon.h"
#include <assert.h>
#include <stdio.h>

@interface TestButton : NSObject
@property BOOL isPressed;
@end
@implementation TestButton
@end
@interface TestAxis : NSObject
@property float value;
@end
@implementation TestAxis
@end
@interface TestStick : NSObject
@property TestAxis *xAxis, *yAxis;
@end
@implementation TestStick
@end
@interface TestDpad : NSObject
@property TestButton *left, *up, *right, *down;
@end
@implementation TestDpad
@end
@interface TestController : NSObject
@property NSString *productCategory, *vendorName;
@end
@implementation TestController
@end
@interface TestPad : NSObject
@property TestController *controller;
@property TestButton *buttonA, *buttonB, *buttonX, *buttonY;
@property TestButton *leftShoulder, *rightShoulder, *leftTrigger, *rightTrigger, *buttonMenu, *buttonOptions;
@property TestButton *leftThumbstickButton, *rightThumbstickButton;
@property TestStick *leftThumbstick, *rightThumbstick;
@property TestDpad *dpad;
@end
@implementation TestPad
@end

// A pad with every element a Joy-Con reports, none pressed, its stick centred
static TestPad *pad(NSString *category) {
    TestPad *g = [TestPad new];
    g.controller = [TestController new]; g.controller.productCategory = category;
    for (NSString *k in @[ @"buttonA", @"buttonB", @"buttonX", @"buttonY", @"leftShoulder", @"rightShoulder", @"leftTrigger", @"rightTrigger",
                           @"buttonMenu", @"buttonOptions", @"leftThumbstickButton", @"rightThumbstickButton" ])
        [g setValue:[TestButton new] forKey:k];
    TestStick *l = [TestStick new], *r = [TestStick new];
    l.xAxis = [TestAxis new]; l.yAxis = [TestAxis new]; r.xAxis = [TestAxis new]; r.yAxis = [TestAxis new];
    g.leftThumbstick = l; g.rightThumbstick = r;
    g.dpad = [TestDpad new];
    g.dpad.left = [TestButton new]; g.dpad.up = [TestButton new]; g.dpad.right = [TestButton new]; g.dpad.down = [TestButton new];
    return g;
}
static GCExtendedGamepad *p(TestPad *g) { return (GCExtendedGamepad *)(id)g; }

int main(void) { @autoreleasepool {
    // Which half a controller is: macOS names each Joy-Con by its side, GameController's productCategory with "Nintendo Switch" in front
    assert(dm_joycon_side(@"Nintendo Switch Joy-Con (L)", @"Joy-Con (L)") == DM_JOY_L);
    assert(dm_joycon_side(@"Nintendo Switch Joy-Con (R)", @"Joy-Con (R)") == DM_JOY_R);
    assert(dm_joycon_side(@"Nintendo Switch Joy-Con (L/R)", @"Joy-Con (L/R)") == DM_JOY_WHOLE);   // iPadOS hands a pair over as one controller
    assert(dm_joycon_side(@"joy-con (r)", nil) == DM_JOY_R);                                      // case and a missing name
    assert(dm_joycon_side(@"", @"Joy-Con (L)") == DM_JOY_L);                                      // only macOS's own device name
    assert(dm_joycon_side(@"Joy-Con", nil) == DM_JOY_NONE);                                       // no side: used as it is
    assert(dm_joycon_side(@"Switch Pro Controller", @"Pro Controller") == DM_JOY_NONE);
    assert(dm_joycon_side(@"DualSense Wireless Controller", nil) == DM_JOY_NONE);
    assert(dm_joycon_side(nil, nil) == DM_JOY_NONE);

    // The left half: d-pad, left stick, L / ZL, − and the pressed stick; no face buttons of its own
    TestPad *l = pad(@"Nintendo Switch Joy-Con (L)"), *r = pad(@"Nintendo Switch Joy-Con (R)");
    l.dpad.left.isPressed = l.dpad.up.isPressed = l.dpad.right.isPressed = l.dpad.down.isPressed = YES;
    l.leftShoulder.isPressed = l.leftTrigger.isPressed = l.buttonOptions.isPressed = l.leftThumbstickButton.isPressed = YES;
    dm_joy_state s = dm_joy_pair_state(p(l), nil);
    assert(s.buttons == (DM_BTN_LEFT | DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN | DM_BTN_L | DM_BTN_ZL | DM_BTN_MINUS | DM_BTN_LS));
    assert(s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0);

    // The right half: X A B Y by their labels, R / ZR, + and the pressed stick
    r.buttonA.isPressed = r.buttonB.isPressed = r.buttonX.isPressed = r.buttonY.isPressed = YES;
    r.rightShoulder.isPressed = r.rightTrigger.isPressed = r.buttonMenu.isPressed = r.rightThumbstickButton.isPressed = YES;
    s = dm_joy_pair_state(nil, p(r));
    assert(s.buttons == (DM_BTN_A | DM_BTN_B | DM_BTN_X | DM_BTN_Y | DM_BTN_R | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_RS));

    // A pair is one pad: both halves' buttons at once, each half's stick on its own side, y up as GameController has it
    l.leftThumbstick.xAxis.value = 0.5f; l.leftThumbstick.yAxis.value = 0.25f;
    r.rightThumbstick.xAxis.value = -0.5f; r.rightThumbstick.yAxis.value = 0.75f;
    s = dm_joy_pair_state(p(l), p(r));
    assert(s.buttons == (DM_BTN_LEFT | DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN | DM_BTN_L | DM_BTN_ZL | DM_BTN_MINUS | DM_BTN_LS |
                         DM_BTN_A | DM_BTN_B | DM_BTN_X | DM_BTN_Y | DM_BTN_R | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_RS));
    assert(s.lx == 0.5f && s.ly == 0.25f && s.rx == -0.5f && s.ry == 0.75f);

    // A lone half reads as what it has: nothing from the half that is not there
    s = dm_joy_pair_state(nil, nil);
    assert(s.buttons == 0 && s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0);

    // A Joy-Con has one stick: a driver that reports it on the other stick still plays (whichever is deflected more is taken, the left one on a tie)
    TestPad *one = pad(@"Nintendo Switch Joy-Con (R)");
    one.rightThumbstick.xAxis.value = 0.5f;
    s = dm_joy_pair_state(nil, p(one));
    assert(s.rx == 0.5f && s.ry == 0);
    TestPad *both = pad(@"Nintendo Switch Joy-Con (L)");   // reported on both sticks, one of them further out: the further one
    both.leftThumbstick.xAxis.value = 0.25f; both.rightThumbstick.xAxis.value = 0.5f;
    s = dm_joy_pair_state(p(both), nil);
    assert(s.lx == 0.5f);
    both.rightThumbstick.xAxis.value = -0.25f;   // a tie: a controller's own stick is the left one
    assert(dm_joy_pair_state(p(both), nil).lx == 0.25f);

    // A pad without the extra buttons (nil) presses nothing
    TestPad *bare = pad(@"Nintendo Switch Joy-Con (R)");
    bare.buttonOptions = nil; bare.rightThumbstickButton = nil;
    assert(dm_joy_pair_state(nil, p(bare)).buttons == 0);
    printf("t_pad_joycon OK\n");
} return 0; }
