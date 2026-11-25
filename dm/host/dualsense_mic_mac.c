// A DualSense's microphone over USB: the SetState report that turns it on and pure-C USB audio-to-HID correlation (dualsense_mic.h).
#include "dualsense_mic.h"
#include <ctype.h>
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

static int parse_hex_range(const char *start, const char *end, int max_digits, uint32_t *out) {
    if (!start || !end || end <= start || end - start > max_digits) return 0;
    uint32_t v = 0;
    for (const char *p = start; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

int ds_parse_model_uid_vid_pid(const char *model_uid, int *vid_out, int *pid_out) {
    if (!model_uid || !vid_out || !pid_out) return 0;
    const char *end = model_uid + strlen(model_uid);
    const char *p2 = strrchr(model_uid, ':');
    if (!p2 || p2 == model_uid) return 0;
    const char *p1 = p2 - 1;
    while (p1 > model_uid && *p1 != ':') p1--;
    if (*p1 != ':') return 0;
    uint32_t vid = 0, pid = 0;
    if (!parse_hex_range(p1 + 1, p2, 4, &vid) || !parse_hex_range(p2 + 1, end, 4, &pid)) return 0;
    *vid_out = (int)vid;
    *pid_out = (int)pid;
    return 1;
}

int ds_parse_box_uid_location(const char *box_uid, uint32_t *location_out) {
    if (!box_uid || !location_out) return 0;
    const char *at = strrchr(box_uid, '@');
    if (!at) return 0;
    uint32_t loc = 0;
    if (!parse_hex_range(at + 1, at + strlen(at), 8, &loc) || !loc) return 0;
    *location_out = loc;
    return 1;
}

int ds_parse_device_uid_location(const char *device_uid, const char *serial, uint32_t *location_out) {
    static const char prefix[] = "AppleUSBAudioEngine:";
    if (!device_uid || !location_out) return 0;
    if (serial && serial[0]) return 0;   // AppleUSBAudioEngine puts iSerialNumber in this field when present; use BoxUID or serial match instead
    size_t plen = sizeof prefix - 1;
    if (strncmp(device_uid, prefix, plen) != 0) return 0;
    const char *end = device_uid + strlen(device_uid);
    const char *last = strrchr(device_uid, ':');
    if (!last || last + 1 >= end) return 0;
    for (const char *p = last + 1; p < end; p++)
        if (!isdigit((unsigned char)*p) && *p != ',') return 0;
    const char *prev = last - 1;
    while (prev > device_uid + plen && *prev != ':') prev--;
    if (prev <= device_uid + plen || *prev != ':') return 0;
    if (!memchr(device_uid + plen, ':', (size_t)(prev - (device_uid + plen)))) return 0;   // need "<maker>:<product>" before "<location>"
    uint32_t loc = 0;
    if (!parse_hex_range(prev + 1, last, 8, &loc) || !loc) return 0;
    *location_out = loc;
    return 1;
}

int ds_match_usb_hid(const ds_audio_usb_id *audio, const ds_hid_candidate *cands, int count) {
    if (!audio || !cands || count <= 0) return -1;
    if (audio->has_vid_pid && !ds_is_dualsense_vid_pid(audio->vid, audio->pid)) return -1;
    int have_loc = audio->location_id != 0;
    int have_ser = audio->serial && audio->serial[0] != '\0';
    if (!have_loc && !have_ser) return -1;   // fail closed: never guess the first DualSense

    int found = -1;
    for (int i = 0; i < count; i++) {
        const ds_hid_candidate *c = &cands[i];
        if (!c->usb || c->synthetic) continue;
        if (!ds_is_dualsense_vid_pid(c->vid, c->pid)) continue;
        if (audio->has_vid_pid && (c->vid != audio->vid || c->pid != audio->pid)) continue;
        if (c->usage_page != 1 || (c->usage != 5 && c->usage != 4)) continue;   // GenericDesktop GamePad or Joystick
        if (have_loc) {
            if (c->location_id != audio->location_id) continue;
            if (have_ser && c->serial && c->serial[0] && strcmp(c->serial, audio->serial) != 0) continue;
        } else {
            if (!c->serial || !c->serial[0] || strcmp(c->serial, audio->serial) != 0) continue;
        }
        if (found >= 0) return -1;   // ambiguous match: fail closed
        found = i;
    }
    return found;
}
