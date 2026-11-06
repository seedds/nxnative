// Players and controllers (GitHub issue #74): the Mac front end's slot assignment (pad_slots.h) and the npads hle_hid.c reports for each player, docked and handheld, with the
// game's supported npad ids; no controller, window or guest exists in this test (the hid take-overs are called directly, events are recorded by the stubs)
#include <time.h>
#include <unistd.h>
#include <sched.h>
static int hid_clock_gettime(clockid_t clock, struct timespec *ts);
static int hid_usleep(useconds_t usec);
#define clock_gettime hid_clock_gettime
#define usleep hid_usleep
#include "../hle_hid.c"
#undef clock_gettime
#undef usleep
#include "../pad_slots.h"
void dm_oe_execute(int index, const void *param, size_t n) { (void)index; (void)param; (void)n; }   // DM_CTL "program", "webshot": not exercised here
void front_web_shot(const char *path) { (void)path; }

static int checks, fails, g_mode = 1, g_signals;
static void *g_signalled;
static time_t rumble_second = 1;
static uint64_t motion_now = 1000000000ull;
static void (*script_hold)(void);
static int hid_clock_gettime(clockid_t clock, struct timespec *ts) { (void)clock; *ts = (struct timespec){.tv_sec = rumble_second}; return 0; }
static int hid_usleep(useconds_t usec) { (void)usec; if (script_hold) script_hold(); return 0; }
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
int dm_opmode(void) { return g_mode; }
uint64_t guest_ns(void) { return 1000000000ull; }
uint64_t now_ns(void) { return motion_now; }
static uint64_t test_frame;   // presented frames (t_rec moves it)
uint64_t dm_guest_frame(void) { return test_frame; }
// The touchscreen as the game would read it: the first finger, and whether one was put down since the last read (a touch's Start is reported once, hle_touch.c)
static int touch_down, touch_new, touch_downs; static float touch_x, touch_y;
static void touch_to(const dm_touch *t, int n) { if (n > 0) { touch_new |= !touch_down; touch_downs += !touch_down; touch_x = t[0].x; touch_y = t[0].y; } touch_down = n > 0; }
void (*_Atomic dm_touch_observer)(const dm_touch *t, int n);
void dm_touch_set(const dm_touch *t, int n) { touch_to(t, n); void (*o)(const dm_touch *, int) = atomic_load(&dm_touch_observer); if (o) o(t, n); }
void dm_touch_script(const dm_touch *t, int n) { touch_to(t, n); }
int touch_fill_n(void *out, int max, uint64_t now) { (void)out; (void)max; (void)now; return 0; }
int gesture_fill(void *out, int n, uint64_t now) { (void)out; (void)n; (void)now; return 0; }
void dm_gfx_shot_at(long frame, const char *path) { (void)frame; (void)path; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { g_signalled = g; g_signals++; }
int hle_signal_bound_system_event(void *g) { g_signalled = g; g_signals++; return 1; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_restop(void) {}
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_ctl_peek(uint64_t a, int n) { (void)a; (void)n; }
void crash_dump_now(const char *why) { (void)why; }
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) { (void)a; (void)n; (void)path; }
void dm_ctl_cvar_i32(uint64_t off, int32_t v) { (void)off; (void)v; }
void dm_ctl_cvar_f32(uint64_t off, float v) { (void)off; (void)v; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
static int applet_depth, applet_opens, applet_closes_at_depth0;   // hle_oe.c: the applet stand-ins open and close their applet around the answer
void dm_applet_enter(const char *name) { (void)name; applet_depth++; applet_opens++; }
void dm_applet_leave(void) { applet_closes_at_depth0 += --applet_depth == 0; }

static uint64_t buttons_of(uint32_t id, uint32_t style) { NpadState s; fill(&s, id, style); return s.attributes ? s.buttons & 0xffff : 0; }
static int32_t lx_of(uint32_t id) { NpadState s; fill(&s, id, STYLE_FULLKEY); return s.lx; }
static void ids(const uint32_t *v, uint64_t n) { h_hid_SetIds(v, n); }

static void t_slots(void) {
    pad_slots s; memset(&s, 0, sizeof s);
    static char a, b, c, d, b2, kbd;   // controllers stand in as distinct addresses
    CHECK(pad_slot_take(&s, &a, "Xbox Wireless Controller") == 0, "the first controller is player 1");
    CHECK(pad_slot_take(&s, &b, "DualSense Wireless Controller") == 1, "the second controller is player 2");
    CHECK(pad_slot_take(&s, &a, "Xbox Wireless Controller") == 0, "taking again keeps the slot");
    CHECK(pad_slot_mask(&s) == 0x3, "two slots held");
    CHECK(pad_slot_free(&s, &b) == 1 && pad_slot_of(&s, &b) < 0, "a disconnected controller frees its slot");
    CHECK(pad_slot_take(&s, &c, "Pro Controller") == 2, "another controller does not take the slot kept for the one that went away");
    CHECK(pad_slot_take(&s, &b2, "DualSense Wireless Controller") == 1, "the same controller reconnected gets its player back");
    CHECK(pad_slot_free(&s, &a) == 0, "player 1's controller goes away");
    CHECK(pad_slot_take(&s, &d, "8BitDo") == 0, "a new controller fills player 1 first");
    CHECK(pad_slot_free(&s, &d) == 0 && pad_slot_take(&s, &a, "Xbox Wireless Controller") == 0, "player 1's slot goes back to its name before any other");
    static char many[DM_PLAYERS];
    for (int i = 0; i < DM_PLAYERS - 3; i++) CHECK(pad_slot_take(&s, &many[i], "Pro Controller") == 3 + i, "the lowest unused slot");
    CHECK(pad_slot_mask(&s) == (1u << DM_PLAYERS) - 1, "every slot held");
    CHECK(pad_slot_take(&s, &many[DM_PLAYERS - 3], "Pro Controller") == -1, "a ninth controller gets no player");
    CHECK(pad_slot_take(&s, NULL, "x") == -1 && pad_slot_free(&s, NULL) == -1, "no owner, no slot");
    memset(&s, 0, sizeof s);   // DM_KBD_PLAYER=own: the keyboard holds player 1 for good
    CHECK(pad_slot_take(&s, &kbd, "keyboard") == 0 && pad_slot_take(&s, &a, "Xbox Wireless Controller") == 1, "with the keyboard on its own the first controller is player 2");
    memset(&s, 0, sizeof s);
    CHECK(pad_slot_assign(&s, &a, "same name", 5) == 5, "explicit assignment selects the requested slot");
    CHECK(pad_slot_assign(&s, &b, "same name", 5) == -1 && pad_slot_of(&s, &a) == 5, "an occupied explicit slot is not stolen by a same-named device");
    CHECK(pad_slot_assign(&s, &a, "same name", 2) == 2 && !s.owner[5], "moving an explicit owner releases its previous slot");
    CHECK(pad_slot_assign(&s, &b, "same name", -1) == -1 && pad_slot_assign(&s, &b, "same name", DM_PLAYERS) == -1, "invalid explicit assignments do not take a slot");
}

static void t_npads(void) {
    // One player (the keyboard, or one controller): as before, everything is player 1 on No1 docked
    g_mode = 1; dm_pad_players(0);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){1}) == 0 && h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == 0, "one player: No1 only");
    CHECK(dm_player_npad(0) == 0 && dm_player_npad(1) == 1 && dm_player_npad(7) == 7, "player n on No n docked");
    dm_input_set(DM_BTN_X, 100, 0, 0, 0); dm_pad_set(DM_BTN_A, -200, 0, 0, 0);
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_X | DM_BTN_A) && lx_of(0) == -200, "keyboard and player 1's controller are ORed, the stick deflected more wins");
    dm_input_set(0, 0, 0, 0, 0); dm_pad_set(0, 0, 0, 0, 0); buttons_of(0, STYLE_FULLKEY);

    // Two controllers: player 2 on No2 with its own input, the style event of No2 signalled
    static uint8_t ev1[0x40];
    h_hid_BindStyleEv(&(uint32_t){1}, ev1, 0);
    CHECK(g_signals == 0, "no signal on binding an npad with no controller");
    dm_pad_players(0x3);
    CHECK(g_signals == 1 && g_signalled == ev1, "No2's style event signalled when player 2's controller connects");
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY, "player 2 is a full-key controller on No2");
    dm_pad_set_player(1, DM_BTN_B, 300, 0, 0, 0); dm_pad_set(DM_BTN_Y, 0, 0, 0, 0);
    CHECK(buttons_of(1, STYLE_FULLKEY) == DM_BTN_B && lx_of(1) == 300, "player 2 reads its own controller");
    CHECK(buttons_of(0, STYLE_FULLKEY) == DM_BTN_Y && lx_of(0) == 0, "player 1 does not see player 2");
    CHECK(buttons_of(1, STYLE_HANDHELD) == 0, "player 2 is not a handheld controller");
    CHECK(h_hid_Led(&(uint32_t){0}) == 0x1 && h_hid_Led(&(uint32_t){1}) == 0x3 && h_hid_Led(&(uint32_t){7}) == 0x6, "player LED patterns");
    int32_t info[2]; int8_t arg[8] = {1, 4};
    CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 2 && info[1] == 0, "the controller applet reports two players, player 1 selected");
    CHECK(applet_opens == 1 && applet_depth == 0 && applet_closes_at_depth0 == 1, "the controller applet opens and closes once around its answer (focus 2, then 1)");
    arg[1] = 1; h_hid_ShowCtrl(info, arg);
    CHECK(info[0] == 1, "the applet's count is capped at the game's maximum");
    arg[0] = 3; arg[1] = 4;
    CHECK(h_hid_ShowCtrl(info, arg) == RES_CTRL_CANCELED && info[0] == 2, "fewer players than the game's minimum: canceled (2202-3101), as the player closing the applet");
    CHECK(h_hid_ShowCtrl0(arg) == RES_CTRL_CANCELED, "the overload without a result: canceled too");
    arg[0] = 2; CHECK(h_hid_ShowCtrl0(arg) == RES_OK, "the overload without a result: OK when the minimum is met");
    CHECK(h_hid_Strap() == RES_OK && h_hid_StrapArg(arg) == RES_OK, "both strap guide overloads answer as the player closing the guide");
    CHECK(applet_opens == 7 && applet_depth == 0, "every answer opened and closed its applet");
    arg[0] = 1;
    CHECK(six_connected(0x20103) && !six_connected(0x20203), "player 2 has a six-axis sensor, No3 none");

    // A one-player game ({No1, Handheld}): player 2's controller plays as player 1, No2 reads as not connected
    g_signals = 0; ids((const uint32_t[]){0, NPAD_HANDHELD}, 2);
    CHECK(g_signals == 1 && g_signalled == ev1, "No2's style event signalled when the game stops supporting it");
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == 0 && buttons_of(1, STYLE_FULLKEY) == 0, "No2 not reported");
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_Y | DM_BTN_B) && lx_of(0) == 300, "player 2's controller feeds player 1");
    h_hid_ShowCtrl(info, NULL);
    CHECK(info[0] == 1, "the applet reports one player");

    // A four-player game: a fifth controller is not used, nor merged into player 1
    ids((const uint32_t[]){0, 1, 2, 3, NPAD_HANDHELD}, 5);
    dm_pad_players(0x1f); dm_pad_set_player(4, DM_BTN_ZR, 0, 0, 0, 0);
    CHECK(h_hid_GetStyle(&(uint32_t){3}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){4}) == 0, "players 1..4 reported, player 5 not");
    CHECK(!(buttons_of(0, STYLE_FULLKEY) & DM_BTN_ZR), "player 5 does not feed player 1 in a game for four");
    h_hid_ShowCtrl(info, NULL);
    CHECK(info[0] == 4, "the applet reports four players");

    // Player 2 goes away: No2 disconnected, its input released
    dm_pad_players(0x1);
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == 0 && atomic_load(&g_pad_btn[1]) == 0, "a disconnected player's npad and input are cleared");

    // Handheld mode: player 1 on Handheld, player 2 on No2 (No1 stays empty)
    g_mode = 0; dm_pad_players(0x3); dm_pad_set_player(1, DM_BTN_L, 0, 0, 0, 0);
    CHECK(dm_player_npad(0) == NPAD_HANDHELD && dm_player_npad(1) == 1, "handheld: player 1 on Handheld, player 2 on No2");
    CHECK(h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == STYLE_HANDHELD && h_hid_GetStyle(&(uint32_t){0}) == 0 && h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY, "handheld styles");
    CHECK(buttons_of(NPAD_HANDHELD, STYLE_HANDHELD) == DM_BTN_Y && buttons_of(1, STYLE_FULLKEY) == DM_BTN_L, "handheld: each player its own input");
    h_hid_ShowCtrl(info, NULL);
    CHECK(info[0] == 2 && info[1] == NPAD_HANDHELD, "handheld: the applet selects the handheld controller");
}

