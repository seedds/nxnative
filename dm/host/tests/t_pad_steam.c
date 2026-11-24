// Valve's first Steam Controller's reports and commands (pad_steam.h): its buttons by position, the triggers past half or through their click, the left pad as the d-pad
// only when clicked, the joystick and the left pad sharing one pair of fields, the right pad as the right stick while touched, a receiver's connect events, the command
// bytes, Steam's list of the controllers it turns into its virtual pad, and Steam's user client; the bits and the left pad's place DM_HID_LOG logs
#include <assert.h>
#include <stdio.h>
#include "../pad_steam.h"

static uint8_t r[PAD_STEAM_LEN];
static void state(uint32_t b, int lt, int rt, int x, int y, int rx, int ry) {
    memset(r, 0, sizeof r); r[0] = 1; r[2] = PAD_STEAM_MSG_STATE; r[3] = 0x3c;
    r[8] = (uint8_t)b; r[9] = (uint8_t)(b >> 8); r[10] = (uint8_t)(b >> 16); r[11] = (uint8_t)lt; r[12] = (uint8_t)rt;
    int v[4] = {x, y, rx, ry};
    for (int i = 0; i < 4; i++) { r[16 + 2 * i] = (uint8_t)v[i]; r[17 + 2 * i] = (uint8_t)((uint16_t)v[i] >> 8); }
}
static pad_steam p;
static uint64_t m; static int32_t s[4];
static uint64_t press(uint32_t b) { state(b, 0, 0, 0, 0, 0, 0); assert(pad_steam_state(&p, r, sizeof r, &m, s)); return m; }

