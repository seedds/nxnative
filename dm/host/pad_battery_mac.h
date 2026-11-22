// A controller's battery from GameController, and the low-battery notice drawn over the game by front_mac.m, with the look of the Home hold indicator
// (home_quit_mac.h). GameController reports a battery for DualShock 4, DualSense, Xbox and Switch Pro Controllers; raw HID and Switch 2 pads have none here.
#pragma once
#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#include <math.h>
#include "home_quit_mac.h"
#include "pad_battery.h"

// Whole percent, -1 when the controller reports none or its state is unknown; *charging: plugged in (charging or full)
static int pad_battery_read(GCController *c, int *charging) {
    GCDeviceBattery *b = c.battery;
    *charging = 0;
    if (!b || b.batteryState == GCDeviceBatteryStateUnknown) return -1;
    *charging = b.batteryState != GCDeviceBatteryStateDischarging;
    return (int)lroundf(b.batteryLevel * 100);
}
// "84 %", "84 %, charging", "100 %, full" or "unknown"
static NSString *pad_battery_text(GCController *c) {
    int charging, pct = pad_battery_read(c, &charging);
    if (pct < 0) return @"unknown";
    if (!charging) return [NSString stringWithFormat:@"%d %%", pct];
    return [NSString stringWithFormat:c.battery.batteryState == GCDeviceBatteryStateFull ? @"%d %%, full" : @"%d %%, charging", pct];
}
// The notice: a pill near the bottom of the view (the Home hold indicator is at the top) with a battery symbol, red at the very low threshold
static NSView *pad_battery_view(NSRect bounds, NSString *text, int very_low) {
    CGFloat k = hq_scale(bounds.size);
    NSColor *tint = very_low ? NSColor.systemRedColor : HQ_CREAM;
    NSImageView *icon = hq_glyph(very_low ? @"battery.0percent" : @"battery.25percent", @"battery.25", 22 * k, tint);
    NSStackView *pill = [NSStackView stackViewWithViews:@[icon, hq_label(text, 20 * k, NSFontWeightSemibold, [NSColor colorWithWhite:1 alpha:0.9], 0)]];
    pill.spacing = 12 * k; pill.edgeInsets = NSEdgeInsetsMake(0, 20 * k, 0, 24 * k); pill.translatesAutoresizingMaskIntoConstraints = NO;
    pill.wantsLayer = YES; pill.layer.backgroundColor = [HQ_BACKGROUND colorWithAlphaComponent:0.92].CGColor; pill.layer.cornerRadius = 28 * k;
    pill.layer.borderWidth = 1; pill.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.12].CGColor;
    NSView *host = [[NSView alloc] initWithFrame:bounds]; host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [host addSubview:pill];
    [NSLayoutConstraint activateConstraints:@[[pill.centerXAnchor constraintEqualToAnchor:host.centerXAnchor],
        [pill.bottomAnchor constraintEqualToAnchor:host.bottomAnchor constant:-40 * k], [pill.heightAnchor constraintEqualToConstant:56 * k]]];
    return host;
}