static dm_haptic_state vibration[DM_PLAYERS];
static int vibration_calls;
static void vibration_sink(dm_haptic_state s) { vibration[s.player] = s; vibration_calls++; }
static void t_vibration(void) {
    uint32_t h[2] = {0}, id = 0, info[2];
    dm_vibration v = {0.25, 160, 0.5, 320}, got;
    g_mode = 1; ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3); dm_pad_players(3);
    CHECK(!h_hid_VibPermit() && !h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY), "no sink: no vibration devices");
    dm_haptics_init(vibration_sink, 1);
    CHECK(!h_hid_VibPermit() && h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY) == 2,
          "startup handles do not wait for a routed haptics output");
    CHECK(!h_hid_VibMounted(&h[0]), "no routed output: device not mounted");
    h_hid_SendVib(&h[0], &v);
    CHECK(dm_haptics_actual(0, 0).amplitude_low == 0, "a value before routing is not retained");
    dm_haptics_route(0, 1); dm_haptics_route(1, 1);
    h_hid_SendVib(&h[0], &v);
    CHECK(!memcmp(&vibration[0].motor[0], &v, sizeof v), "cached startup handles reach an output routed later");
    CHECK(h_hid_VibPermit() && h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY) == 2, "two vibration motors available");
    CHECK(h[0] == 3 && h[1] == 0x10003, "SDK vibration handle bytes: style 3, npad 0, motors 0 and 1");
    CHECK(!h_hid_VibHandles(h, 2, &id, STYLE_HANDHELD), "wrong controller style gets no motors");
    CHECK(!h_hid_VibHandles(h, 2, &(uint32_t){2}, STYLE_FULLKEY), "disconnected player gets no motors");
    CHECK(h_hid_VibHandles(h, 1, &id, STYLE_FULLKEY) == 1, "handle count respects caller capacity");
    h_hid_SendVib(&h[0], &v); h_hid_VibActual(&got, &h[0]);
    CHECK(!memcmp(&got, &v, sizeof v) && vibration[1].motor[0].amplitude_low == 0, "game value is retained for the addressed player only");
    h_hid_VibInfo(info, &h[1]); CHECK(info[0] == 1 && info[1] == 2, "right motor is a right LRA");
    CHECK(h_hid_VibMounted(&h[0]) && h_hid_VibMounted(&h[1]), "both motors of a routed controller are mounted");
    uint32_t invalid[] = {0, 0x20003, 0x1000003, 0x203};
    int mounted = 0;
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) mounted |= h_hid_VibMounted(&invalid[i]);
    CHECK(!mounted && !h_hid_VibMounted(NULL), "invalid style, motor, reserved byte, disconnected player and no handle are not mounted");
    vibration_calls = 0;
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) h_hid_SendVib(&invalid[i], &v);
    CHECK(!vibration_calls, "invalid style, motor, reserved byte and disconnected player are ignored");
    dm_vibration pair[] = {{0.1, 160, 0, 320}, {0.6, 160, 0, 320}};
    h_hid_SendVibs(h, pair, 2);
    CHECK(vibration_calls == 1 && vibration[0].motor[0].amplitude_low == 0.1f && vibration[0].motor[1].amplitude_low == 0.6f, "HID batch publishes one complete pair");
    h_hid_InitVib(&h[0]);
    CHECK(dm_haptics_actual(0, 0).amplitude_low == 0 && dm_haptics_actual(0, 1).amplitude_low == 0.6f, "initializing one motor leaves its partner intact");
    ids((uint32_t[]){0, NPAD_HANDHELD}, 2);
    h_hid_SendVib(&h[0], &v);
    CHECK(dm_haptics_actual(1, 0).amplitude_low == 0.25f, "one-player rumble reaches controllers whose input is merged into player 1");
    dm_haptics_route(0, 0);
    CHECK(h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY) == 2, "merged controller supplies rumble when player 1 has no physical output");
    g_mode = 0; id = NPAD_HANDHELD;
    CHECK(h_hid_VibHandles(h, 2, &id, STYLE_HANDHELD) == 2 && h[0] == 0x2004 && h[1] == 0x12004, "handheld SDK vibration handles");
    CHECK(!vib_targets(3), "old docked handle is not valid in handheld mode");
    CHECK(!h_hid_VibMounted(&(uint32_t){3}) && h_hid_VibMounted(&h[0]), "mounted follows the connected controller's style");
    dm_haptics_route(1, 0);
    CHECK(!h_hid_VibPermit(), "disconnecting the last output disables vibration");
    dm_haptics_init(vibration_sink, 0); dm_haptics_route(0, 1);
    CHECK(!h_hid_VibPermit() && h_hid_VibHandles(h, 2, &id, STYLE_HANDHELD) == 2, "rumble disabled: not permitted, but the handles exist, as on a Switch");
    dm_haptics_set_enabled(1);
    CHECK(h_hid_VibPermit(), "rumble turned on while the game runs: the handles it took work");
}

