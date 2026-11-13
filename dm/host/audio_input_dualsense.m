// A DualSense's microphone over USB (audio_input_dualsense.h): before the AUHAL capture opens a DualSense's USB audio, its HID device beside
// it gets the SetState report that selects the built-in mic and unmutes it (dualsense_mic.h). The device is opened shared, as GameController
// keeps it open, and closed again at once.
#include "audio_input_dualsense.h"
#include "dualsense_mic.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <stdio.h>
#include <string.h>

static int number_property(IOHIDDeviceRef dev, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key); int n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &n);
    return n;
}

static int on_usb(IOHIDDeviceRef dev) {
    CFTypeRef v = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey));
    return v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringCompare(v, CFSTR("USB"), 0) == kCFCompareEqualTo;
}

// The first DualSense (or Edge) on USB, created but not opened; asked of the registry each time, as ds4_take in haptics_apple.m does
static IOHIDDeviceRef ds_find_usb(void) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey); io_iterator_t it = 0;
    if (!match) return NULL;
    int vid = DS_VID; CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), v); CFRelease(v);
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return NULL;   // takes match
    IOHIDDeviceRef found = NULL;
    for (io_service_t s; !found && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, s);
        if (!dev) continue;
        int pid = number_property(dev, CFSTR(kIOHIDProductIDKey));
        if ((pid == DS_PID || pid == DS_EDGE_PID) && on_usb(dev)) found = dev;
        else CFRelease(dev);
    }
    IOObjectRelease(it);
    return found;
}

static int device_string(AudioDeviceID device, AudioObjectPropertySelector selector, char out[0x100]) {
    out[0] = 0;
    AudioObjectPropertyAddress address = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = NULL; UInt32 size = sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value) != noErr || !value) return 0;
    int ok = CFStringGetCString(value, out, 0x100, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok;
}

void dm_ds_usb_mic_enable(AudioDeviceID device) {
    UInt32 transport = 0, size = sizeof transport;
    AudioObjectPropertyAddress address = { kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &transport) != noErr || transport != kAudioDeviceTransportTypeUSB) return;
    char name[0x100], maker[0x100];
    device_string(device, kAudioObjectPropertyName, name); device_string(device, kAudioObjectPropertyManufacturer, maker);
    if (!strcasestr(maker, "Sony") && !strcasestr(name, "DualSense")) return;
    IOHIDDeviceRef dev = ds_find_usb();   // a DualShock 4's USB audio is Sony's too, but has no DualSense HID device beside it
    if (!dev) return;
    IOReturn e = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (e == kIOReturnSuccess) {
        uint8_t r[DS_USB_STATE_LEN];
        ds_usb_mic_state_report(r);
        e = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, r[0], r, sizeof r);
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    }
    fprintf(stderr, "[dm] audio input: DualSense over USB (\"%s\"): built-in mic selected and unmuted%s\n", name,
            e == kIOReturnSuccess ? "" : " failed");
    CFRelease(dev);
}
