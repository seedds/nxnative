// Mac USB output for two independent Switch HD rumble motors.
// CoreAudio channels 1 and 2 remain silent; channels 3 and 4 drive the grips.
#pragma once
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include "hle_haptics.h"

enum { DS_RATE = 48000, DS_CHANNELS = 4, DS_QUEUE = 256, DS_RAMP = 96 };
typedef struct {
    uint64_t time;
    unsigned generation;
    dm_haptic_state state;
    float gain, cutoff;
} DsEvent;
typedef struct {
    float amplitude, target, step, frequency, phase;
    unsigned remaining;
} DsTone;
typedef struct {
    AudioUnit unit;
    _Atomic unsigned head, tail, generation;
    DsEvent events[DS_QUEUE];
    DsTone tone[2][2];
    unsigned renderGeneration;   // only the audio thread touches these fields
    uint64_t epoch;               // only the producer touches this field
    uint64_t delay;
    bool initialized;
} DsUsb;

static void ds_usb_clear(DsUsb *d) {
    atomic_fetch_add(&d->generation, 1);
}
static void ds_usb_submit(DsUsb *d, dm_haptic_state s, float gain, float cutoff) {
    if (s.epoch != d->epoch) { ds_usb_clear(d); d->epoch = s.epoch; }
    unsigned head = atomic_load_explicit(&d->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&d->tail, memory_order_acquire) >= DS_QUEUE) {
        // Cancel queued state so an old sustained vibration cannot get stuck.
        ds_usb_clear(d);
        return;
    }
    d->events[head % DS_QUEUE] = (DsEvent){
        clock_gettime_nsec_np(CLOCK_UPTIME_RAW), atomic_load(&d->generation), s, gain, cutoff,
    };
    atomic_store_explicit(&d->head, head + 1, memory_order_release);
}
static void ds_tone_set(DsTone *t, float a, float f) {
    if (f > 0) t->frequency = f;
    if (t->target == a) return;
    t->target = a;
    t->step = (a - t->amplitude) / DS_RAMP;
    t->remaining = DS_RAMP;
}
static void ds_set_event(DsUsb *d, const DsEvent *e) {
    for (int m = 0; m < 2; ++m) {
        const dm_vibration *v = &e->state.motor[m];
        // Give both bands headroom, then apply the cutoff and limit their combined peak.
        // Scaling them together preserves the balance between the two frequencies.
        float lo = v->amplitude_low * e->gain * .5f, hi = v->amplitude_high * e->gain * .5f;
        float peak = lo + hi;
        float scale = peak > e->cutoff ? (fminf(1, peak) - e->cutoff) / ((1 - e->cutoff) * peak) : 0;
        ds_tone_set(&d->tone[m][0], lo * scale, v->frequency_low);
        ds_tone_set(&d->tone[m][1], hi * scale, v->frequency_high);
    }
}
static float ds_tone_sample(DsTone *t) {
    if (t->remaining) {
        t->amplitude += t->step;
        if (!--t->remaining) t->amplitude = t->target;
    }
    if (!t->amplitude) return 0;
    float value = t->amplitude * sinf(t->phase);
    t->phase += t->frequency * (2.f * (float)M_PI / DS_RATE);
    if (t->phase >= 2.f * (float)M_PI) t->phase -= 2.f * (float)M_PI;
    return value;
}
static void ds_usb_pcm(DsUsb *d, float *out, unsigned frames, uint64_t start) {
    // Read the published head before its generation so a newly published event
    // cannot be mistaken for stale data when a route changes mid-callback.
    unsigned head = atomic_load_explicit(&d->head, memory_order_acquire);
    unsigned generation = atomic_load(&d->generation);
    if (d->renderGeneration != generation) {
        memset(d->tone, 0, sizeof d->tone);
        d->renderGeneration = generation;
    }
    unsigned tail = atomic_load_explicit(&d->tail, memory_order_relaxed);
    for (unsigned i = 0; i < frames; ++i) {
        uint64_t time = start + (uint64_t)i * 1000000000 / DS_RATE;
        while (tail != head) {
            const DsEvent *e = &d->events[tail % DS_QUEUE];
            if (e->generation == generation) {
                if (e->time > time) break;
                ds_set_event(d, e);
            }
            ++tail;
        }
        *out++ = 0; *out++ = 0;
        for (int m = 0; m < 2; ++m)
            *out++ = ds_tone_sample(&d->tone[m][0]) + ds_tone_sample(&d->tone[m][1]);
    }
    atomic_store_explicit(&d->tail, tail, memory_order_release);
}
static OSStatus ds_usb_render(void *ctx, AudioUnitRenderActionFlags *flags,
                              const AudioTimeStamp *time, UInt32 bus, UInt32 frames, AudioBufferList *data) {
    (void)flags; (void)time; (void)bus;
    DsUsb *d = ctx;
    if (data->mNumberBuffers != 1 || data->mBuffers[0].mNumberChannels != DS_CHANNELS ||
        data->mBuffers[0].mDataByteSize < frames * DS_CHANNELS * sizeof(float) || !data->mBuffers[0].mData) {
        for (UInt32 i = 0; i < data->mNumberBuffers; ++i)
            if (data->mBuffers[i].mData) memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
        return kAudioUnitErr_FormatNotSupported;
    }
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ds_usb_pcm(d, data->mBuffers[0].mData, frames, now > d->delay ? now - d->delay : 0);
    return noErr;
}
static OSStatus ds_property(AudioObjectID id, AudioObjectPropertySelector selector,
                            AudioObjectPropertyScope scope, UInt32 *size, void *value) {
    AudioObjectPropertyAddress a = {selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(id, &a, 0, NULL, size, value);
}
static CFStringRef ds_string(AudioDeviceID id, AudioObjectPropertySelector selector) CF_RETURNS_RETAINED {
    CFStringRef string = NULL;
    UInt32 size = sizeof(CFStringRef);
    return ds_property(id, selector, kAudioObjectPropertyScopeGlobal, &size, &string) == noErr ? string : NULL;
}
static bool ds_four_outputs(AudioDeviceID id) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &size) || !size) return false;
    AudioBufferList *buffers = calloc(1, size);
    if (!buffers) return false;
    UInt32 n = 0;
    if (!AudioObjectGetPropertyData(id, &a, 0, NULL, &size, buffers))
        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) n += buffers->mBuffers[i].mNumberChannels;
    free(buffers);
    return n == DS_CHANNELS;
}
static AudioDeviceID ds_usb_device(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) || !size) return 0;
    AudioDeviceID *ids = malloc(size);
    if (!ids) return 0;
    AudioDeviceID result = 0;
    if (!AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids)) {
        for (UInt32 i = 0; i < size / sizeof(*ids); ++i) {
            AudioDeviceID id = ids[i];
            UInt32 transport = 0, n = sizeof(transport);
            if (ds_property(id, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &n, &transport) ||
                transport != kAudioDeviceTransportTypeUSB || !ds_four_outputs(id)) continue;
            NSString *name = CFBridgingRelease(ds_string(id, kAudioObjectPropertyName));
            NSString *maker = CFBridgingRelease(ds_string(id, kAudioObjectPropertyManufacturer));
            if (![maker hasPrefix:@"Sony"] ||
                (![name isEqualToString:@"DualSense Wireless Controller"] &&
                 ![name isEqualToString:@"DualSense Edge Wireless Controller"])) continue;
            if (result) { result = 0; break; }
            result = id;
        }
    }
    free(ids);
    return result;
}
static IOReturn ds_usb_enable_pcm(AudioDeviceID audio) {
    // Match the same USB location before touching a HID output report.
    NSString *uid = CFBridgingRelease(ds_string(audio, kAudioDevicePropertyDeviceUID));
    NSArray<NSString *> *parts = [uid componentsSeparatedByString:@":"];
    if (parts.count < 5 || ![parts[0] isEqualToString:@"AppleUSBAudioEngine"]) return kIOReturnNotFound;
    unsigned location = 0;
    if (![[NSScanner scannerWithString:parts[parts.count - 2]] scanHexInt:&location] || !location) return kIOReturnNotFound;
    CFMutableDictionaryRef match = IOServiceMatching(kIOHIDDeviceKey);
    if (!match) return kIOReturnNotFound;
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), (__bridge CFNumberRef)@0x054c);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it)) return kIOReturnNotFound;
    IOReturn result = kIOReturnNotFound;
    for (io_service_t service; (service = IOIteratorNext(it)); IOObjectRelease(service)) {
        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, service);
        if (!dev) continue;
        unsigned pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductIDKey)) unsignedIntValue];
        unsigned loc = [(__bridge NSNumber *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDLocationIDKey)) unsignedIntValue];
        if ((pid == 0x0ce6 || pid == 0x0df2) && loc == location) {
            result = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
            if (!result) {
                // Clear emulated rumble flags without requesting LED, trigger, or audio changes.
                uint8_t report[48] = {2};
                result = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, report[0], report, sizeof(report));
                IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
            }
        }
        CFRelease(dev);
        if (!result) { IOObjectRelease(service); break; }
    }
    IOObjectRelease(it);
    return result;
}
static void ds_usb_close(DsUsb *d) {
    if (d->unit) {
        AudioOutputUnitStop(d->unit);
        if (d->initialized) AudioUnitUninitialize(d->unit);
        AudioComponentInstanceDispose(d->unit);
    }
    *d = (DsUsb){0};
}
static OSStatus ds_usb_open(DsUsb *d, AudioDeviceID id) {
    if (!id) return kAudioHardwareBadDeviceError;
    AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component) return kAudioUnitErr_FailedInitialization;
    OSStatus err = AudioComponentInstanceNew(component, &d->unit);
    if (err) goto fail;
    UInt32 off = 0, on = 1, frames = 0, bytes = sizeof(frames);
    err = AudioUnitSetProperty(d->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &off, sizeof(off));
    if (!err) err = AudioUnitSetProperty(d->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &on, sizeof(on));
    if (!err) err = AudioUnitSetProperty(d->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, sizeof(id));
    if (!err) err = ds_property(id, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &bytes, &frames);
    d->delay = 2000000 + (uint64_t)frames * 1000000000 / DS_RATE;
    AudioStreamBasicDescription format = {DS_RATE, kAudioFormatLinearPCM, kAudioFormatFlagsNativeFloatPacked,
        DS_CHANNELS * sizeof(float), 1, DS_CHANNELS * sizeof(float), DS_CHANNELS, 32, 0};
    if (!err) err = AudioUnitSetProperty(d->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
    SInt32 map[DS_CHANNELS] = {0, 1, 2, 3};
    if (!err) err = AudioUnitSetProperty(d->unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 0, map, sizeof(map));
    AURenderCallbackStruct cb = {ds_usb_render, d};
    if (!err) err = AudioUnitSetProperty(d->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    if (!err) { err = AudioUnitInitialize(d->unit); d->initialized = err == noErr; }
    if (!err) err = AudioOutputUnitStart(d->unit);
    if (!err) return noErr;
fail:
    ds_usb_close(d);
    return err;
}