static void *vibration_sender(void *arg) {
    (void)arg;
    const uint32_t handles[] = {3, 0};
    const dm_vibration values[] = {{0.25, 160, 0, 320}, {0.8, 160, 0, 320}};
    for (int i = 0; i < 1000; i++) h_hid_SendVibs(handles, values, 2);
    return NULL;
}
static void t_rumble_log(void) {
    g_mode = 1; ids((uint32_t[]){0}, 1); dm_pad_players(1);
    dm_haptics_init(vibration_sink, 1); dm_haptics_route(0, 1);
    FILE *capture = tmpfile(); int saved = dup(STDERR_FILENO);
    CHECK(capture && saved >= 0, "capture rumble diagnostics");
    if (!capture || saved < 0) {
        if (capture) fclose(capture);
        if (saved >= 0) close(saved);
        unsetenv("DM_RUMBLE_LOG"); return;
    }
    fflush(stderr); int redirected = dup2(fileno(capture), STDERR_FILENO);
    CHECK(redirected >= 0, "redirect rumble diagnostics");
    if (redirected >= 0) {
        // Start a reporting interval, then send a batch with one routed and one invalid handle.
        rumble_second++;
        h_hid_SendVib(&(uint32_t){3}, &(dm_vibration){0, 160, 0, 320});
        rumble_second++;
        vibration_sender(NULL);
        rumble_second++;
        h_hid_SendVib(&(uint32_t){3}, &(dm_vibration){0, 160, 0, 320});
        fflush(stderr); dup2(saved, STDERR_FILENO);
        rewind(capture); char log[1024]; size_t n = fread(log, 1, sizeof log - 1, capture); log[n] = 0;
        CHECK(strstr(log, "2 values sent, 1 with no output, peak amplitude 0.80") != NULL,
              "a mixed batch counts unroutable values individually");
        CHECK(strstr(log, "1999 values sent, 999 with no output, peak amplitude 0.80") != NULL,
              "diagnostic totals reset once per second and retain the interval peak");
    }
    close(saved); fclose(capture);
    // Game threads may send independently; ThreadSanitizer also covers the logger's shared totals.
    pthread_t threads[4]; int started = 0;
    for (int i = 0; i < 4; i++) {
        int rc = pthread_create(&threads[started], NULL, vibration_sender, NULL);
        CHECK(!rc, "start concurrent vibration sender"); if (!rc) started++;
    }
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    unsetenv("DM_RUMBLE_LOG");
}

// Run the actual scripted publisher without waiting: the controller updates during its hold, after which the game reads twice (the first read consumes the press latch).
static void t_script_hold(void) {
    dm_pad_set(DM_BTN_B, 120, 0, 0, 0);
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_A | DM_BTN_B | DM_BTN_X), "script, controller and keyboard buttons are combined");
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_A | DM_BTN_B | DM_BTN_X), "a scripted hold survives controller updates after the press latch is consumed");
    CHECK(lx_of(0) == -0x7000, "the scripted stick wins over controller drift");
    CHECK(atomic_load(&g_shake_on), "controller updates do not stop a scripted shake");
}
static void t_script(void) {
    g_mode = 1; dm_pad_players(1);
    dm_pad_set(0, 0, 0, 0, 0); buttons_of(0, STYLE_FULLKEY);
    dm_input_set(DM_BTN_X, 0, 0, 0, 0);
    static key_step one[1]; g_steps = one; g_cap_steps = 0;
    g_nsteps = 1; g_steps[0] = (key_step){.btn = DM_BTN_A | DM_BTN_SHAKE, .lx = -0x7000, .hold = 0.15};
    script_hold = t_script_hold; keys_main(NULL); script_hold = NULL;
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_B | DM_BTN_X), "script release preserves the controller and keyboard buttons");
    CHECK(lx_of(0) == 120, "script release preserves the controller stick");
    CHECK(!atomic_load(&g_shake_on), "script release ends its shake hold");
    dm_input_set(0, 0, 0, 0, 0); dm_pad_set(0, 0, 0, 0, 0); buttons_of(0, STYLE_FULLKEY);
    g_steps[0] = (key_step){.btn = DM_BTN_Y}; keys_main(NULL);
    CHECK(buttons_of(0, STYLE_FULLKEY) == DM_BTN_Y, "a short scripted press is latched until the game reads");
    CHECK(buttons_of(0, STYLE_FULLKEY) == 0, "the short scripted press is read once");
    g_mode = 0; dm_pad_players(3);
    script_set(DM_BTN_A, 100, -200, 300, -400);
    CHECK(buttons_of(1, STYLE_FULLKEY) == 0, "player 2 does not receive or consume scripted presses");
    NpadState s; fill(&s, NPAD_HANDHELD, STYLE_HANDHELD);
    CHECK(s.buttons == DM_BTN_A && s.lx == 100 && s.ly == -200 && s.rx == 300 && s.ry == -400, "scripted buttons and all four axes reach player 1 in handheld mode");
    script_set(0, 0, 0, 0, 0); buttons_of(NPAD_HANDHELD, STYLE_HANDHELD);
    g_mode = 1; dm_pad_players(1);
    g_nsteps = 0; g_steps = NULL;
}

