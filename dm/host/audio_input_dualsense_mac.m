// A DualSense's microphone over USB (audio_input_dualsense.h): before the AUHAL capture opens a DualSense's USB audio, the matching USB
// HID controller (correlated by USB VID:PID and AudioBox locationID / DeviceUID / serial, failing closed on ambiguity) gets the SetState
// report that selects the built-in mic and unmutes it (dualsense_mic.h). Opened shared, as GameController keeps it open, and closed at once.
#include "audio_input_dualsense.h"
#include "dualsense_mic.h"
#include <CoreFoundation/CoreFoundation.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DS_HID_MAX = 32 };

static int64_t hid_number(IOHIDDeviceRef dev, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    int64_t n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberSInt64Type, &n);
    return n;
}

static int hid_string(IOHIDDeviceRef dev, CFStringRef key, char out[0x100]) {
    out[0] = 0;
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return 0;
    return CFStringGetCString(v, out, 0x100, kCFStringEncodingUTF8) && out[0];
}

static int object_string(AudioObjectID obj, AudioObjectPropertySelector selector, char out[0x100]) {
    out[0] = 0;
    AudioObjectPropertyAddress address = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = NULL;
    UInt32 size = sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(obj, &address, 0, NULL, &size, &value) != noErr || !value) return 0;
    int ok = CFStringGetCString(value, out, 0x100, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok && out[0];
}

// Find the AudioBox owning device so we can read its BoxUID ("<name>@<location_hex>") and SerialNumber.
static AudioObjectID owning_audio_box(AudioDeviceID device) {
    AudioObjectPropertyAddress box_list = { kAudioHardwarePropertyBoxList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &box_list, 0, NULL, &size) != noErr || size < sizeof(AudioObjectID))
        return kAudioObjectUnknown;
    AudioObjectID *boxes = malloc(size);
    if (!boxes) return kAudioObjectUnknown;
    AudioObjectID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &box_list, 0, NULL, &size, boxes) == noErr) {
        UInt32 nboxes = size / sizeof(AudioObjectID);
        AudioObjectPropertyAddress dev_list = { kAudioBoxPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        for (UInt32 i = 0; i < nboxes && found == kAudioObjectUnknown; i++) {
            UInt32 dsize = 0;
            if (AudioObjectGetPropertyDataSize(boxes[i], &dev_list, 0, NULL, &dsize) != noErr || dsize < sizeof(AudioDeviceID)) continue;
            AudioDeviceID *devs = malloc(dsize);
            if (!devs) continue;
            if (AudioObjectGetPropertyData(boxes[i], &dev_list, 0, NULL, &dsize, devs) == noErr) {
                UInt32 ndevs = dsize / sizeof(AudioDeviceID);
                for (UInt32 j = 0; j < ndevs; j++)
                    if (devs[j] == device) { found = boxes[i]; break; }
            }
            free(devs);
        }
    }
    free(boxes);
    return found;
}

static IOHIDDeviceRef ds_find_matching_usb(const ds_audio_usb_id *audio, uint32_t *matched_loc) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey);
    io_iterator_t it = 0;
    if (!match) return NULL;
    int vid = DS_VID;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
    if (!v) { CFRelease(match); return NULL; }
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), v);
    CFRelease(v);
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return NULL;   // takes match

    IOHIDDeviceRef devs[DS_HID_MAX];
    ds_hid_candidate cands[DS_HID_MAX];
    char serials[DS_HID_MAX][0x100];
    int count = 0;
    for (io_service_t s; count < DS_HID_MAX && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, s);
        if (!dev) continue;
        char transport[0x100];
        hid_string(dev, CFSTR(kIOHIDTransportKey), transport);
        hid_string(dev, CFSTR(kIOHIDSerialNumberKey), serials[count]);
        devs[count] = dev;
        cands[count] = (ds_hid_candidate){
            .vid = (int)hid_number(dev, CFSTR(kIOHIDVendorIDKey)),
            .pid = (int)hid_number(dev, CFSTR(kIOHIDProductIDKey)),
            .location_id = (uint32_t)(uint64_t)hid_number(dev, CFSTR(kIOHIDLocationIDKey)),
            .serial = serials[count],
            .usb = strcmp(transport, "USB") == 0,
            .synthetic = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue,
            .usage_page = (int)hid_number(dev, CFSTR(kIOHIDPrimaryUsagePageKey)),
            .usage = (int)hid_number(dev, CFSTR(kIOHIDPrimaryUsageKey)),
        };
        count++;
    }
    IOObjectRelease(it);

    int idx = ds_match_usb_hid(audio, cands, count);
    IOHIDDeviceRef found = NULL;
    if (idx >= 0) {
        found = devs[idx];
        if (matched_loc) *matched_loc = cands[idx].location_id;
    }
    for (int i = 0; i < count; i++)
        if (i != idx) CFRelease(devs[i]);
    return found;
}

void dm_ds_usb_mic_enable(AudioDeviceID device) {
    UInt32 transport = 0, size = sizeof transport;
    AudioObjectPropertyAddress address = { kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &transport) != noErr || transport != kAudioDeviceTransportTypeUSB) return;

    char model_uid[0x100], name[0x100], maker[0x100], device_uid[0x100], box_uid[0x100], serial[0x100];
    ds_audio_usb_id audio = {0};
    if (object_string(device, kAudioDevicePropertyModelUID, model_uid) &&
        ds_parse_model_uid_vid_pid(model_uid, &audio.vid, &audio.pid)) {
        audio.has_vid_pid = 1;
        if (!ds_is_dualsense_vid_pid(audio.vid, audio.pid)) return;
    } else {
        object_string(device, kAudioObjectPropertyName, name);
        object_string(device, kAudioObjectPropertyManufacturer, maker);
        if (!strcasestr(maker, "Sony") && !strcasestr(name, "DualSense")) return;
    }
    object_string(device, kAudioObjectPropertyName, name);
    object_string(device, kAudioObjectPropertySerialNumber, serial);

    AudioObjectID box = owning_audio_box(device);
    if (box != kAudioObjectUnknown) {
        if (object_string(box, kAudioBoxPropertyBoxUID, box_uid))
            ds_parse_box_uid_location(box_uid, &audio.location_id);
        if (!serial[0]) object_string(box, kAudioObjectPropertySerialNumber, serial);
    }
    if (!audio.location_id && object_string(device, kAudioDevicePropertyDeviceUID, device_uid))
        ds_parse_device_uid_location(device_uid, serial, &audio.location_id);
    audio.serial = serial;

    uint32_t loc = 0;
    IOHIDDeviceRef dev = ds_find_matching_usb(&audio, &loc);
    if (!dev) {
        if (audio.has_vid_pid)
            fprintf(stderr, "[dm] audio input: DualSense over USB (\"%s\"): no unique matching USB HID device (loc=0x%08x)\n",
                    name, audio.location_id);
        return;
    }
    IOReturn e = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (e == kIOReturnSuccess) {
        uint8_t r[DS_USB_STATE_LEN];
        ds_usb_mic_state_report(r);
        e = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, r[0], r, sizeof r);
        IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    }
    if (e == kIOReturnSuccess)
        fprintf(stderr, "[dm] audio input: DualSense over USB (\"%s\", loc=0x%08x): built-in mic selected and unmuted\n", name, loc);
    else
        fprintf(stderr, "[dm] audio input: DualSense over USB (\"%s\", loc=0x%08x): mic enable failed (IOReturn 0x%x)\n", name, loc, (unsigned)e);
    CFRelease(dev);
}
