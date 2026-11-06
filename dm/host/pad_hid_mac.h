// Mac: raw HID gamepads that GameController does not list (pad_hid.h, GitHub issue #637), read on the main run loop alongside front_mac.m's GameController players.
// Each device takes a player slot as a GameController controller does; front_mac.m defines the slot side (hidpad_attach / hidpad_detach / hidpad_read).
// Skipped, so no controller is read twice: GameController's synthetic devices, and a device GameController lists itself. +[GCController supportsHIDDevice:] is only a hint for
// that: it answers YES for the 8BitDo Ultimate 2C (2dc8:301b), which GameController then never lists (GitHub issue #965). So a device it says YES for waits HIDPAD_GC_WAIT
// seconds for a GCController of its name; without one it is read as HID, and dropped again if that GCController connects later. DM_HID_PADS=0 turns this off.
// A device GameController lists is kept, unread, in g_hidpads_gc: identical pads (one model, one name) can outnumber the controllers GameController lists for them, as two
// Pro Controllers over USB do (GitHub issue #1218), and front_mac.m then reads every pad of that model here, each a player of its own (twins_eval, DM_PAD_IDENTICAL).
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GameController.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "pad_hid.h"
#include "pad_pro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hid_pad {
    IOHIDDeviceRef hid; pad_hid st; char name[64]; int vid, pid, gc_hint; struct hid_pad *next;
    int twin, twin_want;   // twin: read here although GameController lists its model, as it lists fewer pads of it than IOKit has; twin_want: front_mac.m's choice for it
    int is_pro, pro_logged, pro_counter; pad_pro pro; uint64_t pro_m; int32_t pro_s[4]; pro_motion motion;   // a Pro Controller read from its report 0x30 (front_mac.m, pro_raw_sync), not from st
} hid_pad;
static IOHIDManagerRef g_hidpad_manager;
static hid_pad *g_hidpads, *g_hidpads_waiting;   // waiting: GameController says it lists the device (gc_hint); read as HID if it has not after HIDPAD_GC_WAIT
static hid_pad *g_hidpads_gc;                    // GameController lists a controller of the device's name: not read here, kept to count identical pads
static void (*g_hidpad_changed)(void);           // front_mac.m's, when g_hidpads_gc changes
#define HIDPAD_GC_WAIT 2.0
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
static hid_pad *hidpad_unlink(hid_pad **list, IOHIDDeviceRef hid) {
    while (*list && (*list)->hid != hid) list = &(*list)->next;
    hid_pad *d = *list; if (d) *list = d->next; return d;
}
// Whether GameController lists a controller of this name (its vendorName is the HID product string). A second pad of the same model counts as listed with the first
static int hidpad_gc_listed(const char *name) { for (GCController *c in GCController.controllers) if (c.vendorName && !strcmp(c.vendorName.UTF8String, name)) return 1; return 0; }
static void hidpad_drop(hid_pad *d) { IOHIDDeviceRegisterInputValueCallback(d->hid, NULL, NULL); CFRelease(d->hid); free(d); }
static void hidpad_keep_gc(hid_pad *d, int notify) {
    IOHIDDeviceRegisterInputValueCallback(d->hid, NULL, NULL);
    d->twin = d->twin_want = 0; d->next = g_hidpads_gc; g_hidpads_gc = d;
    if (notify && g_hidpad_changed) g_hidpad_changed();
}
// A Switch Pro Controller has its own reader for identical pads (report 0x30: pad_pro.h, front_mac.m's pro_raw_sync): HID elements do not carry its sticks
static int hidpad_own_reader(const hid_pad *d) { return d->vid == 0x057e && d->pid == 0x2009; }
static void hidpad_use(hid_pad *d) {
    d->next = g_hidpads; g_hidpads = d;
    IOHIDDeviceRegisterInputValueCallback(d->hid, hidpad_value, d);
    hidpad_attach(d);
    // HID has no standard button numbering: the default map is 8BitDo's, so another vendor's pad may have its buttons in other places
    if (d->vid != PAD_HID_DEFAULT_VENDOR && !getenv("DM_HID_MAP"))
        LOG("controller '%s' (HID, %04x:%04x): buttons use 8BitDo's numbering; if they are in the wrong places, DM_HID_LOG=1 logs each button's number for a DM_HID_MAP", d->name, d->vid, d->pid);
}
static void hidpad_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (hid_pad *d = g_hidpads; d; d = d->next) if (d->hid == hid) return;
    for (hid_pad *d = g_hidpads_waiting; d; d = d->next) if (d->hid == hid) return;
    hid_pad *d = calloc(1, sizeof *d); if (!d) { LOG("[RED] HID gamepad: cannot allocate device state"); return; }
    pad_hid_init(&d->st, getenv("DM_HID_MAP"));
    NSString *name = (__bridge NSString *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductKey));
    snprintf(d->name, sizeof d->name, "%s", name.UTF8String ?: "HID gamepad");
    d->vid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey)) intValue];
    d->pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey)) intValue];
    NSArray *els = CFBridgingRelease(IOHIDDeviceCopyMatchingElements(hid, NULL, kIOHIDOptionsTypeNone));
    for (id o in els) { IOHIDElementRef e = (__bridge IOHIDElementRef)o; int a = pad_hid_axis_of(IOHIDElementGetUsagePage(e), IOHIDElementGetUsage(e)); if (a >= 0) d->st.has[a] = 1; }
    d->hid = (IOHIDDeviceRef)CFRetain(hid);
    d->gc_hint = [GCController supportsHIDDevice:hid];   // once per device: the call is not cheap
    if (!d->gc_hint) { hidpad_use(d); return; }
    if (hidpad_gc_listed(d->name)) { hidpad_keep_gc(d, 1); return; }   // front_mac.m reads it through its GCController
    d->next = g_hidpads_waiting; g_hidpads_waiting = d;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(HIDPAD_GC_WAIT * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        hid_pad *w = hidpad_unlink(&g_hidpads_waiting, hid); if (!w) return;   // removed meanwhile
        if (hidpad_gc_listed(w->name)) { hidpad_keep_gc(w, 1); return; }
        LOG("controller '%s' (HID, %04x:%04x): GameController says it lists it but has not after %g s, so it is read as HID", w->name, w->vid, w->pid, HIDPAD_GC_WAIT);
        hidpad_use(w);
    });
}
static void hidpad_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    hid_pad *d = hidpad_unlink(&g_hidpads, hid); int counted = d && d->twin;
    if (d) hidpad_detach(d); else d = hidpad_unlink(&g_hidpads_waiting, hid);
    if (!d && (d = hidpad_unlink(&g_hidpads_gc, hid))) counted = 1;
    if (d) hidpad_drop(d);
    if (counted && g_hidpad_changed) g_hidpad_changed();   // one pad fewer of its model
}
// A GCController connected (front_mac.m): a HID pad of its name that GameController had only promised is read through it from now on.
// ponytail: matched by name, so two pads of one model that both fell back to HID are both dropped when GameController lists one; a per-device link would need an API GameController does not give
static void hidpad_gc_connected(GCController *c) {
    const char *name = c.vendorName.UTF8String; if (!name) return;
    for (hid_pad **link = &g_hidpads; *link;) {
        hid_pad *d = *link; if (!d->gc_hint || d->twin || strcmp(d->name, name)) { link = &d->next; continue; }   // twins: front_mac.m's twins_eval decides
        *link = d->next; LOG("controller '%s' (HID) is now a GameController controller: read through it", d->name); hidpad_detach(d); hidpad_keep_gc(d, 0);
    }
}
// front_mac.m's twins: a device GameController lists, read here from now on, or given back to it
static void hidpad_twin_read(hid_pad *d) {   // d is already off g_hidpads_gc
    d->twin = 1; d->next = g_hidpads; g_hidpads = d;
    uint8_t has[PH_AXES]; memcpy(has, d->st.has, sizeof has);   // the axes hidpad_found saw
    pad_hid_init(&d->st, getenv("DM_HID_MAP")); memcpy(d->st.has, has, sizeof has);   // nothing held from an earlier turn
    IOHIDDeviceRegisterInputValueCallback(d->hid, hidpad_value, d);
    hidpad_attach(d);
}
static void hidpad_twin_release(hid_pad *d) { hidpad_detach(d); hidpad_keep_gc(d, 0); }   // d is already off g_hidpads
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