// DM_REC (GitHub issue #313): player 1's physical input written as DM_KEYS steps, read back by the DM_KEYS parser and played on the scripted path at the game's reads (keys_sync);
// the game's view of every frame (buttons with their press latch, the shake, sticks, the touchscreen) is the same in the recording and the replay, to the recorder's stick and touch
// steps, also for input that arrived after a frame's read
typedef struct { uint64_t btn; int32_t q[4]; int touch; int32_t tx, ty; } frame_view;
enum { REC_FRAMES = 64, REC_END = 62 };
static frame_view rec_live[REC_FRAMES], rec_replay[REC_FRAMES];
static frame_view game_reads(void) {   // what the game reads in this frame (and consumes: the press latches, a new touch)
    NpadState s; fill(&s, 0, STYLE_FULLKEY);
    frame_view v = {(s.buttons & 0xffff) | ((atomic_load(&g_btn) | atomic_load(&g_sc_btn) | atomic_load(&g_pad_btn[0])) & DM_BTN_SHAKE)};
    int32_t a[4] = {s.lx, s.ly, s.rx, s.ry}; for (int i = 0; i < 4; i++) v.q[i] = (int32_t)lrint(a[i] * 100.0 / DM_STICK_MAX);
    if ((v.touch = touch_down || touch_new)) { v.tx = (int32_t)lrintf(touch_x * 1000); v.ty = (int32_t)lrintf(touch_y * 1000); }
    touch_new = 0;
    return v;
}
static void live_to(uint64_t f) { while (test_frame < f) { rec_live[test_frame] = game_reads(); test_frame++; } }   // the game reads, then the next frame is presented
static void replay_frame(void) { if (test_frame < REC_FRAMES) rec_replay[test_frame] = game_reads(); test_frame++; }   // keys_main waits: a frame goes by
static void replay_late(void) { replay_frame(); replay_frame(); }   // keys_main waits and wakes late: two frames go by
static void t_rec(void) {
    g_mode = 1; dm_pad_players(1);
    dm_input_set(0, 0, 0, 0, 0); dm_pad_set(0, 0, 0, 0, 0); script_set(0, 0, 0, 0, 0); game_reads();
    char path[512]; snprintf(path, sizeof path, "%s/t_hid_rec.keys", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    test_frame = 0;
    CHECK(rec_open(path), "DM_REC opens its file");
    live_to(10); dm_input_set(DM_BTN_A, 0, 0, 0, 0);                         // the keyboard's A
    live_to(12); dm_pad_set(0, 16384, -8192, 0, 0);                          // the controller's left stick half right, a quarter down
    live_to(13); dm_pad_set(0, 16400, -8200, 0, 0);                          // drift within a step: no new step
    live_to(15); dm_input_set(0, 0, 0, 0, 0);
    live_to(20); dm_pad_set(0, 0, 0, 0, 0);
    live_to(22); dm_input_set(DM_BTN_B, 0, 0, 0, 0); dm_input_set(0, 0, 0, 0, 0);   // B tapped between two reads: the latch hands it to frame 22's read, recorded there
                 dm_pad_set(0, 0, 0, DM_STICK_MAX, 0); dm_pad_set(0, 0, 0, 0, 0);    // a stick flicked between two reads: the game cannot see it, not recorded
    live_to(30); dm_touch_set(&(dm_touch){0, .25f, .75f}, 1);                 // the mouse on the touchscreen, dragged, lifted
    live_to(31); dm_touch_set(&(dm_touch){0, .3f, .75f}, 1);
    live_to(32); dm_touch_set(&(dm_touch){0, .302f, .75f}, 1);               // moved within a step
    live_to(33); dm_touch_set(NULL, 0);
    live_to(40); dm_input_set(DM_BTN_ZR | DM_BTN_SHAKE, 0, 0, 0, 0);
    live_to(41); script_set(DM_BTN_X, 0, 0, 0, 0); dm_touch_script(&(dm_touch){0, .5f, .5f}, 1);   // scripted input (DM_KEYS, DM_CTL) is never recorded
                 game_reads(); script_set(0, 0, 0, 0, 0); dm_touch_script(NULL, 0);
    live_to(45); dm_input_set(0, 0, 0, 0, 0);
    live_to(50); rec_live[50] = game_reads(); dm_input_set(DM_BTN_X, 0, 0, 0, 0); test_frame = 51;   // X arrives after frame 50's read: the game sees it from frame 51, stamped there
    live_to(55); dm_input_set(0, 0, 0, 0, 0);
    live_to(60); dm_input_set(DM_BTN_Y, 0, 0, 0, 0);                         // still held at exit: ends at the exit's frame
    live_to(REC_END); dm_rec_finish();
    atomic_store(&g_rec_on, 0); atomic_store(&dm_touch_observer, NULL); fclose(g_rec_file); g_rec_file = NULL;
    dm_input_set(0, 0, 0, 0, 0); game_reads();

    char text[1024] = {0}; FILE *f = fopen(path, "r"); size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0; if (f) fclose(f); text[n] = 0; unlink(path);
    const char *want = "f10:A~f2,\nf12:A+L0.50/-0.25~f3,\nf15:L0.50/-0.25~f5,\nf22:B~f1,\nf30:T0.250x0.750~f1,\nf31:T0.300x0.750~f2,\nf40:ZR+Shake~f5,\nf51:X~f4,\nf60:Y~f2,\n";
    CHECK(!strcmp(text, want), "DM_REC writes the expected DM_KEYS steps");
    if (strcmp(text, want)) fprintf(stderr, "recorded:\n%s", text);

    keys_parse(text);
    CHECK(g_nsteps == 9, "the DM_KEYS parser reads every recorded step");
    if (g_nsteps == 9) {
        CHECK(g_steps[1].frame && g_steps[1].at == 12 && g_steps[1].fhold && g_steps[1].hold == 3 && g_steps[1].btn == DM_BTN_A && g_steps[1].lx == 16384 && g_steps[1].ly == -8192,
              "a recorded step reads back as its frame, frame hold, buttons and stick");
        CHECK(g_steps[4].tap && g_steps[4].tx == .25f && g_steps[4].ty == .75f && g_steps[6].btn == (DM_BTN_ZR | DM_BTN_SHAKE), "touches and the shake read back");
    }
    CHECK(keys_framed(), "a recording is all frame steps held in frames: it is played at the game's reads");
    touch_downs = 0; g_sync_next = g_sync_held = 0; atomic_store(&g_sync_on, 1);
    for (test_frame = 0; test_frame <= REC_END; test_frame++) rec_replay[test_frame] = game_reads();
    CHECK(touch_downs == 1, "a recorded drag replays as one finger that moves, not as taps");
    CHECK(rec_replay[REC_END - 1].btn == DM_BTN_Y && !rec_replay[REC_END].btn && !atomic_load(&g_sync_on), "the replay ends at the frame the recording ended");
    int bad = -1;
    for (int i = 0; i < REC_END && bad < 0; i++) {
        const frame_view *a = &rec_live[i], *b = &rec_replay[i]; int same = a->btn == b->btn && a->touch == b->touch;
        for (int k = 0; k < 4; k++) same &= abs(a->q[k] - b->q[k]) < REC_STICK_STEP;
        if (a->touch) same &= abs(a->tx - b->tx) < REC_TOUCH_STEP && abs(a->ty - b->ty) < REC_TOUCH_STEP;
        if (!same) bad = i;
    }
    CHECK(bad < 0, "the replay gives the game the recorded input in every frame");
    if (bad >= 0) fprintf(stderr, "frame %d: recorded %#llx L%d/%d touch %d, replayed %#llx L%d/%d touch %d\n", bad, (unsigned long long)rec_live[bad].btn, rec_live[bad].q[0], rec_live[bad].q[1],
                          rec_live[bad].touch, (unsigned long long)rec_replay[bad].btn, rec_replay[bad].q[0], rec_replay[bad].q[1], rec_replay[bad].touch);
    CHECK(rec_live[22].btn == DM_BTN_B && rec_replay[22].btn == DM_BTN_B && !rec_replay[23].btn, "a tap between two reads reaches the game once, live and replayed");
    CHECK(!rec_live[50].btn && !rec_replay[50].btn && rec_replay[51].btn == DM_BTN_X, "input that came after a frame's read reaches the replay's next frame, as it did live");
    CHECK(!atomic_load(&g_sc_btn) && !touch_down, "the replay releases everything at its end");
    free(g_steps); g_steps = NULL; g_nsteps = g_cap_steps = 0;
    // With the step limit raised for long recordings the steps are ordered by a merge sort: repeated groups still interleave, and steps at one time keep their listed order
    keys_parse("290:A*3/3,291:Left*3/3 291.5:B*3/3\n300:X,300:Y");
    static const uint64_t order[11] = {DM_BTN_A, DM_BTN_LEFT, DM_BTN_B, DM_BTN_A, DM_BTN_LEFT, DM_BTN_B, DM_BTN_A, DM_BTN_LEFT, DM_BTN_B, DM_BTN_X, DM_BTN_Y};
    int ordered = g_nsteps == 11; for (int i = 0; ordered && i < 11; i++) ordered = g_steps[i].btn == order[i];
    CHECK(ordered, "repeats interleave in time order and equal times keep their listed order; spaces and newlines separate steps");
    free(g_steps); g_steps = NULL; g_nsteps = g_cap_steps = 0;
    // A recording of the earlier format (stamped when input arrived: a press within one frame as ~f0) still plays at the reads: the press latch hands it to its frame's read
    keys_parse("f5:B~f0,\nf6:A~f1,\n");
    g_sync_next = g_sync_held = 0; atomic_store(&g_sync_on, 1); memset(rec_replay, 0, sizeof rec_replay);
    for (test_frame = 0; test_frame < 9; test_frame++) rec_replay[test_frame] = game_reads();
    CHECK(rec_replay[5].btn == DM_BTN_B && rec_replay[6].btn == DM_BTN_A && !rec_replay[4].btn && !rec_replay[7].btn, "a hold of 0 frames reaches its frame's read once");
    free(g_steps); g_steps = NULL; g_nsteps = g_cap_steps = 0;
    // A list with a step in seconds is played by keys_main. A replay that runs late: two frames go by each time keys_main waits, so the first tap is pressed a frame late and the second is already due when the first is let go. A gap
    // in the list is a release the game must read, so the second tap is a press of its own (it used to follow without a release, and the game saw one long press)
    keys_parse("f11:A~f2,\nf14:A~f2,\n");
    memset(rec_replay, 0, sizeof rec_replay); test_frame = 0; script_hold = replay_late; keys_main(NULL); script_hold = NULL; replay_frame();   // the game reads the last frame
    int presses = 0; for (int i = 1; i < REC_FRAMES; i++) presses += (rec_replay[i].btn & DM_BTN_A) && !(rec_replay[i - 1].btn & DM_BTN_A);
    CHECK(presses == 2, "a late replay keeps the release between two taps: the game reads two presses");
    free(g_steps); g_steps = NULL; g_nsteps = g_cap_steps = 0;
    // DM_KEYS=@<file>: a recording too long for the environment is read from its file as it is
    FILE *kf = fopen(path, "w"); CHECK(kf != NULL, "the steps file is written");
    if (kf) { for (int i = 0; i < 50000; i++) fprintf(kf, "f%d:A+L0.50/-0.25~f1,\n", 2 * i); fclose(kf); }
    char *kt = keys_file(path); CHECK(kt && strlen(kt) > 1000000, "keys_file reads a recording of more than 1 MiB whole");
    if (kt) keys_parse(kt); free(kt); unlink(path);
    CHECK(g_nsteps == 50000 && g_steps[49999].at == 99998, "all 50,000 steps of the file are parsed");
    free(g_steps); g_steps = NULL; g_nsteps = g_cap_steps = 0;
    CHECK(!keys_file(path), "a missing steps file reads as none");
}

// Live controller motion (dm_motion_set): player 1's six-axis sensor reports the controller's tilt and rotation instead of lying flat, and goes back to flat when cleared
static _Atomic int motion_ready, motion_go;
static void *motion_sender(void *arg) {
    int p = (int)(intptr_t)arg;
    atomic_fetch_add(&motion_ready, 1);
    while (!atomic_load(&motion_go)) sched_yield();
    for (int i = 0; i < 100; i++) {
        dm_motion_set(p, (float[]){0, 0, -1}, (float[]){0, 0, 1});
        dm_motion_set_hid(p, (float[]){0, 0, -1}, (float[]){0, 0, 1}, motion_now);
        if (!p) dm_motion_set_switch(0, (float[]){0, 0, -1}, (float[]){0, 0, 0});
        dm_motion_clear(p);
        dm_motion_clear_hid(p);
    }
    return NULL;
}
static void t_motion_threads(void) {
    pthread_t threads[DM_PLAYERS];
    for (int p = 0; p < DM_PLAYERS; p++) CHECK(!pthread_create(&threads[p], NULL, motion_sender, (void *)(intptr_t)p), "start concurrent first motion sample");
    while (atomic_load(&motion_ready) != DM_PLAYERS) sched_yield();
    atomic_store(&motion_go, 1);
    for (int p = 0; p < DM_PLAYERS; p++) pthread_join(threads[p], NULL);
    g_phone_motion.last_ns = 0;   // the following test starts with no live source
}
static void t_motion_live(void) {
    g_mode = 1; dm_pad_players(1);
    atomic_store(&g_shake_t0, 0); atomic_store(&g_shake_end, 0);   // no shake: the clocks here stand at 1 s, so the cycle t_script's shake finishes would never end

    uint32_t h; uint32_t id = 0;
    CHECK(h_hid_SixHandles(&h, 1, &id, STYLE_FULLKEY) == 1, "one six-axis handle for player 1");
    SixAxisState st[2];
    CHECK(h_hid_SixStates(st, 2, &h) == 2 && st[0].acc[2] == -1 && st[0].acc[0] == 0, "no live motion: flat");
    float a[3] = {0.5f, 0, -0.8660254f}, w[3] = {0, 0, 3.14159265f};
    dm_motion_set(0, a, w);
    CHECK(h_hid_SixStates(st, 2, &h) == 2, "live: every entry");
    CHECK(fabsf(st[0].acc[0] - 0.5f) < 1e-4f && fabsf(st[1].acc[0] - 0.5f) < 1e-4f, "live: the controller's acceleration");
    CHECK(fabsf(st[0].gyro[2] - 0.5f) < 1e-4f, "live: rad/s to revolutions/s");
    CHECK(fabsf(-st[0].dir[2] - 0.5f) < 0.01f, "live: primed tilted with the measured gravity");
    CHECK(st[0].sampling == st[1].sampling + 1, "live: numbered by time");
    CHECK(!h_hid_SixRest(&h), "live and turning: not at rest");
    dm_motion_clear(0);
    CHECK(h_hid_SixStates(st, 1, &h) == 1 && st[0].acc[0] == 0 && st[0].acc[2] == -1, "cleared: flat again");
    CHECK(h_hid_SixRest(&h), "cleared: at rest");

    // Auto uses fresh controller data ahead of the phone, including when the phone publishes last.
    dm_motion_set(0, a, w);
    dm_motion_set_switch(0, (float[]){0, -1, 0}, (float[]){0, 0, 0});
    h_hid_SixStates(st, 1, &h);
    CHECK(fabsf(st[0].acc[0] - 0.5f) < 1e-4f, "auto: phone samples do not replace a fresh controller");
    dm_motion_clear(0);
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[1] == -1 && fabsf(st[0].dir[5] - 1) < 1e-4f, "auto: disconnect falls back to the phone's own upright orientation");
    motion_now += MOTION_STALE_NS + 1;
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[2] == -1 && st[0].acc[1] == 0, "stale phone data falls back to flat");

    dm_motion_set_switch(0, (float[]){0, -1, 0}, (float[]){0, 0, 0});
    dm_motion_set(0, a, w);
    g_phone_mode = 1;
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[1] == -1, "forced phone: a controller publishing last cannot replace it");
    g_phone_mode = 0;
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[0] == a[0], "phone off: controller remains live");
    dm_motion_clear(0);
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[2] == -1 && st[0].acc[1] == 0, "phone off: a fresh phone cannot take over");

    // The same controller slot must drive buttons and motion when a game supports only one player.
    dm_pad_players(3); ids((uint32_t[]){0, NPAD_HANDHELD}, 2);
    dm_motion_set(1, a, w);
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[0] == a[0] && !h_hid_SixRest(&h), "one-player game: player 2's controller also drives player 1's motion");
    ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3);
    h_hid_SixStates(st, 1, &h);
    CHECK(st[0].acc[2] == -1 && h_hid_SixRest(&h), "co-op: player 2's motion stays on its own sensor");
    dm_motion_clear(1); dm_pad_players(1); g_phone_mode = -1;
}
static void t_motion_hid(void) {
    g_mode = 1; dm_pad_players(3); ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3); g_phone_mode = 0;
    uint32_t h = 0x20003; SixAxisState st;   // FullKey: SDK style 3, device 2.
    float flat[3] = {0, 0, -1}, tilted[3] = {.5f, 0, -.8660254f}, turn[3] = {0, 0, 2 * (float)M_PI};
    motion_now += 1000000000;
    dm_motion_clear(0);
    for (int i = 0; i < 3; i++) dm_motion_set_hid(0, flat, turn, motion_now - (2 - i) * 5000000);
    h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == 0 && fabsf(st.angle[2] - .01f) < 1e-6f, "HID: batch integrates two 5 ms intervals, independently of GC arrival time");
    dm_motion_set_hid(0, tilted, (float[]){0, 0, 0}, motion_now - 5000000);
    dm_motion_set_hid(0, tilted, turn, motion_now + 1);
    dm_motion_set_hid(0, tilted, turn, motion_now - MOTION_STALE_NS - 1);
    h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == 0 && st.gyro[2] > .99f && fabsf(st.angle[2] - .01f) < 1e-6f, "HID: reordered, future and stale samples cannot rewind or replace the orientation");
    motion_now += 1000000; dm_motion_set(0, tilted, turn);
    h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == .5f && st.angle[2] == 0, "HID: usable GC motion takes over with its independent orientation");
    dm_motion_clear(0); h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == 0 && fabsf(st.angle[2] - .01f) < 1e-6f, "HID: GC removal reveals raw source's independent orientation");
    dm_motion_clear_hid(0); h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == 0 && st.acc[2] == -1 && st.angle[2] == 0, "HID: removing both controller sources returns to flat");
    dm_motion_set_hid(0, flat, turn, motion_now); h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == 0 && st.angle[2] == 0, "HID: reconnect starts a new orientation");
    motion_now += MOTION_STALE_NS + 1; dm_motion_set(0, tilted, turn); h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == .5f, "HID: stale raw stream yields to live GC motion");
    dm_motion_set_hid(1, flat, turn, motion_now); h_hid_SixStates(&st, 1, &h);
    CHECK(st.acc[0] == .5f, "HID: another player's raw data stays on its own sensor");
    dm_motion_set_hid(0, flat, turn, motion_now); dm_motion_set_switch(0, (float[]){0, -1, 0}, (float[]){0, 0, 0}); g_phone_mode = 1;
    h_hid_SixStates(&st, 1, &h); CHECK(st.acc[1] == -1, "HID: forced phone selection still wins");
    dm_motion_clear_hid(0); dm_motion_clear_hid(1); dm_motion_clear(0); g_phone_motion.last_ns = 0;
    dm_pad_players(1); g_phone_mode = -1;
}

