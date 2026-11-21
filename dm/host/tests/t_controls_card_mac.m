// The controls card (controls_card_mac.h), built without a game or a window: the built-in map's rows, a controls file's changes (a rebound key, a key doing two things,
// the mouse as the right stick), the card inside a small window, and clicks passing through it to the game's view.
// DM_CONTROLS_CARD_CAPTURE=<folder> also draws the card over a stand-in picture into PNG files there (the pull request's screenshots), in an offscreen window.
#import "../controls_card_mac.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static id keys_of(NSArray<NSArray *> *rows, NSString *label) {
    for (NSArray *r in rows) if ([r[0] isEqualToString:label]) return r[1];
    return nil;
}
static NSView *card_of(NSView *host) { return host.subviews.firstObject; }
// Every text of the card lies inside its insets
static int texts_inside(NSView *card, NSView *v, CGFloat inset) {
    if ([v isKindOfClass:NSTextField.class] && !NSContainsRect(NSInsetRect(card.bounds, inset, inset), [v convertRect:v.bounds toView:card])) return 0;
    for (NSView *s in v.subviews) if (!texts_inside(card, s, inset)) return 0;
    return 1;
}

static void capture(NSView *overlay, NSSize size, const char *dir, const char *name) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(-100000, -100000, size.width, size.height) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    NSView *b = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)]; b.wantsLayer = YES;
    b.layer.backgroundColor = hq_color(0.25, 0.45, 0.35, 1).CGColor;
    w.contentView = b; overlay.frame = b.bounds; [b addSubview:overlay];
    [b layoutSubtreeIfNeeded]; [b displayIfNeeded];
    NSBitmapImageRep *rep = [b bitmapImageRepForCachingDisplayInRect:b.bounds];
    if (!rep) return;
    [b cacheDisplayInRect:b.bounds toBitmapImageRep:rep];
    NSString *path = [NSString stringWithFormat:@"%s/%s", dir, name];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    printf("t_controls_card_mac: wrote %s\n", path.UTF8String);
    [overlay removeFromSuperview];
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        controls c; ctl_defaults(&c);
        NSArray<NSArray *> *rows = cc_rows(&c);
        // Letter names follow the keyboard layout; the test machine's is a QWERTY one, so the built-in map reads as written in controls.h
        CHECK([keys_of(rows, @"Left stick") isEqualToString:@"W  A  S  D"], "the left stick is WASD");
        CHECK([keys_of(rows, @"Right stick") isEqualToString:@"⇧↑  ⇧←  ⇧↓  ⇧→"], "the right stick is Shift and the arrows");
        CHECK([keys_of(rows, @"D-pad") isEqualToString:@"↑  ←  ↓  →"], "the D-pad is the arrows");
        CHECK([keys_of(rows, @"A") isEqualToString:@"J"] && [keys_of(rows, @"B") isEqualToString:@"K"] && [keys_of(rows, @"X") isEqualToString:@"U"] && [keys_of(rows, @"Y") isEqualToString:@"I"], "J K U I are A B X Y");
        CHECK([keys_of(rows, @"ZR") isEqualToString:@"C"] && [keys_of(rows, @"+") isEqualToString:@"Return"] && [keys_of(rows, @"Shake") isEqualToString:@"Space"], "ZR, + and Shake");
        CHECK([keys_of(rows, @"Touch screen") isEqualToString:@"Left click"], "the mouse is the touch screen");
        CHECK(!keys_of(rows, @"SL (left)"), "an unbound rail button has no row");

        char l1[] = "key 49=B", l2[] = "key 38=A+ZR", l3[] = "mouse=right", l4[] = "key 123=Y";
        CHECK(ctl_line(&c, l1) && ctl_line(&c, l2) && ctl_line(&c, l3) && ctl_line(&c, l4), "the file's lines parse");
        rows = cc_rows(&c);
        CHECK([keys_of(rows, @"B") isEqualToString:@"K / Space"], "Space rebound to B joins K");
        CHECK(keys_of(rows, @"A") == NSNull.null, "nothing left on A alone: drawn as a dash");
        CHECK([keys_of(rows, @"A+ZR") isEqualToString:@"J"], "a key doing two things has a row of its own");
        CHECK([keys_of(rows, @"Right stick") isEqualToString:@"⇧↑  ·  ⇧↓  ⇧→ · Mouse"], "a rebound left arrow leaves the right stick's left without Shift+←; the mouse is listed");
        CHECK([keys_of(rows, @"Y") isEqualToString:@"I / ←"], "the left arrow is Y");
        CHECK([keys_of(rows, @"ZR") isEqualToString:@"C / Left click"] && [keys_of(rows, @"ZL") isEqualToString:@"Z / Right click"], "the captured mouse's clicks join their buttons");
        CHECK([keys_of(rows, @"Right stick press") isEqualToString:@"G / Middle click"] && keys_of(rows, @"Capture the mouse") != nil, "the middle click and the capture hint");
        CHECK(!keys_of(rows, @"Touch screen"), "a captured mouse is no touch screen");

        NSView *small = cc_card_view(NSMakeRect(0, 0, 1280, 720), &c);
        [small layoutSubtreeIfNeeded];
        CHECK(NSContainsRect(small.bounds, card_of(small).frame), "the card is inside a 1280x720 window");
        CHECK(texts_inside(card_of(small), card_of(small), 10), "every text is inside the card, away from its edges");
        NSView *tv = cc_card_view(NSMakeRect(0, 0, 1920, 1080), &c); [tv layoutSubtreeIfNeeded];
        CHECK(texts_inside(card_of(tv), card_of(tv), 10) && NSContainsRect(tv.bounds, card_of(tv).frame), "and on a 1080p screen");
        CHECK(NSMaxX(card_of(small).frame) > 1240 && NSMaxY(card_of(small).frame) > 680, "the card is in the top-right corner");
        CHECK([small hitTest:NSMakePoint(NSMidX(card_of(small).frame), NSMidY(card_of(small).frame))] == nil, "a click on the card reaches the game's view");

        const char *dir = getenv("DM_CONTROLS_CARD_CAPTURE");
        if (dir && *dir) {
            controls d; ctl_defaults(&d);
            capture(cc_card_view(NSMakeRect(0, 0, 1280, 720), &d), NSMakeSize(1280, 720), dir, "controls_card_720.png");
            capture(cc_card_view(NSMakeRect(0, 0, 1920, 1080), &c), NSMakeSize(1920, 1080), dir, "controls_card_1080_remapped.png");
        }
    }
    printf("t_controls_card_mac: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
