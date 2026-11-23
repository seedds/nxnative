// DualSense USB microphone helpers (dualsense_mic_mac.c):
// - Byte-for-byte SetState 0x02 report (only mic sections enabled, leaving light bar, triggers and rumble untouched).
// - CoreAudio ModelUID, AudioBox BoxUID and AppleUSBAudioEngine DeviceUID parsers.
// - Exact USB audio-to-HID controller matching across multiple DualSense pads, mixed Sony devices, synthetic entries and missing identity.
#include "../dualsense_mic.h"
#include <assert.h>
#include <stdio.h>

static void test_state_report(void) {
    uint8_t u[DS_USB_STATE_LEN];
    ds_usb_mic_state_report(u);
    assert(u[0] == 0x02 && u[1] == 0xC0 && u[2] == 0x03 && u[7] == 0x40 && u[8] == 0x01 && u[9] == 0 && u[10] == 0);
    int set = 0;
    for (int i = 0; i < DS_USB_STATE_LEN; i++) set += u[i] != 0;
    assert(set == 5);
}

static void test_parsers(void) {
    int vid = 0, pid = 0;
    assert(ds_parse_model_uid_vid_pid("DualSense Wireless Controller:054C:0CE6", &vid, &pid));
    assert(vid == DS_VID && pid == DS_PID);
    assert(ds_parse_model_uid_vid_pid("Name:With:Colons:54c:df2", &vid, &pid));
    assert(vid == DS_VID && pid == DS_EDGE_PID);
    assert(ds_parse_model_uid_vid_pid("Wireless Controller:054C:09CC", &vid, &pid));
    assert(vid == DS_VID && pid == 0x09CC && !ds_is_dualsense_vid_pid(vid, pid));
    assert(!ds_parse_model_uid_vid_pid("BuiltInMicrophoneDevice", &vid, &pid));
    assert(!ds_parse_model_uid_vid_pid("Foo:054C:0CE60", &vid, &pid));
    assert(!ds_parse_model_uid_vid_pid("Foo::0CE6", &vid, &pid));
    assert(!ds_parse_model_uid_vid_pid("054C:0CE6", &vid, &pid));

    uint32_t loc = 0;
    assert(ds_parse_box_uid_location("DualSense Wireless Controller@03100000", &loc) && loc == 0x03100000u);
    assert(ds_parse_box_uid_location("Device@80210000", &loc) && loc == 0x80210000u);
    assert(ds_parse_box_uid_location("Device@2120000", &loc) && loc == 0x02120000u);
    assert(!ds_parse_box_uid_location("Device@00000000", &loc));
    assert(!ds_parse_box_uid_location("AXAudioPluginBox_UID", &loc));
    assert(!ds_parse_box_uid_location("Device@123456789", &loc));
    assert(!ds_parse_box_uid_location("Device@xyz", &loc));

    loc = 0;
    assert(ds_parse_device_uid_location("AppleUSBAudioEngine:Sony Interactive Entertainment:DualSense Wireless Controller:2120000:1,2", "", &loc));
    assert(loc == 0x02120000u);
    assert(ds_parse_device_uid_location("AppleUSBAudioEngine:Sony Interactive Entertainment:Wireless Controller:03100000:3", NULL, &loc));
    assert(loc == 0x03100000u);
    // When serial is present, AppleUSBAudioEngine puts the serial in that field: do not misread an all-hex serial as a locationID.
    assert(!ds_parse_device_uid_location("AppleUSBAudioEngine:Sony:DualSense:12345678:1", "12345678", &loc));
    assert(!ds_parse_device_uid_location("AppleUSBAudioEngine:Actions:USB Audio & HID:F44EFD64BD41:2", "F44EFD64BD41", &loc));
    assert(!ds_parse_device_uid_location("AppleUSBAudioEngine:Sony:DualSense:00000000:1", "", &loc));
    assert(!ds_parse_device_uid_location("AppleUSBAudioEngine:OnlyOne:2120000:1", "", &loc));
    assert(!ds_parse_device_uid_location("BuiltInMicrophoneDevice", "", &loc));
}