static void t_explicit(void) {
    g_mode = 0; ids((uint32_t[]){0, 1, 2, NPAD_HANDHELD}, 4);
    h_hid_SetStyles(0x1f);
    dm_pad_explicit(1);
    dm_pad_style(0, DM_STYLE_LEFT, 0); dm_pad_style(1, DM_STYLE_RIGHT, 0); dm_pad_style(2, DM_STYLE_PAIR, 0);
    dm_pad_players(7);
    CHECK(dm_player_npad(0) == 0 && !style_of(NPAD_HANDHELD), "explicit external controller uses No1 even in handheld mode");
    CHECK(style_of(0) == STYLE_JOYLEFT && style_of(1) == STYLE_JOYRIGHT && style_of(2) == STYLE_JOYDUAL, "three players report their independently selected styles");
    dm_input_set(0, 0, 0, 0, 0);
    dm_pad_set_player(0, DM_BTN_A | DM_BTN_L | DM_BTN_R, 10000, -12000, 20000, 23000);
    dm_pad_set_player(1, DM_BTN_B | DM_BTN_L | DM_BTN_R, -7000, 9000, 0, 0);
    NpadState left, right, wrong;
    fill(&left, 0, STYLE_JOYLEFT); fill(&right, 1, STYLE_JOYRIGHT); fill(&wrong, 0, STYLE_FULLKEY);
    CHECK(left.buttons == (DM_BTN_RIGHT | DM_BTN_LEFT_SL | DM_BTN_LEFT_SR), "single left Joy-Con exposes direction buttons and both left SL/SR bits");
    CHECK(right.buttons == (DM_BTN_B | DM_BTN_RIGHT_SL | DM_BTN_RIGHT_SR), "single right Joy-Con exposes face buttons and both right SL/SR bits");
    CHECK(left.lx == 10000 && !left.rx && right.rx == -7000 && !right.lx, "the primary stick drives only the selected Joy-Con side");
    // Game changes the hold type between reads while both pads stay held.
    dm_pad_style(0, DM_STYLE_LEFT, 1); dm_pad_style(1, DM_STYLE_RIGHT, 1);
    h_hid_HoldType(JOY_HOLD_HORIZONTAL);
    fill(&left, 0, STYLE_JOYLEFT); fill(&right, 1, STYLE_JOYRIGHT);
    CHECK(h_hid_GetHold() == JOY_HOLD_HORIZONTAL, "the game still reads back its horizontal hold type");
    CHECK(left.lx == -12000 && left.ly == -10000 && left.buttons == (DM_BTN_DOWN | DM_BTN_LEFT_SL | DM_BTN_LEFT_SR),
          "horizontal virtual left compensates with a clockwise turn");
    CHECK(right.rx == -9000 && right.ry == -7000 && right.buttons == (DM_BTN_A | DM_BTN_RIGHT_SL | DM_BTN_RIGHT_SR),
          "horizontal virtual right compensates with a counterclockwise turn");
    dm_pad_style(1, DM_STYLE_RIGHT, 0);   // physical Joy-Con keeps the previous layout
    fill(&right, 1, STYLE_JOYRIGHT);
    CHECK(right.rx == -7000 && right.ry == 9000 && (right.buttons & DM_BTN_B), "physical Joy-Con bypasses hold compensation");
    h_hid_HoldType(0);
    fill(&left, 0, STYLE_JOYLEFT);
    CHECK(left.lx == 10000 && left.ly == -12000 && (left.buttons & DM_BTN_RIGHT), "returning to vertical restores the layout immediately");
    CHECK(left.attributes == (ATTR_CONNECTED | ATTR_LEFT_CONN) && right.attributes == (ATTR_CONNECTED | ATTR_RIGHT_CONN) && !wrong.attributes, "connection attributes and state reader match the chosen style");
    CHECK(h_hid_GetJoyAssign(&(uint32_t){0}) == 1 && h_hid_GetJoyAssign(&(uint32_t){2}) == 0, "single and paired Joy-Con assignments are reported consistently");
    uint32_t lh[2], rh[2], ph[2];
    CHECK(h_hid_SixHandles(lh, 2, &(uint32_t){0}, STYLE_JOYLEFT) == 1 && lh[0] == 6, "left sensor uses SDK style 6 and device 0");
    CHECK(h_hid_SixHandles(rh, 2, &(uint32_t){1}, STYLE_JOYRIGHT) == 1 && rh[0] == 0x10107, "right sensor uses SDK style 7 and device 1");
    CHECK(h_hid_SixHandles(ph, 2, &(uint32_t){2}, STYLE_JOYDUAL) == 2 && ph[0] == 0x205 && ph[1] == 0x10205, "paired Joy-Con has two sensor handles on its own player");
    dm_motion_clear(0); dm_motion_clear(1);
    dm_motion_set(0, (float[]){0, 0, -1}, (float[]){1, 0, 0});
    dm_motion_set(1, (float[]){0, -1, 0}, (float[]){0, 2, 0});
    SixAxisState ls, rs;
    CHECK(h_hid_SixStates(&ls, 1, lh) == 1 && h_hid_SixStates(&rs, 1, rh) == 1 && ls.acc[2] == -1 && rs.acc[1] == -1, "each Joy-Con sensor reads only its assigned player's motion");
    CHECK(!six_connected(lh[0] | 1u << 16) && !six_connected(rh[0] & ~0x10000u), "handles for the other half are disconnected");
    dm_haptics_init(vibration_sink, 1); dm_haptics_route(0, 1); dm_haptics_route(1, 1);
    CHECK(h_hid_VibHandles(lh, 2, &(uint32_t){0}, STYLE_JOYLEFT) == 1 && lh[0] == 6, "left Joy-Con exposes only its left vibration channel");
    CHECK(h_hid_VibHandles(rh, 2, &(uint32_t){1}, STYLE_JOYRIGHT) == 1 && rh[0] == 0x10107, "right Joy-Con exposes only its right vibration channel");
    CHECK(vib_targets(lh[0]) == 1 && vib_targets(rh[0]) == 2, "vibration routes to the independently assigned players");
    h_hid_SetStyles(STYLE_JOYRIGHT);
    CHECK(!style_of(0) && style_of(1) == STYLE_JOYRIGHT, "a game cannot see a selected style it does not support");
    h_hid_SetStyles(0x1f);
    ids((uint32_t[]){0, NPAD_HANDHELD}, 2);
    CHECK(!merged_players(), "explicit player 2 does not silently feed player 1 in a one-player game");
    dm_pad_players(0);
    CHECK(!style_of(0) && !six_connected(6), "disconnecting explicit player 1 disconnects its style and sensor");
    dm_pad_explicit(0); for (int p = 0; p < DM_PLAYERS; p++) dm_pad_style(p, DM_STYLE_AUTO, 0);
    dm_pad_players(1); g_mode = 1; h_hid_SetStyles(0xffffffffu);
}

