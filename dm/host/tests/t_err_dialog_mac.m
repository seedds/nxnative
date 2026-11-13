// The error applet's dialog (err_dialog_mac.h), built without a game: the code and the message are its texts, OK and Quit its buttons, and text that is missing or not UTF-8
// shows as nothing instead of failing. DM_ERR_DIALOG_CAPTURE=<folder> also brings it up as the game would (a modal alert of a regular app), draws its window as it is on
// screen into err_dialog.png there, and closes it with each button in turn: OK and Quit are what runModal answers. The window is drawn by the process itself, not with
// screencapture, which needs the screen recording permission and without it gave a blank picture
#import "../err_dialog_mac.h"
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

// The window as it is on screen, over the window background colour: the alert's glass background is transparent in the picture
static int draw_window(NSWindow *w, const char *path) {
    NSView *v = w.contentView.superview ?: w.contentView;   // the frame view: the whole window, title area included
    CGFloat k = w.backingScaleFactor;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)(v.bounds.size.width * k) pixelsHigh:(NSInteger)(v.bounds.size.height * k)
                                                              bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    if (!w.isVisible || !v.layer || !rep) return 0;
    CGContextRef c = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep].CGContext;
    CGContextScaleCTM(c, k, k);
    [w.effectiveAppearance performAsCurrentDrawingAppearance:^{ CGContextSetFillColorWithColor(c, NSColor.windowBackgroundColor.CGColor); }];
    CGContextFillRect(c, v.bounds);
    [v.layer renderInContext:c];
    int ok = [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES];
    printf("t_err_dialog_mac: %s %s\n", ok ? "wrote" : "could not write", path);
    return ok;
}
// Bring the alert up modally; after a second capture it (when path is set) and press button
static NSModalResponse run_and_press(NSAlert *a, NSInteger button, const char *path) {
    __block int captured = 0;
    NSTimer *t = [NSTimer timerWithTimeInterval:1.0 repeats:NO block:^(NSTimer *x) {
        (void)x;
        if (path) captured = draw_window(a.window, path);
        [a.buttons[button] performClick:nil];
    }];
    [NSRunLoop.mainRunLoop addTimer:t forMode:NSRunLoopCommonModes];
    NSModalResponse r = [a runModal];
    if (path) CHECK(captured, "the dialog was captured");
    return r;
}

int main(void) {
    @autoreleasepool {
        NSAlert *a = err_dialog_alert("Error code 2162-0001", "Save data is damaged.\n\n\xE5\x90\x8D %s%n", @"Game");
        CHECK([a.messageText isEqualToString:@"Error code 2162-0001"], "the code is the title");
        CHECK([a.informativeText isEqualToString:@"Save data is damaged.\n\n名 %s%n"], "the message is the text, as given");
        CHECK(a.buttons.count == 2 && [a.buttons[0].title isEqualToString:@"OK"] && [a.buttons[1].title isEqualToString:@"Quit Game"], "OK and Quit");
        NSAlert *b = err_dialog_alert(NULL, "\xC3\x28", @"Game");
        CHECK([b.messageText isEqualToString:@""] && [b.informativeText isEqualToString:@""], "a missing title and text that is not UTF-8 show as nothing");

        const char *dir = getenv("DM_ERR_DIALOG_CAPTURE");
        if (dir && *dir) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];
            char path[1024]; snprintf(path, sizeof path, "%s/err_dialog.png", dir);
            CHECK(run_and_press(a, 0, path) == ERR_DIALOG_OK, "OK closes it with ERR_DIALOG_OK");
            NSAlert *q = err_dialog_alert("Error code 2162-0001", "Save data is damaged.", @"Game");
            CHECK(run_and_press(q, 1, NULL) == ERR_DIALOG_QUIT, "Quit closes it with ERR_DIALOG_QUIT");
        }
    }
    printf("t_err_dialog_mac: %d of %d checks failed\n", fails, checks);
    return fails != 0;
}
