// A DualSense's microphone over USB: the controller is a USB audio device that the AUHAL path in audio_input_macos.m captures like any
// microphone, and output report 0x02 (SetState, 48 bytes with the report ID) selects its built-in mic, sets its volume and clears a mute
// or audio power-saving state the controller may be in. No IOKit here (tested by dm/host/tests/t_dualsense_mic.c); audio_input_dualsense_mac.m
// sends it. Over Bluetooth the mic is not usable on a Mac (docs/reference/audio.md, "A DualSense's microphone over Bluetooth").
// The format is from the controllers wiki's DualSense data structures.
#ifndef DM_DUALSENSE_MIC_H
#define DM_DUALSENSE_MIC_H

#include <stdint.h>

enum { DS_VID = 0x054C, DS_PID = 0x0CE6, DS_EDGE_PID = 0x0DF2 };
enum { DS_USB_STATE_REPORT = 0x02, DS_USB_STATE_LEN = 48 };

void ds_usb_mic_state_report(uint8_t out[DS_USB_STATE_LEN]);

#endif