static void t_ios_touch_fallback(void) {
    g_mode = 0; h_hid_SetStyles(0x1f);
    ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3);
    dm_pad_players_ios(2, 0);
    CHECK(style_of(NPAD_HANDHELD) == STYLE_HANDHELD && style_of(1) == STYLE_FULLKEY,
          "automatic iOS assignments keep the legacy player 1 fallback");
    dm_pad_style(1, DM_STYLE_RIGHT, 1); dm_pad_players_ios(2, 2);
    CHECK(!style_of(NPAD_HANDHELD) && !style_of(0) && style_of(1) == STYLE_JOYRIGHT,
          "a manual assignment to player 2 does not add a hidden touch player");
    dm_pad_style(1, DM_STYLE_AUTO, 0); dm_pad_players_ios(0, 0);
    CHECK(style_of(NPAD_HANDHELD) == STYLE_HANDHELD, "with no assigned controller the touch pad returns as handheld");
    g_mode = 1;
    CHECK(style_of(0) == STYLE_FULLKEY, "docked touch fallback uses the Pro slot");
    h_hid_SetStyles(STYLE_JOYRIGHT);
    CHECK(!style_of(0), "touch fallback is not counted for a Joy-Con-only game");

    ids((uint32_t[]){0}, 1); h_hid_SetStyles(0x1f);
    dm_pad_style(1, DM_STYLE_LEFT, 1); dm_pad_players_ios(7, 2);
    CHECK(merged_players() == 4, "only untouched automatic pads still merge into player 1");
    dm_pad_style(1, DM_STYLE_AUTO, 0); dm_pad_players(1); h_hid_SetStyles(0xffffffffu);
}