static void test_matching(void) {
    const ds_hid_candidate two_pads[] = {
        { DS_VID, DS_PID,      0x01100000u, "", 1, 0, 1, 5 },   // DualSense #1 on port 1
        { DS_VID, DS_PID,      0x02100000u, "", 1, 0, 1, 5 },   // DualSense #2 on port 2
        { DS_VID, DS_EDGE_PID, 0x03100000u, "", 1, 0, 1, 5 },   // DualSense Edge on port 3
        { DS_VID, DS_PID,      0x02100000u, "", 1, 1, 1, 5 },   // synthetic duplicate on port 2 (must be skipped)
        { DS_VID, DS_PID,      0x02100000u, "", 0, 0, 1, 5 },   // Bluetooth entry (must be skipped)
        { DS_VID, DS_PID,      0x02100000u, "", 1, 0, 12, 1 },  // non-gamepad HID usage (must be skipped)
        { DS_VID, 0x09CC,      0x04100000u, "", 1, 0, 1, 5 },   // DualShock 4 on port 4
    };
    int n = (int)(sizeof two_pads / sizeof *two_pads);

    // Selecting pad #2 matches candidate 1, not candidate 0.
    ds_audio_usb_id pad2 = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_PID, .location_id = 0x02100000u, .serial = "" };
    assert(ds_match_usb_hid(&pad2, two_pads, n) == 1);

    // Selecting pad #1 matches candidate 0.
    ds_audio_usb_id pad1 = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_PID, .location_id = 0x01100000u, .serial = "" };
    assert(ds_match_usb_hid(&pad1, two_pads, n) == 0);

    // Selecting DualSense Edge on port 3 matches candidate 2.
    ds_audio_usb_id edge = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_EDGE_PID, .location_id = 0x03100000u, .serial = "" };
    assert(ds_match_usb_hid(&edge, two_pads, n) == 2);

    // PID mismatch at same location fails closed.
    ds_audio_usb_id wrong_pid = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_EDGE_PID, .location_id = 0x02100000u, .serial = "" };
    assert(ds_match_usb_hid(&wrong_pid, two_pads, n) == -1);

    // DualShock 4 audio device fails closed even when DualSense HID devices are present.
    ds_audio_usb_id ds4 = { .has_vid_pid = 1, .vid = DS_VID, .pid = 0x09CC, .location_id = 0x04100000u, .serial = "" };
    assert(ds_match_usb_hid(&ds4, two_pads, n) == -1);

    // Sony USB headset without ModelUID on another port fails closed (never grabs pad #1).
    ds_audio_usb_id headset = { .has_vid_pid = 0, .location_id = 0x05100000u, .serial = "" };
    assert(ds_match_usb_hid(&headset, two_pads, n) == -1);

    // Missing both locationID and serial fails closed (never guesses first DualSense).
    ds_audio_usb_id unknown = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_PID, .location_id = 0, .serial = "" };
    assert(ds_match_usb_hid(&unknown, two_pads, n) == -1);

    // Unique serial fallback when locationID is unavailable, and fail closed on duplicate serials.
    const ds_hid_candidate by_serial[] = {
        { DS_VID, DS_PID, 0x01100000u, "SER-AAA", 1, 0, 1, 5 },
        { DS_VID, DS_PID, 0x02100000u, "SER-BBB", 1, 0, 1, 5 },
    };
    ds_audio_usb_id ser_b = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_PID, .location_id = 0, .serial = "SER-BBB" };
    assert(ds_match_usb_hid(&ser_b, by_serial, 2) == 1);

    // Location match with contradicting serial fails closed.
    ds_audio_usb_id ser_mismatch = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_PID, .location_id = 0x02100000u, .serial = "SER-AAA" };
    assert(ds_match_usb_hid(&ser_mismatch, by_serial, 2) == -1);

    const ds_hid_candidate dup_serial[] = {
        { DS_VID, DS_PID, 0x01100000u, "SAME", 1, 0, 1, 5 },
        { DS_VID, DS_PID, 0x02100000u, "SAME", 1, 0, 1, 5 },
    };
    ds_audio_usb_id ambig = { .has_vid_pid = 1, .vid = DS_VID, .pid = DS_PID, .location_id = 0, .serial = "SAME" };
    assert(ds_match_usb_hid(&ambig, dup_serial, 2) == -1);
}

int main(void) {
    test_state_report();
    test_parsers();
    test_matching();
    puts("DualSense USB microphone report and matching: PASS");
    return 0;
}
