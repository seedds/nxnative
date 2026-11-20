// The output route's audio path, decided per output device and live (hle_audio.c reads the device, this decides; tests/t_audio_route.c checks the table). Pure: no Core Audio,
// so the decision is the same in the host, the tests and the notes. The audio counterpart of nvn/hdr.h (the screen decides HDR, the setting stays as the player chose it).
//
// DM_SPATIAL_AUDIO (read once, never written): auto (the default for a game whose profile says spatial_audio on: Ocarina of Time) or 0 (the stereo fold on a two-channel device,
// as before). The game mixes what the host reports through GetActiveAudioDeviceChannelCountForOutput (hle_audren.c): 6 → its 5.1 mode, else stereo, asked at start and again on its output-device notification. So on a spatial
// route the host reports 6 although the device has 2, and renders the game's real 5.1 for the device:
//   six or more device channels (a Studio Display: USB, 8 channels; HDMI 5.1)  → AP_SURROUND: the 5.1 mix as it is (MPEG_5_1_A), the device maps it to its speakers. Either setting
//   two channels, Bluetooth or the built-in headphone jack (AirPods)          → AP_SYSTEM: AVSampleBufferAudioRenderer with the 5.1 layout, so macOS offers its own Spatial Audio
//                                                                                (Sound menu: Off / Fixed / Head Tracked); AP_HEADPHONES (AUSpatialMixer for headphones) when
//                                                                                the system renderer is not available
//   two channels, built-in speakers (MacBook Pro)                             → AP_SPEAKERS: AUSpatialMixer, output type built-in speakers (Apple's speaker virtualization)
//   anything else (USB or AirPlay stereo, virtual devices such as BlackHole 2ch, a mono route, an unknown transport) → AP_FOLD: the stereo fold, reported 2
#pragma once
#include <stdint.h>
#include <string.h>

enum { SPATIAL_PREF_OFF = 0, SPATIAL_PREF_AUTO = 1 };
// DM_SPATIAL_AUDIO: "auto" or "0"; unset or empty → dflt (the profile's); anything else → dflt with *bad set (the caller logs it, the setting stays the default)
static inline int spatial_pref_parse(const char *e, int dflt, int *bad) {
    *bad = 0;
    if (!e || !*e) return dflt;
    if (!strcmp(e, "auto")) return SPATIAL_PREF_AUTO;
    if (!strcmp(e, "0")) return SPATIAL_PREF_OFF;
    *bad = 1; return dflt;
}

// kAudioDevicePropertyTransportType values (AudioHardwareBase.h), as four-character codes so this header needs no Core Audio
#define AR_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum { AT_UNKNOWN, AT_BUILTIN, AT_BLUETOOTH, AT_USB, AT_HDMI, AT_DISPLAYPORT, AT_THUNDERBOLT, AT_AIRPLAY, AT_VIRTUAL, AT_AGGREGATE, AT_OTHER };
static inline int audio_transport_of(uint32_t t) {
    switch (t) {
    case 0: return AT_UNKNOWN;
    case AR_FOURCC('b', 'l', 't', 'n'): return AT_BUILTIN;
    case AR_FOURCC('b', 'l', 'u', 'e'): case AR_FOURCC('b', 'l', 'e', 'a'): return AT_BLUETOOTH;   // classic and LE
    case AR_FOURCC('u', 's', 'b', ' '): return AT_USB;
    case AR_FOURCC('h', 'd', 'm', 'i'): return AT_HDMI;
    case AR_FOURCC('d', 'p', 'r', 't'): return AT_DISPLAYPORT;
    case AR_FOURCC('t', 'h', 'u', 'n'): return AT_THUNDERBOLT;
    case AR_FOURCC('a', 'i', 'r', 'p'): return AT_AIRPLAY;
    case AR_FOURCC('v', 'i', 'r', 't'): return AT_VIRTUAL;
    case AR_FOURCC('g', 'r', 'u', 'p'): case AR_FOURCC('a', 'g', 'g', 'r'): return AT_AGGREGATE;
    default: return AT_OTHER;
    }
}
static inline const char *audio_transport_name(int t) {
    static const char *n[] = {"unknown transport", "built-in", "Bluetooth", "USB", "HDMI", "DisplayPort", "Thunderbolt", "AirPlay", "virtual", "aggregate", "other transport"};
    return t >= 0 && t <= AT_OTHER ? n[t] : "?";
}
// kAudioDevicePropertyDataSource of a built-in device: 'hdpn' the headphone jack, 'ispk' the internal speakers
#define AR_DS_HEADPHONES AR_FOURCC('h', 'd', 'p', 'n')
#define AR_DS_SPEAKERS AR_FOURCC('i', 's', 'p', 'k')

enum { AP_FOLD, AP_SURROUND, AP_SYSTEM, AP_SPEAKERS, AP_HEADPHONES, AP_COUNT };
// What the host can make: the system renderer (audio_spatial_mac.m, linked into the Mac host only) and AUSpatialMixer (the component exists)
enum { AR_CAN_SYSTEM = 1, AR_CAN_MIXER = 2 };
// kSpatialMixerOutputType_* (AudioUnitProperties.h): the mixer's output type for AP_SPEAKERS / AP_HEADPHONES
enum { AR_MIXER_HEADPHONES = 1, AR_MIXER_BUILTIN_SPEAKERS = 2 };
typedef struct {
    int path;        // AP_*
    int report;      // channels the game is told (6: it mixes 5.1; 2: stereo)
    int pull;        // channels a DeviceSink output takes off its queue per frame: the six, or their fold
    int mixer_type;  // AR_MIXER_* for the AUSpatialMixer paths, else 0
} audio_route_decision;
static inline int audio_route_headphones(int transport, uint32_t data_source) {
    return transport == AT_BLUETOOTH || (transport == AT_BUILTIN && data_source == AR_DS_HEADPHONES);
}
static inline audio_route_decision audio_route_decide(int pref, int transport, uint32_t data_source, int dev_ch, unsigned can) {
    audio_route_decision d = {AP_FOLD, 2, 2, 0};
    if (dev_ch >= 6) { d.path = AP_SURROUND; d.report = d.pull = 6; return d; }   // as before the setting existed: the device takes the 5.1 itself
    if (pref != SPATIAL_PREF_AUTO || dev_ch != 2) return d;                      // off, mono (a headset's call mode), or unknown
    if (audio_route_headphones(transport, data_source)) {
        if (can & AR_CAN_SYSTEM) d.path = AP_SYSTEM;
        else if (can & AR_CAN_MIXER) { d.path = AP_HEADPHONES; d.mixer_type = AR_MIXER_HEADPHONES; }
    } else if (transport == AT_BUILTIN && (can & AR_CAN_MIXER)) { d.path = AP_SPEAKERS; d.mixer_type = AR_MIXER_BUILTIN_SPEAKERS; }
    if (d.path != AP_FOLD) d.report = d.pull = 6;
    return d;
}
static inline const char *audio_path_name(int p) {
    static const char *n[] = {"the stereo fold", "5.1 as it is (MPEG_5_1_A, the device maps it to its speakers)",
                              "5.1 through the system renderer (AVSampleBufferAudioRenderer: macOS's Spatial Audio, Sound menu Fixed / Head Tracked)",
                              "5.1 through AUSpatialMixer for the built-in speakers", "5.1 through AUSpatialMixer for headphones (the system renderer is not available)"};
    return p >= 0 && p < AP_COUNT ? n[p] : "?";
}