static void t_ios_automatic_compatibility(void) {
    const uint32_t masks[] = {0, 1, 2, 3, 0xff};
    const uint32_t supported_ids[] = {0, 1, 0x103, 0x3ff};
    const uint32_t supported_styles[] = {0, STYLE_FULLKEY, STYLE_HANDHELD, 0x1f};
    dm_pad_explicit(0);
    for (int p = 0; p < DM_PLAYERS; p++) dm_pad_style(p, DM_STYLE_AUTO, 0);
    for (g_mode = 0; g_mode <= 1; g_mode++) {
        for (unsigned i = 0; i < sizeof supported_ids / sizeof supported_ids[0]; i++) {
            atomic_store(&g_ids_supported, supported_ids[i]);
            for (unsigned s = 0; s < sizeof supported_styles / sizeof supported_styles[0]; s++) {
                h_hid_SetStyles(supported_styles[s]);
                for (unsigned m = 0; m < sizeof masks / sizeof masks[0]; m++) {
                    dm_pad_players_ios(masks[m], 0);
                    uint32_t legacy_players = masks[m] | 1u, unsupported = 0;
                    for (int p = 0; p < DM_PLAYERS; p++) {
                        // Original iOS always reported the default Player 1.
                        uint32_t id = p == 0 && !g_mode ? NPAD_HANDHELD : (uint32_t)p;
                        int legacy_reported = !p || ((legacy_players >> p & 1u) && (supported_ids[i] >> p & 1u));
                        uint32_t expected = legacy_reported ? (id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY) & supported_styles[s] : 0;
                        CHECK(style_of(id) == expected, "automatic iOS controller styles match the original HID behavior");
                        if (p && (legacy_players >> p & 1u) && !legacy_reported) unsupported |= 1u << p;
                    }
                    uint32_t expected_merge = !(supported_ids[i] & 0xfeu) ? unsupported : 0;
                    CHECK(merged_players() == expected_merge, "automatic iOS one-player merging matches the original HID behavior");
                }
            }
        }
    }
    g_mode = 1;
    atomic_store(&g_ids_supported, 0x3ff);
    h_hid_SetStyles(0xffffffffu);
    dm_pad_players(1);
    g_signals = 0;
    uint64_t generation = dm_pad_generation(0);
    dm_pad_reconfigure_begin();
    dm_pad_reconfigure_sources(1);
    dm_pad_players_ios(1, 0);
    dm_pad_reconfigure_end();
    CHECK(dm_pad_generation(0) == generation + 1, "automatic source changes still invalidate queued motion");
    CHECK(!g_signals, "an unchanged automatic controller layout emits no extra style event");
    static uint8_t second[0x40];
    h_hid_BindStyleEv(&(uint32_t){1}, second, 0);
    dm_pad_reconfigure_begin();
    dm_pad_reconfigure_sources(2);
    dm_pad_players_ios(3, 0);
    dm_pad_reconfigure_end();
    CHECK(g_signals == 1, "connecting automatic player 2 emits exactly the original connection event");
    dm_pad_players(1);
}

