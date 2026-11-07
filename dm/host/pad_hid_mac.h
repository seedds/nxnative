// Mac: raw HID gamepads that GameController does not list (pad_hid.h, GitHub issue #637), read on the main run loop alongside front_mac.m's GameController players.
// Each device takes a player slot as a GameController controller does; front_mac.m defines the slot side (hidpad_attach / hidpad_detach / hidpad_read).
// Skipped, so no controller is read twice: GameController's synthetic devices, and every device GameController says it will list itself (+[GCController supportsHIDDevice:]:
// Xbox, PlayStation, Switch and MFi pads of any vendor). A device GameController claims gets 1.5 s for its GCController to appear; one that never comes (an 8BitDo Ultimate 2C
// Wireless over Bluetooth on macOS 27: supportsHIDDevice: says YES, GCController.controllers stays empty) is read raw after all, so the pad is not dead in the game.
// DM_HID_CLAIMED=raw reads a claimed device raw at once, DM_HID_CLAIMED=gc never (the behaviour before). DM_HID_PADS=0 turns raw pads off.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "pad_hid.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct hid_pad { IOHIDDeviceRef hid; pad_hid st; char name[64]; struct hid_pad *next; } hid_pad;
static IOHIDManagerRef g_hidpad_manager;
static hid_pad *g_hidpads;
static int g_hidpad_log;
static void hidpad_attach(hid_pad *d);
static void hidpad_detach(hid_pad *d);
static void hidpad_read(hid_pad *d);

