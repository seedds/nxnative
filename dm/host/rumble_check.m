// rumble_check: drives the host's controller vibration output (haptics_apple.m, hle_haptics.c) without a game, to tell a host-side haptics failure from a game
// that sends no vibration. It opens a small window (GameController and Core Haptics serve the frontmost app only), attaches the first controller to player 1
// as the Mac front end does, and plays a fixed sequence as a game would send it: both motors, the left one, the right one, then a fade, values sent
// every 5 ms. Built and run by scripts/rumble_check.sh. Exit status: 0 after the sequence played, 1 when no controller connected within 15 s.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include "haptics_apple.h"
#include <stdio.h>

static void hold(float low, float high, int left, int right, double seconds) {
    dm_vibration on = {low, 160, high, 320}, off = {0, 160, 0, 320};
    for (double t = 0; t < seconds; t += 0.005) {
        dm_vibration_update u[2] = {{0, 0, left ? on : off}, {0, 1, right ? on : off}};
        dm_haptics_send(u, 2);
        [NSThread sleepForTimeInterval:0.005];
    }
}

static void step(const char *what, float low, float high, int left, int right, double seconds) {
    printf("  %-40s", what); fflush(stdout);
    hold(low, high, left, right, seconds);
    hold(0, 0, 1, 1, 0.6);   // a pause, so each step is felt on its own
    printf("done\n");
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 90) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        w.title = @"rumble_check: keep this window in front";
        [w center]; [w makeKeyAndOrderFront:nil];
        [NSApp finishLaunching]; [NSApp activateIgnoringOtherApps:YES];
        GCController.shouldMonitorBackgroundEvents = NO;
        dm_haptics_apple_init();
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
        GCController *pad = nil;
        for (int i = 0; i < 150 && !(pad = GCController.controllers.firstObject); i++)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        if (!pad) { fprintf(stderr, "[RED] no controller connected within 15 s (GameController lists none)\n"); return 1; }
        GCDeviceHaptics *h = pad.haptics;
        printf("controller: %s (%s), haptics %s, localities %s\n", pad.vendorName.UTF8String ?: "?", pad.productCategory.UTF8String ?: "?",
               h ? "yes" : "NO (GameController offers none for it)", h ? [[h.supportedLocalities.allObjects componentsJoinedByString:@", "] UTF8String] : "-");
        dm_haptics_apple_controller(0, pad);
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];   // the engine starts on the haptics queue
        printf("routed outputs: %#x (bit 0 = player 1; 0 means Core Haptics opened no channel on it). Playing, about 10 s:\n", dm_haptics_available());
        step("1. both sides, strong (0.8), 1.5 s", 0.8f, 0.8f, 1, 1, 1.5);
        step("2. left only (0.8), 1.5 s", 0.8f, 0.8f, 1, 0, 1.5);
        step("3. right only (0.8), 1.5 s", 0.8f, 0.8f, 0, 1, 1.5);
        step("4. both, weak (0.3), 1.5 s", 0.3f, 0.3f, 1, 1, 1.5);
        printf("  5. fade 1.0 -> 0, 2 s                  "); fflush(stdout);
        for (int i = 200; i >= 0; i--) hold(i / 200.0f, i / 200.0f, 1, 1, 0.01);
        printf("done\n");
        dm_haptics_apple_shutdown();
        printf("finished: report which steps you felt (none / all / some)\n");
    }
    return 0;
}