int main(void) {
    // not a state report: the battery (as a wired controller sent it), a receiver's event, a short or foreign report
    static const uint8_t battery[16] = {0x01, 0x00, 0x04, 0x0b, 0x9f, 0x17, 0, 0, 0, 0, 0, 0, 0x88, 0x14, 0x64};
    assert(!pad_steam_state(&p, battery, sizeof battery, &m, s));
    state(PAD_STEAM_A, 0, 0, 0, 0, 0, 0); assert(!pad_steam_state(&p, r, 20, &m, s));
    r[0] = 2; assert(!pad_steam_state(&p, r, sizeof r, &m, s));

    // face buttons by position (right A, bottom B, top X, left Y), the rest by name
    assert(press(PAD_STEAM_B) == DM_BTN_A && press(PAD_STEAM_A) == DM_BTN_B && press(PAD_STEAM_Y) == DM_BTN_X && press(PAD_STEAM_X) == DM_BTN_Y);
    assert(press(PAD_STEAM_LB) == DM_BTN_L && press(PAD_STEAM_RB) == DM_BTN_R && press(PAD_STEAM_START) == DM_BTN_PLUS && press(PAD_STEAM_BACK) == DM_BTN_MINUS);
    assert(press(PAD_STEAM_STICK_CLICK) == DM_BTN_LS && press(PAD_STEAM_RPAD_CLICK | PAD_STEAM_RPAD_TOUCH) == DM_BTN_RS);
    assert(press(PAD_STEAM_LT) == DM_BTN_ZL && press(PAD_STEAM_RT) == DM_BTN_ZR);   // pulled through the click
    assert(press(PAD_STEAM_STEAM | PAD_STEAM_LGRIP | PAD_STEAM_RGRIP) == 0);          // no Switch button
    // analog triggers press ZL / ZR from half on
    state(0, 127, 127, 0, 0, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(m == 0);
    state(0, 128, 255, 0, 0, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(m == (DM_BTN_ZL | DM_BTN_ZR));

    // the left pad: its direction bits are the d-pad only with its click; touching it is no press
    assert(press(PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_UP) == 0);
    assert(press(PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_CLICK | PAD_STEAM_LPAD_UP) == DM_BTN_UP);
    assert(press(PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_CLICK | PAD_STEAM_LPAD_DOWN | PAD_STEAM_LPAD_LEFT) == (DM_BTN_DOWN | DM_BTN_LEFT));
    assert(press(PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_CLICK | PAD_STEAM_LPAD_RIGHT) == DM_BTN_RIGHT);
    // older firmware: the joystick's click as the left pad's, with the pad untouched
    assert(press(PAD_STEAM_LPAD_CLICK) == DM_BTN_LS);
    // the bits and the thumb's place DM_HID_LOG logs: the last report's, a thumb on the left pad in either report of the pair that take turns
    state(PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_CLICK, 0, 0, 1200, 25000, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s);
    assert(p.bits == (PAD_STEAM_LPAD_TOUCH | PAD_STEAM_LPAD_CLICK) && p.lpad[0] == 1200 && p.lpad[1] == 25000);
    state(PAD_STEAM_LPAD_AND_STICK, 0, 0, 0, 32767, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s);
    assert(p.bits == (PAD_STEAM_LPAD_AND_STICK | PAD_STEAM_LPAD_TOUCH) && p.lpad[1] == 25000);

    // the joystick: up is positive, as on the Switch; the raw HID dead zone at rest
    state(0, 0, 0, 32767, 0, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[0] == DM_STICK_MAX && s[1] == 0 && s[2] == 0 && s[3] == 0);
    state(0, 0, 0, 0, -32768, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[0] == 0 && s[1] == -DM_STICK_MAX);
    state(0, 0, 0, 2000, -2000, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[0] == 0 && s[1] == 0);
    // the left pad alone: its position is in the fields, the joystick at rest
    state(0, 0, 0, 30000, 0, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[0] > 0);
    state(PAD_STEAM_LPAD_TOUCH, 0, 0, -30000, 20000, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[0] == 0 && s[1] == 0);
    // both: the reports take turns, a left pad report keeps the joystick
    state(PAD_STEAM_LPAD_AND_STICK, 0, 0, 0, 32767, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[1] == DM_STICK_MAX);
    state(PAD_STEAM_LPAD_AND_STICK | PAD_STEAM_LPAD_TOUCH, 0, 0, -30000, -30000, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[0] == 0 && s[1] == DM_STICK_MAX);
    state(0, 0, 0, 0, 0, 0, 0); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[1] == 0);

    // the right pad: where the thumb is while it touches, at rest otherwise (whatever the fields hold)
    state(PAD_STEAM_RPAD_TOUCH, 0, 0, 0, 0, -32767, 32767); pad_steam_state(&p, r, sizeof r, &m, s);
    assert(s[2] < -DM_STICK_MAX / 2 && s[3] > DM_STICK_MAX / 2 && m == 0);
    state(PAD_STEAM_RPAD_TOUCH, 0, 0, 0, 0, 1500, -1500); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[2] == 0 && s[3] == 0);   // its centre
    state(0, 0, 0, 0, 0, 20000, 20000); pad_steam_state(&p, r, sizeof r, &m, s); assert(s[2] == 0 && s[3] == 0);

    // a receiver's events
    uint8_t ev[64] = {1, 0, PAD_STEAM_MSG_WIRELESS, 1, 2};
    assert(pad_steam_wireless(ev, sizeof ev) == 1);
    ev[4] = 1; assert(pad_steam_wireless(ev, sizeof ev) == -1);
    ev[4] = 3; assert(pad_steam_wireless(ev, sizeof ev) == 0);
    assert(pad_steam_wireless(battery, sizeof battery) == 0);

    // commands: the command, the length of its data, three bytes a setting
    uint8_t c[PAD_STEAM_LEN];
    pad_steam_cmd(c, PAD_STEAM_CMD_CLEAR_MAPPINGS); assert(c[0] == 0x81 && c[1] == 0);
    static const uint8_t gamepad[] = {0x87, 12, 49, 2, 0, 7, 7, 0, 8, 7, 0, 24, 0, 0};
    pad_steam_gamepad_settings(c); assert(!memcmp(c, gamepad, sizeof gamepad) && c[sizeof gamepad] == 0);
    static const uint8_t lizard[] = {0x87, 3, 8, 0, 0};
    pad_steam_lizard_settings(c); assert(!memcmp(c, lizard, sizeof lizard));

    // the controllers Steam turns into its virtual pad (SDL's list), and Steam's user client
    assert(pad_steam_listed("0x28de/0x1102", PAD_STEAM_VID, PAD_STEAM_PID_WIRED));
    assert(pad_steam_listed("0x045e/0x028e,0x28DE/0x1142", PAD_STEAM_VID, PAD_STEAM_PID_RECEIVER));
    assert(!pad_steam_listed("0x045e/0x028e,0x28DE/0x1142", PAD_STEAM_VID, PAD_STEAM_PID_WIRED));
    assert(!pad_steam_listed(NULL, PAD_STEAM_VID, PAD_STEAM_PID_WIRED) && !pad_steam_listed("", PAD_STEAM_VID, PAD_STEAM_PID_WIRED));
    assert(!pad_steam_listed("@/tmp/list,zz,0x28de", PAD_STEAM_VID, PAD_STEAM_PID_WIRED) && pad_steam_listed("zz, 0x28de/0x1102", PAD_STEAM_VID, PAD_STEAM_PID_WIRED));
    assert(pad_steam_creator("pid 56019, steam_osx") && !pad_steam_creator("pid 1, steam_osx2") && !pad_steam_creator("pid 2, Steam Helper") && !pad_steam_creator(NULL));
    assert(pad_steam_is(0x28de, 0x1102) && pad_steam_is(0x28de, 0x1142) && !pad_steam_is(0x28de, 0x1106) && !pad_steam_is(0x045e, 0x028e));
    printf("t_pad_steam: ok\n");
    return 0;
}
