// A DualSense's microphone over USB, Mac only: the controller is a USB audio device that the AUHAL path captures like any microphone;
// when one is picked, the host first selects its built-in mic and clears a mute or power-saving state (dualsense_mic.h has the report).
#ifndef DM_AUDIO_INPUT_DUALSENSE_H
#define DM_AUDIO_INPUT_DUALSENSE_H

#include <CoreAudio/CoreAudio.h>

void dm_ds_usb_mic_enable(AudioDeviceID device);   // a no-op unless device matches a DualSense's USB audio and its own USB HID device

#endif
