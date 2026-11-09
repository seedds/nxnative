// Front end (Mac keyboard / iOS touchscreen) -> nn::hid input state. Button bits match nn::hid::NpadButton:
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; sticks ±32767.
#pragma once
#include <stdint.h>
int dm_players_reported(void);   // upstream joining count; deliberately separate from editor readiness
// Button bits (= nn::hid::NpadButton bit order; the DM_KEYS key-name table in hle_hid.c is generated from here). Frontends always compose buttons from these names.
enum {
    DM_BTN_A = 1u << 0, DM_BTN_B = 1u << 1, DM_BTN_X = 1u << 2, DM_BTN_Y = 1u << 3,
    DM_BTN_LS = 1u << 4, DM_BTN_RS = 1u << 5, DM_BTN_L = 1u << 6, DM_BTN_R = 1u << 7,
    DM_BTN_ZL = 1u << 8, DM_BTN_ZR = 1u << 9, DM_BTN_PLUS = 1u << 10, DM_BTN_MINUS = 1u << 11,
    DM_BTN_LEFT = 1u << 12, DM_BTN_UP = 1u << 13, DM_BTN_RIGHT = 1u << 14, DM_BTN_DOWN = 1u << 15,
    // The rail buttons of a physical or virtual single Joy-Con (NpadButton 24..27).
    DM_BTN_LSL = 1u << 24, DM_BTN_LSR = 1u << 25, DM_BTN_RSL = 1u << 26, DM_BTN_RSR = 1u << 27,
};
#define DM_BTN_SHAKE (1ull << 63)   // not an NpadButton: the controller is shaken while set (its six-axis sensor, hle_hid.c); never reaches the game's buttons
#define DM_STICK_MAX 32767
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
uint64_t dm_input_samples(void);   // Cumulative number of times the game has sampled the controller
// The cheat VM's key mask (hle_cheats.c): every input source's button bits (0..15 in the enum above) ORed and the
// merged axes' eight stick-direction bits (16..23), one flat bitset; no latches are consumed (the game's own reads
// keep them). SL / SR bits (24..27) pass through when a source supplies them. Virtual single Joy-Con layout conversion happens in the game-facing state reader
uint64_t dm_cheats_keys(void);
// Physical controller path (Mac / iOS GameController): stored separately from keyboard / touch and scripted input (DM_KEYS / DM_CTL); when the game reads player 1,
// buttons are ORed bitwise and each stick axis takes whichever source is deflected more. All 0 when no controller is connected.
// dm_pad_set is player 1's controller (DM_BTN_SHAKE counts there only); dm_pad_set_player sets player n + 1's, which the game reads on its own npad (hle_hid.c) while
// dm_pad_players has its bit set; a player whose npad the game does not support (SetSupportedNpadIdType) feeds player 1 instead, so a one-player game sees every controller
#define DM_PLAYERS 8   // npads No1..No8; player 1 is the Handheld npad in handheld mode (DM_OPMODE=0)
enum { DM_STYLE_AUTO = 0, DM_STYLE_PRO = 1, DM_STYLE_PAIR = 4, DM_STYLE_LEFT = 8, DM_STYLE_RIGHT = 16 };
// Explicit game-facing styles. AUTO retains the platform's legacy docked / handheld behavior.
// Called before guest startup, or inside a live reconfiguration transaction. Device ownership stays with the front end.
// follow_hold adapts a full-size pad assigned as a single Joy-Con to the game's hold type.
void dm_pad_style(int player, uint32_t style, int follow_hold);
// Explicit two-source Joy-Con pair. Role 1 is left, 2 right; mask 0 disables grouping.
// A grouped player is connected only with both roles present. Single-pad pair emulation stays shared.
void dm_pad_pair_sources(int player, int grouped, uint32_t sides);
void dm_pad_set_side(int player, int role, uint64_t buttons, int32_t lx, int32_t ly);
// Full-size member of a two-source pair: preserve mapped button meanings across
// both virtual halves. The primary stick, Shake and motion retain the source role.
void dm_pad_set_pair_gamepad(int player, int role, uint64_t buttons, int32_t lx, int32_t ly);
void dm_motion_set_side_generation(int player, int role, uint64_t generation, const float acc[3], const float rot[3]);
void dm_motion_set_side_sample(int player, int role, uint64_t generation, const float acc[3], const float rot[3], uint64_t timestamp);
void dm_motion_clear_side(int player, int role);
void dm_pad_explicit(int enabled);   // explicit player slots never merge into player 1
uint32_t dm_pad_configured_style(int player);
void dm_pad_supported(uint32_t *styles, uint32_t *ids);
// Effective limits while controller-support requests are pending. Outside an
// applet this returns the game's ordinary styles/IDs and the range 0..8.
void dm_controller_setup_requirements(uint32_t *styles, uint32_t *ids, int *minimum, int *maximum);

