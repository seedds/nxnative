// Mac: the 8BitDo Ultimate 2 Wireless's motion from its HID reports (motion_8bitdo.h), for the player GameController gives the controller. Its HID device is the first one
// no other player reads, found in the registry at attach as haptics_apple.m finds a DualShock 4's: GameController does not name a controller's HID device. Opened
// nonexclusively and read on the main run loop; GameController keeps its buttons. DM_MOTION=0 / DM_MOTION_HID=0 leave it off, as motion_pro_mac.h
#pragma once
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#include <stdlib.h>
#include <string.h>
#include "motion_8bitdo.h"
#include "dm_input.h"

typedef struct { IOHIDDeviceRef dev; uint64_t id; const void *owner; int player; uint8_t report[64]; } abitdo_in;
static abitdo_in g_abitdo[DM_PLAYERS];

static void abitdo_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender; (void)report_id;
    abitdo_in *d = ctx; float acc[3], rot[3];
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || len < 0 || !d->dev || !abitdo_motion_decode(r, (size_t)len, acc, rot)) return;
    dm_motion_set_hid(d->player, acc, rot, now_ns());
}
static void abitdo_motion_attach(GCController *c, int player) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    e = getenv("DM_MOTION_HID"); if (e && !atoi(e)) return;
    if (player < 0 || player >= DM_PLAYERS || g_abitdo[player].dev || !c.vendorName || strcmp(c.vendorName.UTF8String, ABITDO_U2W_NAME)) return;
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey); io_iterator_t it = 0;
    if (!match) return;
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), (__bridge CFNumberRef)@(ABITDO_VID));
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), (__bridge CFNumberRef)@(ABITDO_U2W_BT));
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return;   // takes match
    abitdo_in *d = &g_abitdo[player];
    for (io_service_t s; !d->dev && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        uint64_t id = 0; int taken = 0;
        if (IORegistryEntryGetRegistryEntryID(s, &id) != KERN_SUCCESS) continue;
        for (int q = 0; q < DM_PLAYERS; q++) taken |= g_abitdo[q].dev && g_abitdo[q].id == id;
        IOHIDDeviceRef dev = taken ? NULL : IOHIDDeviceCreate(kCFAllocatorDefault, s);
        if (!dev) continue;
        int size = [(__bridge NSNumber *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDMaxInputReportSizeKey)) intValue];
        if (size < ABITDO_U2W_REPORT) { LOG("[hid] %s: %d-byte reports carry no motion (firmware before 1.03)", ABITDO_U2W_NAME, size); CFRelease(dev); continue; }
        if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) { CFRelease(dev); continue; }
        *d = (abitdo_in){.dev = dev, .id = id, .owner = (__bridge const void *)c, .player = player};
        IOHIDDeviceRegisterInputReportCallback(dev, d->report, sizeof d->report, abitdo_report, d);
        IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        LOG("[hid] %s raw motion: player %d (HID report 0x01)", ABITDO_U2W_NAME, player + 1);
    }
    IOObjectRelease(it);
}
// Only the controller that took the player's reader stops it: with DM_PAD_MERGE every controller is player 1
static void abitdo_motion_detach(GCController *c, int player) {
    if (player < 0 || player >= DM_PLAYERS || !g_abitdo[player].dev || g_abitdo[player].owner != (__bridge const void *)c) return;
    abitdo_in *d = &g_abitdo[player];
    IOHIDDeviceRegisterInputReportCallback(d->dev, d->report, sizeof d->report, NULL, NULL);
    IOHIDDeviceUnscheduleFromRunLoop(d->dev, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOHIDDeviceClose(d->dev, kIOHIDOptionsTypeNone); CFRelease(d->dev);
    *d = (abitdo_in){0};
    dm_motion_clear_hid(player);
}
