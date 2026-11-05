// Output routing (interface in audio_route_mac.h): CoreAudio listeners on the system's default output device
// and on the built-in device's data source decide headphone / speaker, and while the Audio output setting is
// Wide stereo or Binaural (and Headphone auto-switch is on) move between exactly those two. A mode the player
// picked outside them is never overridden. Three listeners: the default output device, the device list, and
// the built-in output device's data source - a 3.5 mm plug usually keeps the device ID and only changes the
// data source, so the device list alone would miss it.
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#include "audio_route_mac.h"
#include "hle.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static dispatch_queue_t g_q;
static _Atomic int g_started, g_hp = -1;
static AudioObjectID g_builtin;
static int g_gen;   // only read/written on g_q

static UInt32 dev_u32(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    UInt32 v = 0, sz = sizeof v;
    AudioObjectPropertyAddress pa = { sel, scope, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &pa, 0, NULL, &sz, &v)) return 0;
    return v;
}
static AudioObjectID default_out(void) {
    AudioObjectID d = 0; UInt32 sz = sizeof d;
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, &d)) return 0;
    return d;
}
static int has_outputs(AudioObjectID dev) {
    AudioObjectPropertyAddress pa = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    return !AudioObjectGetPropertyDataSize(dev, &pa, 0, NULL, &sz) && sz > 0;
}
static const char *transport_name(UInt32 tr) {
    switch (tr) {
    case kAudioDeviceTransportTypeBuiltIn: return "built-in";
    case kAudioDeviceTransportTypeUSB: return "USB";
    case kAudioDeviceTransportTypeBluetooth: case kAudioDeviceTransportTypeBluetoothLE: return "Bluetooth";
    case kAudioDeviceTransportTypeHDMI: return "HDMI";
    case kAudioDeviceTransportTypeDisplayPort: return "DisplayPort";
    case kAudioDeviceTransportTypeThunderbolt: return "Thunderbolt";
    case kAudioDeviceTransportTypeAirPlay: return "AirPlay";
    case kAudioDeviceTransportTypeVirtual: return "virtual";
    case kAudioDeviceTransportTypeAggregate: return "aggregate";
    default: return "other";
    }
}
static void dev_name(AudioObjectID dev, char *o, size_t n) {
    CFStringRef nm = NULL; UInt32 sz = sizeof nm;
    AudioObjectPropertyAddress pa = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    o[0] = 0;
    if (!AudioObjectGetPropertyData(dev, &pa, 0, NULL, &sz, &nm) && nm) { CFStringGetCString(nm, o, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(nm); }
}
// Headphone verdict: the built-in device is classified by its data source ('hdpn' headphones / 'ispk'
// speakers); USB and Bluetooth count as headphones; HDMI, DisplayPort, AirPlay, virtual and aggregate count
// as speakers. With the data source unreadable the device's name breaks the tie.
static int is_headphones(AudioObjectID dev) {
    if (!dev) return 0;
    UInt32 tr = dev_u32(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
    if (tr == kAudioDeviceTransportTypeBuiltIn) {
        UInt32 src = dev_u32(dev, kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput);
        if (src == 'hdpn') return 1;
        if (src == 'ispk') return 0;
        char nm[128]; dev_name(dev, nm, sizeof nm);
        return strcasestr(nm, "headphone") != NULL;
    }
    return tr == kAudioDeviceTransportTypeUSB || tr == kAudioDeviceTransportTypeBluetooth || tr == kAudioDeviceTransportTypeBluetoothLE;
}
static int hp_auto_on(void) {
    const char *h = getenv("DM_AUDIO_HP_AUTO");
    return !(h && atoi(h) == 0);
}
// Decide, and switch if allowed. old = -1 is the first verdict after start: headphones already plugged in
// also switch wide -> binaural, the same meaning as "headphones inserted".
static void evaluate(const char *why) {
    AudioObjectID dev = default_out();
    int hp = is_headphones(dev);
    { const char *t = getenv("DM_TEST_AUDIO_ROUTE");   // test: force the verdict so the switch path runs without the hardware
      if (t && *t) hp = !strcmp(t, "headphone"); }
    int old = atomic_exchange(&g_hp, hp);
    UInt32 tr = dev ? dev_u32(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) : 0;
    char nm[128] = "(no output device)"; if (dev) dev_name(dev, nm, sizeof nm);
    LOG("audio route (%s): default output \"%s\", device %u / %s -> %s%s", why, nm, (unsigned)dev, transport_name(tr),
        hp ? "headphones" : "speakers", old < 0 ? " (first verdict)" : old != hp ? " (changed)" : "");
    // A device change reapplies the output unit's buffer through its own property listeners (au_device_changed
    // in hle_audio.c); the mode decision comes after, so a switch never runs against the old device's cadence.
    if (old == hp) return;   // unchanged: no switch
    if (!hp_auto_on()) { LOG("audio route: Headphone auto-switch is off: recording only"); return; }
    int m = dm_audio_mode();
    if (hp && m == DM_AM_WIDE) {
        dm_audio_set_mode(DM_AM_BINAURAL);
        LOG("audio route: wide stereo -> binaural (headphones)");
    } else if (!hp && m == DM_AM_BINAURAL) {
        dm_audio_set_mode(DM_AM_WIDE);
        LOG("audio route: binaural -> wide stereo (speakers)");
    } else LOG("audio route: the current mode \"%s\" is not one of the two the auto-switch moves between", dm_audio_mode_name());
}
static void schedule(const char *why) {
    int my = ++g_gen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), g_q, ^{ if (my == g_gen) evaluate(why); });
}
static void watch_builtin(AudioObjectID already);
static OSStatus on_prop(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ctx) {
    (void)obj; (void)n; (void)a; (void)ctx;
    dispatch_async(g_q, ^{ schedule("device change"); });
    if (a && a->mSelector == kAudioHardwarePropertyDevices) dispatch_async(g_q, ^{
        watch_builtin(g_builtin);   // find the built-in device again: its ID may have changed
    });
    return noErr;
}
// (Re)find the built-in output device and attach its data-source listener. Runs once at startup and again on
// every device-list change: the built-in device's ID may change across those, and a 3.5 mm plug usually keeps
// the device ID and only changes the data source, so the device list alone would miss it. already = the device
// currently tracked, which the lookup skips: re-adding the same listener is pointless (0 at startup).
static void watch_builtin(AudioObjectID already) {
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &sz) || !sz) return;
    AudioObjectID *ids = malloc(sz); UInt32 got = sz;
    if (!AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &got, ids)) {
        for (UInt32 i = 0; i < got / sizeof(AudioObjectID); i++) {
            if (dev_u32(ids[i], kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal) != kAudioDeviceTransportTypeBuiltIn || !has_outputs(ids[i])) continue;
            if (ids[i] == already) break;
            g_builtin = ids[i];
            AudioObjectPropertyAddress ds = { kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
            AudioObjectAddPropertyListener(g_builtin, &ds, on_prop, NULL);
            LOG("audio route: listening for the data source of the built-in device %u (3.5 mm plug)", (unsigned)g_builtin);
            break;
        }
    }
    free(ids);
}
void dm_audio_route_init(void) {
    if (atomic_exchange(&g_started, 1)) return;
    g_q = dispatch_queue_create("dm.audio.route", DISPATCH_QUEUE_SERIAL);
    AudioObjectPropertyAddress pa = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &pa, on_prop, NULL);
    AudioObjectPropertyAddress pd = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &pd, on_prop, NULL);
    dispatch_async(g_q, ^{
        watch_builtin(0);   // first lookup of the built-in device
        evaluate("startup");
    });
}