static void hidpad_value(void *ctx, IOReturn result, void *sender, IOHIDValueRef v) {
    (void)sender;
    hid_pad *d = ctx; if (result != kIOReturnSuccess) return;
    IOHIDElementRef e = IOHIDValueGetElement(v);
    uint32_t page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e);
    CFIndex x = IOHIDValueGetIntegerValue(v), min = IOHIDElementGetLogicalMin(e), max = IOHIDElementGetLogicalMax(e);
    if (page == kHIDPage_Button && usage >= 1 && usage <= 32) {
        uint32_t bit = 1u << (usage - 1), was = d->st.buttons & bit;
        d->st.buttons = x ? d->st.buttons | bit : d->st.buttons & ~bit;
        if (g_hidpad_log && x && !was) LOG("[hid] %s: button usage %u", d->name, usage);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch) {
        d->st.hat = pad_hid_hat(x, min, max);
    } else {
        int a = pad_hid_axis_of(page, usage); if (a < 0) return;
        d->st.axis[a] = pad_hid_norm(x, min, max);
    }
    hidpad_read(d);
}
// Reads one device raw: a player slot, its axes, its input callback (hidpad_found's checks are done)
static void hidpad_read_raw(IOHIDDeviceRef hid) {
    NSNumber *vendor = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey)), *product = (__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey));
    int vid = vendor.intValue, pid = product.intValue;
    hid_pad *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] HID gamepad: cannot allocate device state"); return; }
    pad_hid_init(&d->st, getenv("DM_HID_MAP"));
    NSString *name = (__bridge NSString *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductKey));
    snprintf(d->name, sizeof d->name, "%s", name.UTF8String ?: "HID gamepad");
    NSArray *els = CFBridgingRelease(IOHIDDeviceCopyMatchingElements(hid, NULL, kIOHIDOptionsTypeNone));
    for (id o in els) { IOHIDElementRef e = (__bridge IOHIDElementRef)o; int a = pad_hid_axis_of(IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e)); if (a >= 0) d->st.has[a] = 1; }
    d->hid = (IOHIDDeviceRef)CFRetain(hid); d->next = g_hidpads; g_hidpads = d;
    IOHIDDeviceRegisterInputValueCallback(hid, hidpad_value, d);
    hidpad_attach(d);
    // HID has no standard button numbering: the default map is 8BitDo's, so another vendor's pad may have its buttons in other places
    if (vid != PAD_HID_DEFAULT_VENDOR && !getenv("DM_HID_MAP"))
        LOG("controller '%s' (HID, %04x:%04x): buttons use 8BitDo's numbering; if they are in the wrong places, DM_HID_LOG=1 logs each button's number for a DM_HID_MAP", d->name, vid, pid);
}
// Whether GameController delivered a controller for the device: its vendorName is the device's product string
static BOOL hidpad_delivered(IOHIDDeviceRef hid) {
    NSString *name = (__bridge NSString *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductKey));
    for (GCController *c in GCController.controllers) if (c.extendedGamepad && name && [c.vendorName isEqualToString:name]) return YES;
    return NO;
}
#define HIDPAD_CLAIMED_MAX 8
static IOHIDDeviceRef g_hidpad_claimed[HIDPAD_CLAIMED_MAX];   // claimed devices waiting for their GCController; cleared when it comes, when the wait ends or when the device goes
static void hidpad_claimed_forget(IOHIDDeviceRef hid) { for (int i = 0; i < HIDPAD_CLAIMED_MAX; i++) if (g_hidpad_claimed[i] == hid) g_hidpad_claimed[i] = NULL; }
static int hidpad_claimed_waiting(IOHIDDeviceRef hid) { for (int i = 0; i < HIDPAD_CLAIMED_MAX; i++) if (g_hidpad_claimed[i] == hid) return 1; return 0; }
// A device GameController claims (supportsHIDDevice:) is normally read through its GCController. GameController claims some pads it then never delivers (an 8BitDo Ultimate 2C
// Wireless over Bluetooth on macOS 27), which left them dead: such a device gets 1.5 s, then is read raw if no controller of its name came. DM_HID_CLAIMED=raw: raw at once; gc: never raw
static void hidpad_claimed(IOHIDDeviceRef hid) {
    const char *e = getenv("DM_HID_CLAIMED");
    if (e && !strcmp(e, "gc")) return;
    if ((e && !strcmp(e, "raw")) && !hidpad_delivered(hid)) { hidpad_read_raw(hid); return; }
    if (hidpad_delivered(hid) || hidpad_claimed_waiting(hid)) return;
    int slot = -1; for (int i = 0; i < HIDPAD_CLAIMED_MAX && slot < 0; i++) if (!g_hidpad_claimed[i]) slot = i;
    if (slot < 0) return;
    g_hidpad_claimed[slot] = hid; CFRetain(hid);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (hidpad_claimed_waiting(hid)) {   // still connected, and not read meanwhile
            hidpad_claimed_forget(hid);
            if (!hidpad_delivered(hid)) {
                NSString *name = (__bridge NSString *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductKey));
                LOG("controller '%s': GameController claims it but delivered no controller in 1.5 s; read as a raw HID pad (DM_HID_CLAIMED=gc leaves it to GameController)", name.UTF8String ?: "HID gamepad");
                hidpad_read_raw(hid);
            }
        }
        CFRelease(hid);
    });
}
static void hidpad_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (hid_pad *d = g_hidpads; d; d = d->next) if (d->hid == hid) return;
    if ([GCController supportsHIDDevice:hid]) { hidpad_claimed(hid); return; }   // front_mac.m reads it through its GCController (once per device: the call is not cheap), unless none comes
    hidpad_read_raw(hid);
}
static void hidpad_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    hidpad_claimed_forget(hid);   // a claimed device that went before its wait ended: the block then only releases it
    hid_pad **link = &g_hidpads;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    hid_pad *d = *link; *link = d->next;
    IOHIDDeviceRegisterInputValueCallback(hid, NULL, NULL);
    hidpad_detach(d);
    CFRelease(d->hid); free(d);
}
static void hidpad_start(void) {
    const char *e = getenv("DM_HID_PADS"); if (e && !atoi(e)) return;
    e = getenv("DM_HID_LOG"); g_hidpad_log = e && atoi(e);
    g_hidpad_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone); if (!g_hidpad_manager) return;
    IOHIDManagerSetDeviceMatchingMultiple(g_hidpad_manager, (__bridge CFArrayRef)@[
        @{@kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad)},
        @{@kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_Joystick)}]);
    IOHIDManagerRegisterDeviceMatchingCallback(g_hidpad_manager, hidpad_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_hidpad_manager, hidpad_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_hidpad_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_hidpad_manager, kIOHIDOptionsTypeNone);   // nonexclusive: GameController and other apps keep their devices
    if (r != kIOReturnSuccess) {
        LOG("[RED] HID gamepads: cannot open HID (0x%x); only GameController controllers are read", r);
        IOHIDManagerUnscheduleFromRunLoop(g_hidpad_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_hidpad_manager); g_hidpad_manager = NULL;
    }
}