static int controller_hook_answer, controller_hook_calls, controller_hook_min, controller_hook_max;
static int controller_hook(int minimum, int maximum) {
    CHECK(applet_depth == 1, "native controller request stays within the applet focus lifetime");
    controller_hook_calls++; controller_hook_min = minimum; controller_hook_max = maximum;
    return controller_hook_answer;
}
static _Atomic int live_reader_started, live_reader_finished;
static NpadState live_observed;
static void *live_reader(void *unused) {
    (void)unused; atomic_store(&live_reader_started, 1);
    fill(&live_observed, 0, STYLE_JOYRIGHT);
    atomic_store(&live_reader_finished, 1); return NULL;
}
static void t_live_apply(void) {
    g_mode = 1; ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3); h_hid_SetStyles(0x1f);
    dm_pad_explicit(1); dm_pad_style(0, DM_STYLE_PRO, 0); dm_pad_style(1, DM_STYLE_PRO, 0); dm_pad_players(3);
    dm_input_set(0, 0, 0, 0, 0); script_set(0, 0, 0, 0, 0);
    dm_pad_set_player(0, DM_BTN_A, 111, 0, 0, 0); dm_pad_set_player(0, 0, 0, 0, 0, 0);
    uint64_t old = dm_pad_generation(0), unchanged = dm_pad_generation(1);
    dm_motion_set_generation(0, old, (float[]){0, 0, -1}, (float[]){1, 0, 0});
    dm_motion_set_generation(1, unchanged, (float[]){0, -1, 0}, (float[]){0, 2, 0});
    dm_haptics_init(vibration_sink, 1); dm_haptics_route(0, 1); dm_haptics_route(1, 1);
    h_hid_SendVib(&(uint32_t){3}, &(dm_vibration){.amplitude_low = .7f, .frequency_low = 160, .frequency_high = 320});
    h_hid_SendVib(&(uint32_t){0x103}, &(dm_vibration){.amplitude_low = .6f, .frequency_low = 160, .frequency_high = 320});
    dm_pad_reconfigure_begin(); dm_pad_reconfigure_end();
    CHECK(dm_pad_generation(0) == old && dm_haptics_actual(0, 0).amplitude_low == .7f, "validation without a reset leaves active state unchanged");
    dm_pad_reconfigure_begin(); dm_pad_reconfigure_reset(1);
    pthread_t reader; pthread_create(&reader, NULL, live_reader, NULL);
    while (!atomic_load(&live_reader_started)) sched_yield();
    usleep(10000);
    CHECK(!atomic_load(&live_reader_finished), "a guest state read waits until the complete live apply has been published");
    dm_pad_style(0, DM_STYLE_RIGHT, 0); dm_pad_set_player(0, DM_BTN_B, 1234, 0, 0, 0); dm_pad_players(3);
    dm_motion_set_generation(0, old, (float[]){1, 0, 0}, (float[]){9, 9, 9});
    motion_state ms;
    CHECK(!motion_live(0, &ms), "a queued gyro sample from the old slot owner is ignored");
    CHECK(dm_pad_generation(1) == unchanged && motion_live(1, &ms) && ms.acc[1] == -1, "unchanged players retain their motion generation and source");
    CHECK(dm_haptics_actual(0, 0).amplitude_low == 0 && dm_haptics_actual(1, 0).amplitude_low == .6f, "only reassigned players lose their old vibration values");
    dm_pad_reconfigure_end(); pthread_join(reader, NULL);
    CHECK(live_observed.attributes == (ATTR_CONNECTED | ATTR_RIGHT_CONN) && live_observed.buttons == DM_BTN_B && live_observed.rx == 1234, "a guest reader sees the complete new style and input without the old press latch");
    dm_motion_set_generation(0, dm_pad_generation(0), (float[]){0, 0, -1}, (float[]){0, 1, 0});
    CHECK(motion_live(0, &ms) && ms.acc[2] == -1, "the new controller generation supplies motion normally");
    dm_motion_suspend(1);
    CHECK(!motion_live(0, &ms) && !motion_live(1, &ms), "the controller sheet suppresses live motion for every player");
    dm_motion_suspend(0);
    CHECK(motion_live(1, &ms) && ms.acc[1] == -1, "closing the sheet restores motion on the same player");
    dm_pad_set_player(0, DM_BTN_A, 0, 0, 0, 0); dm_pad_set_player(0, 0, 0, 0, 0, 0);
    dm_input_set(DM_BTN_X, 0, 0, 0, 0); dm_input_quiesce();
    NpadState state; fill(&state, 0, STYLE_JOYRIGHT);
    CHECK(!state.buttons, "opening configuration discards physical held input and pending press edges");
    dm_controller_support_hook = controller_hook;
    int8_t arg[32] = {2, 4}; int32_t info[2];
    controller_hook_answer = 0;
    CHECK(h_hid_ShowCtrl(info, arg) == RES_CTRL_CANCELED, "native Cancel returns the controller applet's cancellation result");
    CHECK(controller_hook_min == 2 && controller_hook_max == 4 && !applet_depth, "the game's player limits reach the native request and applet focus is restored");
    controller_hook_answer = 1;
    CHECK(h_hid_ShowCtrl0(arg) == RES_OK && controller_hook_calls == 2, "the no-result applet overload also waits for native Apply");
    dm_controller_support_hook = NULL;
    dm_pad_explicit(0); dm_pad_style(0, DM_STYLE_AUTO, 0); dm_pad_style(1, DM_STYLE_AUTO, 0); g_mode = 0; dm_pad_players(1);
    static uint8_t handheld[0x40], external[0x40];
    h_hid_BindStyleEv(&(uint32_t){NPAD_HANDHELD}, handheld, 0); h_hid_BindStyleEv(&(uint32_t){0}, external, 0);
    g_signals = 0;
    dm_pad_reconfigure_begin(); dm_pad_reconfigure_reset(1); dm_pad_explicit(1); dm_pad_style(0, DM_STYLE_PRO, 0); dm_pad_players(1);
    CHECK(!g_signals, "style events are deferred while live Apply is incomplete");
    dm_pad_reconfigure_end();
    CHECK(g_signals == 2, "changing player 1 from handheld to external signals both npad style events once");
    dm_pad_explicit(0); dm_pad_style(0, DM_STYLE_AUTO, 0); dm_pad_players(1); g_mode = 1; h_hid_SetStyles(0xffffffffu);
}

int main(void) {
    CHECK(setenv("DM_RUMBLE_LOG", "1", 1) == 0, "enable rumble diagnostics");
    t_slots();
    t_npads();
    t_script();
    t_vibration();
    t_rumble_log();
    t_motion_threads();
    t_motion_live();
    t_motion_hid();
    t_rec();
    {   // Nmouse with no device (SDK 21's forms): the pointer range is kept, the merged handle 0 takes settings, the standard mouse and the handle queries report nothing
        int w = 0, h = 0; uint64_t std = 7; uint32_t npad = 9;
        h_hid_NmouseInitT(NULL, 0, 1920, 1080, 1); h_hid_NmouseGetRange(&w, &h);
        CHECK(w == 1920 && h == 1080, "InitializeNmouse with a pointer type keeps the range");
        h_hid_NmouseSetRange(1280, 720); h_hid_NmouseGetRange(&w, &h);
        CHECK(w == 1280 && h == 720, "SetNmousePointerRanges is what GetNmousePointerRanges reads");
        CHECK(!h_hid_NmouseStd(&std) && std == 7, "no standard mouse handle");
        uint64_t merged = 1; CHECK(h_hid_NmouseMerged(&merged) && merged == 0, "the merged handle is 0");
        CHECK(h_hid_NmouseEnable(merged, 0x101) && h_hid_NmousePos(merged, 10, 10), "the merged handle takes the pointer settings (a bool argument's high bits ignored)");
        CHECK(!h_hid_NmouseEnable(5, 1) && !h_hid_NmousePos(5, 0, 0), "an unknown handle does not");
        CHECK(!h_hid_NmouseState(NULL, merged) && !h_hid_NmouseNpad(&npad, merged) && npad == 9, "no state and no controller behind the merged handle");
    }
    t_explicit();
    t_live_apply();
    t_ios_touch_fallback();
    t_ios_automatic_compatibility();
    printf("hid players: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
