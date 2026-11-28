// A DualSense's microphone over USB: the controller is a USB audio device that the AUHAL path in audio_input_mac.m captures like any
// microphone, and output report 0x02 (SetState, 48 bytes with the report ID) selects its built-in mic, sets its volume and clears a mute
// or audio power-saving state the controller may be in. Pure C helpers (no CoreAudio/IOKit, tested by dm/host/tests/t_dualsense_mic.c)
// build the report and match a CoreAudio USB audio device to its own USB HID controller; audio_input_dualsense_mac.m queries macOS and
// sends it. Over Bluetooth the mic is not usable on a Mac (docs/reference/audio.md, "A DualSense's microphone over Bluetooth").
// The report format is from the controllers wiki's DualSense data structures and awalol/DS5Dongle (MIT).
#ifndef DM_DUALSENSE_MIC_H
#define DM_DUALSENSE_MIC_H

#include <stdint.h>

enum { DS_VID = 0x054C, DS_PID = 0x0CE6, DS_EDGE_PID = 0x0DF2 };
enum { DS_USB_STATE_REPORT = 0x02, DS_USB_STATE_LEN = 48 };

typedef struct {
    int has_vid_pid;
    int vid, pid;
    uint32_t location_id;     // USB locationID from AudioBox BoxUID or AppleUSBAudioEngine DeviceUID; 0 if unavailable
    const char *serial;       // USB serial from kAudioObjectPropertySerialNumber; NULL or "" if unavailable
} ds_audio_usb_id;

typedef struct {
    int vid, pid;
    uint32_t location_id;
    const char *serial;       // kIOHIDSerialNumberKey; NULL or "" if unavailable
    int usb;                  // kIOHIDTransportKey == "USB"
    int synthetic;            // kIOHIDGCSyntheticDeviceKey == true
    int usage_page, usage;    // GenericDesktop (1), GamePad (5) or Joystick (4)
} ds_hid_candidate;

static inline int ds_is_dualsense_vid_pid(int vid, int pid) {
    return vid == DS_VID && (pid == DS_PID || pid == DS_EDGE_PID);
}

void ds_usb_mic_state_report(uint8_t out[DS_USB_STATE_LEN]);

// Parse CoreAudio USB strings into exact hardware identifiers (all fail closed and return 0 on mismatch/invalid input):
// - kAudioDevicePropertyModelUID: "<name>:<vid_hex>:<pid_hex>"
// - kAudioBoxPropertyBoxUID: "<name>@<location_hex>"
// - kAudioDevicePropertyDeviceUID: "AppleUSBAudioEngine:<maker>:<product>:<location_hex>:<iface>" (only when serial is absent)
int ds_parse_model_uid_vid_pid(const char *model_uid, int *vid_out, int *pid_out);
int ds_parse_box_uid_location(const char *box_uid, uint32_t *location_out);
int ds_parse_device_uid_location(const char *device_uid, const char *serial, uint32_t *location_out);

// Return the unique matching USB DualSense HID candidate index in [0, count), or -1 if none or ambiguous (fail closed).
int ds_match_usb_hid(const ds_audio_usb_id *audio, const ds_hid_candidate *cands, int count);

#endif