// A live reassignment holds this barrier across detach, state clearing and reattach.
// Guest readers see either complete configuration. Only affected players lose motion history.
void dm_pad_reconfigure_begin(void);
void dm_pad_reconfigure_reset(uint32_t affected);
void dm_pad_reconfigure_sources(uint32_t affected);   // invalidate sources without changing the reported controller layout
void dm_pad_reconfigure_end(void);
void dm_motion_suspend(int suspended);   // temporarily hide live motion while controller setup is open
void dm_input_quiesce(void);   // discard physical buttons, sticks and press latches while a native input UI is open
uint64_t dm_pad_generation(int player);
void dm_motion_set_generation(int player, uint64_t generation, const float acc[3], const float rot[3]);
// Frontends install the native editor bridge before guest startup. The joining
// popup has priority; this fallback is called only for an incompatible setup.
// Returns 1 for Apply, 0 for Cancel, -1 when unavailable (legacy/headless behavior).
extern int (*dm_controller_support_hook)(int minimum, int maximum);
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void dm_pad_set_player(int player, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);   // player 0..DM_PLAYERS-1 (player 1..8)
void dm_pad_players_ios(uint32_t mask, uint32_t manual);   // preserve automatic routing for slots without a manual override
void dm_pad_players(uint32_t mask);   // bit p: player p + 1 has a controller; player 1 has keyboard / touch fallback unless an explicit physical controller was selected there
// Live controller motion (hle_hid.c, motion_fusion.h): player 0..DM_PLAYERS-1's gyro and accelerometer as GameController reports them, acceleration in G (gravity direction,
// (0, 0, -1) face up) and rotation rate in rad/s, in Apple's controller frame. While samples keep coming the player's six-axis sensor follows the controller; dm_motion_clear stops it
void dm_motion_set(int player, const float acc[3], const float rot[3]);
void dm_motion_set_switch(int player, const float acc[3], const float rot[3]);   // player 0 only: the phone's own sensors in the Switch frame, kept separate; DM_MOTION_AXES does not apply
void dm_motion_clear(int player);   // clears GameController motion; another fresh source can take over
// Passive controller HID, already in the Switch frame (G, rad/s). sample_ns is the sample's host now_ns() time, not the packet's arrival; stale / reordered samples are ignored.
// Kept separate from GameController and the phone; used while GameController supplies no fresh motion for that player. DM_MOTION_AXES / ACC_SIGN apply only to GameController.
void dm_motion_set_hid(int player, const float acc[3], const float rot[3], uint64_t sample_ns);
void dm_motion_clear_hid(int player);
// Joy-Cons as devices of their own (a front end that reads Joy-Cons one by one: Switch 2 Joy-Cons, pad_switch2_apple.m): pad p (the same index as dm_pad_set_player's player) holds a left Joy-Con (sides bit 0), a right one (bit 1) or both;
// sides 0 is a Pro-style pad again. Each Joy-Con reports its own buttons (NpadButton bits, including the rail buttons) and stick, and its own sensor; the pad's merged state of dm_pad_set_player
// stays the fallback for a game that supports no Joy-Con style. The game decides how the Joy-Cons of a pair are used (dual: one npad with two sensors; single: two npads, hle_hid.c)
void dm_joy_attach(int pad, uint32_t sides);   // sides | DM_JOY_2: they are Joy-Con 2s (the Switch 2 styles, the mouse sensor)
#define DM_JOY_2 4u
void dm_joy_set(int pad, int side, uint64_t buttons, int32_t sx, int32_t sy);   // side 0 left, 1 right
void dm_motion_set_joy(int pad, int side, const float acc[3], const float rot[3], uint64_t sample_ns);   // like dm_motion_set_hid, for one Joy-Con's sensor
void dm_motion_clear_joy(int pad, int side);
// A Joy-Con's mouse sensor (the Joy-Con 2's, on its rail side): the movement since the last report, in sensor counts (x right, y toward the player), and its state (DM_JOYMOUSE_*).
// The game reads it as that Joy-Con's JcmsState ring (hle_hid.c); nothing is reported for a Joy-Con that never calls this
enum { DM_JOYMOUSE_CONNECTED = 1u << 0, DM_JOYMOUSE_ON_SURFACE = 1u << 1 };
void dm_joy_mouse(int pad, int side, int32_t dx, int32_t dy, uint32_t state);
int dm_joy_mouse_wanted(void);   // 1 once the game has set up the mouse sensors or pointer (InitializeJcms / InitializeNmouse): a front end turns a Joy-Con's sensor on only then. Any thread
uint32_t dm_player_npad(int player);  // the npad id the game reads player p + 1 on: Handheld (0x20) or No1 (0) for player 1, No n (id n - 1) for player n
// A library applet in front of the game (the offline web page, hle_web.c / web_apple.m): while held, the game reads no buttons and centred sticks on every npad, as an application
// out of focus does, and presses made meanwhile are dropped when the hold ends. dm_input_peek reads what the applet sees: every path ORed (any controller drives it), each stick
// axis the most deflected one, plus the presses since the last peek (a tap between two peeks is reported once). Any thread
void dm_input_hold(int on);
void dm_input_peek(uint64_t *buttons, int32_t axes[4]);

// Optional diagnostic DM_HID_RECEIPT_LOG=<writable JSONL path>: received samples of real GetNpadState returns.
// Fields read_start_ns/time_ns use the same host now_ns(); the front-end publish window must use the same helper, and a read interval that spans a publish must not be force-matched.
// State changes + at most one activity witness per second, limit_reached after 1024 entries; no file is written when the environment variable is not set.
// The actual return includes id/style/sampling/buttons/four axes/attributes/script_mask; front-end output is not treated as received.
// No new line does not prove that a blank touch point was read; a valid connection / sampling activity / evidence from the original UI and command packets is required; the log has IO overhead and is not used for timing.

// Private skill-aim-v1: takes only paired received samples of the right axis that GetNpad has actually returned, not values the front end has not yet had consumed.
void dm_input_sampled_right(int32_t *rx,int32_t *ry);

// Controller state injection point: called after GetNpadState has filled in the original state and before the pseudo-key override. The Mac host touch point script (touch_mac.c, Mac only, kept out of the iOS app by the _mac convention) registers at startup; iOS does not carry that file and it stays empty
extern void (*dm_pad_hook)(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
