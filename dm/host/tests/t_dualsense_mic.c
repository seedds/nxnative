// A DualSense's USB mic report (dualsense_mic.c), byte for byte: the SetState that selects the built-in mic and unmutes it, and nothing
// outside the mic's sections, so the light bar, triggers and rumble keep what GameController set.
#include "../dualsense_mic.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t u[DS_USB_STATE_LEN];
    ds_usb_mic_state_report(u);
    assert(u[0] == 0x02 && u[1] == 0xC0 && u[2] == 0x03 && u[7] == 0x40 && u[8] == 0x01 && u[9] == 0 && u[10] == 0);
    int set = 0;
    for (int i = 0; i < DS_USB_STATE_LEN; i++) set += u[i] != 0;
    assert(set == 5);
    puts("DualSense USB microphone report: PASS");
    return 0;
}
