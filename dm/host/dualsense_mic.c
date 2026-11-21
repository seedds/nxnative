// A DualSense's microphone over USB: the SetState report that turns it on (dualsense_mic.h has the format and where it comes from)
#include "dualsense_mic.h"
#include <string.h>

void ds_usb_mic_state_report(uint8_t out[DS_USB_STATE_LEN]) {
    memset(out, 0, DS_USB_STATE_LEN);
    out[0] = DS_USB_STATE_REPORT;
    uint8_t *s = out + 1;              // SetState: only the sections whose allow bits are set apply
    s[0] = 0x40 | 0x80;                // allow the mic volume and the audio control
    s[1] = 0x01 | 0x02;                // allow the mute light and the mute control
    s[6] = 0x40;                       // mic volume: the top of its range
    s[7] = 0x01;                       // audio control: the built-in mic, echo and noise cancelling off (a hum or an instrument stays raw)
    s[8] = 0x00;                       // mute light off
    s[9] = 0x00;                       // mute control: nothing muted, no power saving
}
