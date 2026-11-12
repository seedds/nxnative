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
// The controller applet's prompt (front.h): -1 is no window; a test sets the answer and the players that join while it is up
static int g_prompt_answer = -1, g_prompt_calls, g_prompt_min, g_prompt_max; static uint32_t g_prompt_join;
int front_ctrl_support(int min, int max) { g_prompt_calls++; g_prompt_min = min; g_prompt_max = max; if (g_prompt_join) dm_pad_players(g_prompt_join); return g_prompt_answer; }

static int checks, fails, g_mode = 1, g_signals;
static void *g_signalled;
static time_t rumble_second = 1;
static uint64_t motion_now = 1000000000ull;
static void (*script_hold)(void);
static int hid_clock_gettime(clockid_t clock, struct timespec *ts) { (void)clock; *ts = (struct timespec){.tv_sec = rumble_second}; return 0; }
static int hid_usleep(useconds_t usec) { (void)usec; if (script_hold) script_hold(); return 0; }
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
int dm_opmode(void) { return g_mode; }
static uint64_t guest_now = 1000000000ull;   // the guest clock (t_npad_imu_samples moves it)
uint64_t guest_ns(void) { return guest_now; }
// The answers' high-water marks (answer_tick) follow the real clock, which never goes back; the tests move the guest clock back and forth, so a test that sets it starts them again
static void hwm_clear(void) { memset((void *)&g_hwm_states, 0, sizeof g_hwm_states); memset((void *)&g_hwm_imu, 0, sizeof g_hwm_imu); }
uint64_t now_ns(void) { return motion_now; }
static _Atomic uint64_t g_test_lag;   // t_live_edges: the guest clock behind the host one (a freeze), as hle_os.c's g_glag
static int g_test_pinned;   // t_live_pinned: a frozen or DM_DETCLOCK guest clock, every host time mapped to the guest's now
uint64_t guest_ns_at_host_time(uint64_t host_ns) { return g_test_pinned ? guest_now : host_ns + guest_now - motion_now; }   // the two test clocks: host now is guest now
int guest_clock_pinned(void) { return g_test_pinned; }
static uint64_t test_frame;   // presented frames (t_rec moves it)
uint64_t dm_guest_frame(void) { return test_frame; }
// The touchscreen as the game would read it: the first finger, and whether one was put down since the last read (a touch's Start is reported once, hle_touch.c)
static int touch_down, touch_new, touch_downs; static float touch_x, touch_y;
static void touch_to(const dm_touch *t, int n) { if (n > 0) { touch_new |= !touch_down; touch_downs += !touch_down; touch_x = t[0].x; touch_y = t[0].y; } touch_down = n > 0; }
void (*_Atomic dm_touch_observer)(const dm_touch *t, int n);
void dm_touch_set(const dm_touch *t, int n) { touch_to(t, n); void (*o)(const dm_touch *, int) = atomic_load(&dm_touch_observer); if (o) o(t, n); }
void dm_touch_script(const dm_touch *t, int n) { touch_to(t, n); }
static int64_t touch_samp; static int touch_points = -1;   // hle_touch.c numbers every read from 1; the last read's points argument
int touch_fill_n(void *out, int max, uint64_t now, int points) { (void)max; (void)now; touch_points = points; memset(out, 0, 0x10); int64_t s = ++touch_samp; memcpy(out, &s, 8); return 0; }
int gesture_fill(void *out, int n, uint64_t now) { (void)out; (void)n; (void)now; return 0; }
void dm_gfx_shot_at(long frame, const char *path) { (void)frame; (void)path; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { g_signalled = g; g_signals++; }
int hle_signal_bound_system_event(void *g) { g_signalled = g; g_signals++; return 1; }
void dm_gfx_texlist(int target) { (void)target; }
// the guest address space (hle_svc.c): the unit test has none, so every stack is a host stack and the big-stack take-overs (the controller applet) run on it directly (hle_bigstack.c)
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
void dm_gfx_restop(void) {}
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_ctl_peek(uint64_t a, int n) { (void)a; (void)n; }
void crash_dump_now(const char *why) { (void)why; }
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) { (void)a; (void)n; (void)path; }
void dm_ctl_cvar_i32(uint64_t off, int32_t v) { (void)off; (void)v; }
void dm_ctl_cvar_f32(uint64_t off, float v) { (void)off; (void)v; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_addr_note(uint64_t a, char *note, size_t n) { (void)a; if (n) note[0] = 0; }   // DM_CTL "addr" (nvn_mtl.m)
void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path) { (void)key; (void)a; (void)n; (void)path; }   // DM_CTL "bufsnap" (nvn_mtl.m)
void probe_atrace(uint64_t lo, uint64_t n) { (void)lo; (void)n; }   // DM_CTL "atrace" (probe.c)
static int applet_depth, applet_opens, applet_closes_at_depth0;   // hle_oe.c: the applet stand-ins open and close their applet around the answer
void dm_applet_enter(const char *name) { (void)name; applet_depth++; applet_opens++; }
void dm_applet_leave(void) { applet_closes_at_depth0 += --applet_depth == 0; }

static uint64_t buttons_of(uint32_t id, uint32_t style) { NpadState s; fill(&s, id, style); return s.attributes ? s.buttons & 0xffff : 0; }
static int32_t lx_of(uint32_t id) { NpadState s; fill(&s, id, STYLE_FULLKEY); return s.lx; }
static void ids(const uint32_t *v, uint64_t n) { h_hid_SetIds(v, n); }
static NpadState npad_of(uint32_t id, uint32_t style) { NpadState s; fill(&s, id, style); return s; }   // all of it, the rail buttons (bits 24..27) included

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
    {   // Home button protection: what the game set, per npad, off by default (Nintendo Switch Sports Resort asks at boot)
        uint32_t no1 = 0, no8 = 7, other = 0x10, handheld = 0x20, bad = 9;
        CHECK(!h_hid_HomeProtectOn(&no1) && !h_hid_HomeProtectOn(&handheld), "Home protection starts off");
        h_hid_HomeProtect(&no1, true); h_hid_HomeProtect(&handheld, true);
        CHECK(h_hid_HomeProtectOn(&no1) && h_hid_HomeProtectOn(&handheld) && !h_hid_HomeProtectOn(&no8) && !h_hid_HomeProtectOn(&other), "on for the npads the game turned it on for");
        h_hid_HomeProtect(&no1, false);
        CHECK(!h_hid_HomeProtectOn(&no1) && h_hid_HomeProtectOn(&handheld), "off again for No1, Handheld kept");
        h_hid_HomeProtect(&bad, true); CHECK(!h_hid_HomeProtectOn(&bad) && !h_hid_HomeProtectOn(NULL), "an id that is no npad: off");
        h_hid_HomeProtect(&handheld, false);
    }
    int32_t info[2]; int8_t arg[8] = {1, 4};
    CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 2 && info[1] == 0, "the controller applet reports two players, player 1 selected");
    CHECK(applet_opens == 1 && applet_depth == 0 && applet_closes_at_depth0 == 1, "the controller applet opens and closes once around its answer (focus 2, then 1)");
    arg[1] = 1; h_hid_ShowCtrl(info, arg);
    CHECK(info[0] == 1, "the applet's count is capped at the game's maximum");
    arg[0] = 3; arg[1] = 4;
    CHECK(h_hid_ShowCtrl(info, arg) == RES_CTRL_CANCELED && info[0] == 2, "fewer players than the game's minimum: canceled (2202-3101), as the player closing the applet");
    CHECK(h_hid_ShowCtrl0(arg) == RES_CTRL_CANCELED, "the overload without a result: canceled too");
    arg[0] = 2; CHECK(h_hid_ShowCtrl0(arg) == RES_OK, "the overload without a result: OK when the minimum is met");
    CHECK(g_prompt_calls == 2 && g_prompt_min == 3 && g_prompt_max == 4, "below the minimum the front end's prompt is asked, with the game's minimum and maximum");
    {   // GitHub issue #1640: with a window the prompt waits for players to join
        int calls = g_prompt_calls; arg[0] = 3; arg[1] = 4;
        g_prompt_answer = 1; g_prompt_join = 0x7;
        CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 3, "a third player joins while the prompt is up: OK with three");
        g_prompt_join = 0x3; dm_pad_players(0x3);
        CHECK(h_hid_ShowCtrl(info, arg) == RES_CTRL_CANCELED && info[0] == 2, "the prompt done with fewer than the minimum: canceled all the same");
        g_prompt_answer = 0; g_prompt_join = 0x7;
        CHECK(h_hid_ShowCtrl(info, arg) == RES_CTRL_CANCELED && info[0] == 3, "the player closes the prompt (B): canceled, whoever joined");
        dm_pad_players(0x3); g_prompt_answer = 1; g_prompt_join = 0;
        setenv("DM_CTL", "/nonexistent", 1);
        CHECK(h_hid_ShowCtrl(info, arg) == RES_CTRL_CANCELED && g_prompt_calls == calls + 3, "scripted input (DM_CTL): no prompt, canceled at once");
        unsetenv("DM_CTL");
        arg[0] = 2; CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && g_prompt_calls == calls + 3, "the minimum met: no prompt");
        g_prompt_answer = -1;
    }
    CHECK(h_hid_Strap() == RES_OK && h_hid_StrapArg(arg) == RES_OK, "both strap guide overloads answer as the player closing the guide");
    CHECK(applet_opens == 12 && applet_depth == 0, "every answer opened and closed its applet");
    arg[0] = 1;
    CHECK(six_connected(0x0100 | 3) && !six_connected(0x0200 | 3), "player 2 has a six-axis sensor, No3 none");
    {   // GitHub issue #2237: more players than the game's maximum. The applet leaves the players past it out, so the game does not see them and open it again
        arg[0] = 1; arg[1] = 1; g_signals = 0;
        CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 1 && dm_players_reported() == 1, "two players, one wanted: the applet answers one and one is reported");
        CHECK(h_hid_GetStyle(&(uint32_t){1}) == 0 && buttons_of(1, STYLE_FULLKEY) == 0, "No2 reads as not connected");
        CHECK(g_signals == 1 && g_signalled == ev1, "No2's style event tells the game");
        CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_Y | DM_BTN_B) && lx_of(0) == 300, "a maximum of one: the controller left out feeds player 1");
        g_signals = 0;
        CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 1 && g_signals == 0, "asked again: the same answer, nothing changes");
        h_hid_ShowCtrl(info, NULL); CHECK(info[0] == 1, "the applet without arguments reports the players the game sees");
        dm_pad_players(0x1); dm_pad_players(0x3); dm_pad_set_player(1, DM_BTN_B, 300, 0, 0, 0);
        CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY, "a controller that went away and came back is a player again, until the applet says otherwise");
        CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && h_hid_GetStyle(&(uint32_t){1}) == 0, "and the applet leaves it out again");
        dm_pad_players(0x7); dm_pad_set_player(2, DM_BTN_X, 0, 0, 0, 0);
        CHECK(h_hid_GetStyle(&(uint32_t){2}) == STYLE_FULLKEY, "a third controller connects: a player of its own, on No3");
        arg[1] = 2;
        CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 2, "one to two players wanted, three connected: two");
        CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){2}) == 0, "the lowest players stay: player 2 is back, player 3 is left out");
        CHECK(buttons_of(0, STYLE_FULLKEY) == DM_BTN_Y && buttons_of(1, STYLE_FULLKEY) == DM_BTN_B, "a maximum above one: the controller left out feeds nobody");
        arg[1] = 4;
        CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 3 && buttons_of(2, STYLE_FULLKEY) == DM_BTN_X, "the applet opened for up to four: all three play");
        arg[1] = 1; h_hid_ShowCtrl(info, arg);
        CHECK(h_hid_GetStyle(&(uint32_t){1}) == 0 && h_hid_GetStyle(&(uint32_t){2}) == 0, "one wanted: players 2 and 3 left out");
        CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_Y | DM_BTN_B | DM_BTN_X), "and both feed player 1");
        ids((const uint32_t[]){0, 1, 2, NPAD_HANDHELD}, 4);
        CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){2}) == STYLE_FULLKEY, "the game sets its supported npads again: everyone counts again");
        ids((const uint32_t[]){0, 1, 2, 3, 4, 5, 6, 7, NPAD_HANDHELD}, 9);
        dm_pad_players(0x3); arg[1] = 4;
    }

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


// The console mode at run time (dm_set_opmode → dm_hid_opmode_changed): player 1 moves between No1 and Handheld and both npads' style events are signalled, Handheld's too
// (GitHub issue #2143); a pair of Joy-Cons split over two npads gives up the second one in handheld mode, and that npad's event is signalled as well
static void t_opmode(void) {
    static uint8_t e0[0x40], e1[0x40], eh[0x40];
    g_mode = 1; ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3); dm_pad_players(1); h_hid_SetStyles(0xffffffffu);
    g_signals = 0; h_hid_BindStyleEv(&(uint32_t){0}, e0, 0);
    CHECK(g_signals == 1 && g_signalled == e0, "binding No1 docked: signalled once for its controller");
    h_hid_BindStyleEv(&(uint32_t){NPAD_HANDHELD}, eh, 0); h_hid_BindStyleEv(&(uint32_t){1}, e1, 0);
    CHECK(g_signals == 1, "binding Handheld and No2 docked: no controller, no signal");
    dm_hid_opmode_changed(); CHECK(g_signals == 1, "no mode change, no style change: no signal");
    g_signals = 0; g_signalled = NULL; g_mode = 0; dm_hid_opmode_changed();
    CHECK(g_signals == 2 && g_signalled == eh, "to handheld: No1 and Handheld signalled");
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == 0 && h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == STYLE_HANDHELD, "player 1 is on Handheld");
    g_signals = 0; g_mode = 1; dm_hid_opmode_changed();
    CHECK(g_signals == 2 && h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == 0, "back to console: both signalled again, player 1 on No1");
    // A split pair: left on No1, right on No2 while docked; handheld takes both back
    dm_joy_attach(0, 3); h_hid_JoySingle(&(uint32_t){0});
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYLEFT && h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYRIGHT, "a split pair docked: JoyLeft on No1, JoyRight on No2");
    g_signals = 0; g_mode = 0; dm_hid_opmode_changed();
    CHECK(g_signals == 3 && h_hid_GetStyle(&(uint32_t){1}) == 0, "to handheld with a split pair: No1, No2 and Handheld signalled, No2 empty");
    g_signals = 0; g_mode = 1; dm_hid_opmode_changed();
    CHECK(g_signals == 3 && h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYRIGHT, "back to console: the three again, the right Joy-Con on No2");
    h_hid_JoyDual(&(uint32_t){0}); dm_joy_attach(0, 0);
    // An npad bound while its controller is there is told that style by the bind: the next report signals nothing for it
    static uint8_t e0b[0x40]; h_hid_BindStyleEv(&(uint32_t){0}, e0b, 0);
    g_signals = 0; dm_pad_players(1); dm_hid_opmode_changed(); CHECK(g_signals == 0, "nothing changed since the bind: no signal");
}

// Joy-Cons as devices of their own (dm_joy_attach / dm_joy_set): nothing changes until a front end reports a Joy-Con half; then the pair is a JoyDual npad, splits into JoyLeft and
// JoyRight when the game asks for single assignment (or the controller applet needs another player), keeps the rail buttons, and has vibration handles per Joy-Con. Handheld: unchanged.
static void t_joycons(void) {
    g_mode = 1; ids((uint32_t[]){0, 1, 2, 3}, 4); dm_pad_players(1);
    h_hid_SetStyles(STYLE_FULLKEY | STYLE_JOYDUAL | STYLE_JOYLEFT | STYLE_JOYRIGHT);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY, "no Joy-Con reported: player 1 is full-key, as before");
    { int m; CHECK(vib_targets(0x0003, &m) && m == 0, "and its full-key vibration handle reaches it"); }
    dm_haptics_route(0, 1);
    static uint8_t ev0[0x40]; h_hid_BindStyleEv(&(uint32_t){0}, ev0, 0);
    g_signals = 0; dm_joy_attach(0, 3);
    CHECK(g_signals == 1 && g_signalled == ev0 && h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYDUAL, "a left and a right Joy-Con on player 1: one JoyDual npad, No1's style event signalled and no other");
    g_signals = 0; dm_joy_attach(0, 3); CHECK(g_signals == 0, "the same Joy-Cons reported again: no signal");
    dm_joy_set(0, 0, DM_BTN_UP | DM_BTN_LSL, -20000, 0); dm_joy_set(0, 1, DM_BTN_A | DM_BTN_RSR, 0, 15000);
    NpadState st = npad_of(0, STYLE_JOYDUAL);
    CHECK((st.buttons & (DM_BTN_UP | DM_BTN_A | DM_BTN_LSL | DM_BTN_RSR)) == (DM_BTN_UP | DM_BTN_A | DM_BTN_LSL | DM_BTN_RSR), "JoyDual: both Joy-Cons' buttons, the rail buttons kept");
    CHECK(st.lx == -20000 && st.ry == 15000 && (st.attributes & (ATTR_LEFT_CONN | ATTR_RIGHT_CONN)) == (ATTR_LEFT_CONN | ATTR_RIGHT_CONN), "JoyDual: each Joy-Con's stick on its side, both halves connected");
    uint32_t h[2], id = 0;
    CHECK(h_hid_VibHandles(h, 2, &id, STYLE_JOYDUAL) == 2 && (h[0] & 0xff) == 5 && h[1] == (h[0] | 1u << 16), "JoyDual vibration handles: type 5, a motor per Joy-Con");
    { int m; CHECK(vib_targets(h[1], &m) == 1 && m == 1, "the right Joy-Con's JoyDual handle reaches motor 1"); }
    { int m; CHECK(vib_targets(0x10003, &m) == 1 && m == 1 && vib_targets(0x0003, &m) == 1 && m == 0, "the full-key handles the game got before the pair connected still reach both Joy-Cons"); }

    // The game supports no Joy-Con style: the pad's merged state as full-key, as before
    h_hid_SetStyles(STYLE_FULLKEY);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY, "no Joy-Con style supported: full-key");
    h_hid_SetStyles(STYLE_FULLKEY | STYLE_JOYDUAL | STYLE_JOYLEFT | STYLE_JOYRIGHT);

    // Single assignment: the left Joy-Con stays on No1, the right one takes No2 (no player of its own there)
    h_hid_JoySingle(&(uint32_t){0});
    CHECK(h_hid_GetJoyAssign(&(uint32_t){0}) == 1 && h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYLEFT && h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYRIGHT, "single: JoyLeft on No1, JoyRight on No2");
    st = npad_of(1, STYLE_JOYRIGHT);
    CHECK((st.buttons & (DM_BTN_A | DM_BTN_RSR)) == (DM_BTN_A | DM_BTN_RSR) && !(st.buttons & DM_BTN_UP) && st.ry == 15000 && st.attributes == (ATTR_CONNECTED | ATTR_RIGHT_CONN), "the right half on No2: its own buttons and stick only");
    id = 1; CHECK(h_hid_VibHandles(h, 2, &id, STYLE_JOYRIGHT) == 1 && (h[0] & 0xff) == 7, "JoyRight: one vibration handle, type 7");
    { int m; CHECK(vib_targets(h[0], &m) == 1 && m == 1, "it reaches player 1's right motor, on No2"); }
    uint32_t other = 9; CHECK(h_hid_JoySingleOut(&other, &(uint32_t){0}, 0) == 1 && other == 1, "single assignment with the other half: No2");
    h_hid_JoyDual(&(uint32_t){0});
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYDUAL && h_hid_GetStyle(&(uint32_t){1}) == 0, "dual again: one JoyDual npad, No2 empty");

    // The controller applet asks for two players with one pair connected: it splits the pair, as holding the Joy-Cons sideways does on a Switch
    int32_t info[2]; int8_t arg[8] = {2, 4};
    { int calls = g_prompt_calls;
    CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 2 && h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYRIGHT, "the applet splits the pair for a second player");
    CHECK(g_prompt_calls == calls && dm_players_reported() == 2, "the split pair is enough: no controller prompt, and the prompt's count (dm_players_reported) sees two players"); }
    h_hid_JoyDual(&(uint32_t){0});

    // Handheld mode: player 1's Joy-Cons count as attached to the console (the Handheld npad), and are never split
    g_mode = 0; ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3);
    CHECK(h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == STYLE_HANDHELD && h_hid_GetStyle(&(uint32_t){0}) == 0, "handheld: player 1's Joy-Cons are the Handheld npad");
    g_mode = 1; ids((uint32_t[]){0, 1, 2, 3}, 4);

    // One Joy-Con alone on player 2: JoyRight on No2; leaving: full-key again
    dm_pad_players(3); dm_joy_attach(1, 2);
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYRIGHT, "one right Joy-Con alone: JoyRight on its own npad");
    dm_joy_attach(1, 0); dm_joy_attach(0, 0);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY, "no Joy-Con reported any more: full-key controllers, as before");
    dm_haptics_route(0, 0); dm_pad_players(1); h_hid_SetStyles(0xffffffffu);
}

// A Joy-Con's mouse sensor (dm_joy_mouse): its JcmsState ring by handle (npad id, JcmsType 0 left / 1 right), newest first; none for a Joy-Con that never reported
static void t_jcms(void) {
    g_mode = 1; ids((uint32_t[]){0, 1}, 2); dm_pad_players(1); dm_joy_attach(0, 3);
    JcmsState st[4]; uint32_t hl = h_hid_JcmsHandle(&(uint32_t){0}, &(uint32_t){0}), hr = h_hid_JcmsHandle(&(uint32_t){0}, &(uint32_t){1});
    CHECK(hl == 0x000 && hr == 0x100 && h_hid_JcmsStates(st, 4, &hr) == 0, "no mouse samples before a front end reports them");
    CHECK(!dm_joy_mouse_wanted(), "the sensors stay off until the game sets them up");
    h_hid_JcmsInit(NULL, 0); CHECK(dm_joy_mouse_wanted(), "InitializeJcms asks for the sensors");
    h_hid_NmouseInit(NULL, 0, 1920, 1080); h_hid_JcmsFini(); CHECK(dm_joy_mouse_wanted(), "the pointer still wants them after FinalizeJcms");
    h_hid_NmouseFini(); CHECK(!dm_joy_mouse_wanted(), "off again once both are finalized");
    dm_joy_mouse(0, 1, 5, -3, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE); dm_joy_mouse(0, 1, 7, 2, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE);
    CHECK(h_hid_JcmsStates(st, 4, &hr) == 2 && st[0].sampling == 2 && st[0].dx == 7 && st[1].dy == -3 && st[0].attributes == 3, "the right Joy-Con's samples, newest first");
    CHECK(h_hid_JcmsStates(st, 4, &hl) == 0, "the left Joy-Con has reported none");
    memset(st, 0xff, sizeof st); h_hid_JcmsState(st, &hr);
    CHECK(st[0].sampling == 2 && st[0].dx == 7 && st[0].unk0 == 0, "GetJcmsState: the newest");
    for (int i = 0; i < 40; i++) dm_joy_mouse(0, 1, i, 0, 1);
    CHECK(h_hid_JcmsStates(st, 4, &hr) == 4 && st[0].dx == 39 && st[3].dx == 36, "a full ring keeps the newest");
    uint32_t bad = 0x200; CHECK(h_hid_JcmsStates(st, 4, &bad) == 0, "a JcmsType past 1 has no states");
    // The pointer: a handle per Joy-Con whose sensor reports, the one last used on a surface first, kept first while lifted
    uint64_t nh[4]; CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && (nh[0] & 0xffff) == 0x0001, "one pointer handle: the right Joy-Con, the only one that reported");
    dm_joy_mouse(0, 0, 1, 1, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 2 && (nh[0] & 1) == 0 && (nh[1] & 1) == 1, "the left Joy-Con, last on a surface, comes first");
    dm_joy_mouse(0, 0, 0, 0, DM_JOYMOUSE_CONNECTED);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 2 && (nh[0] & 1) == 0, "it stays first while lifted");
    NmouseState ns; CHECK(h_hid_NmouseState(&ns, nh[0]) && (ns.attributes & 2) && !(ns.attributes & 1) && !h_hid_NmouseState(&ns, 0), "its state: connected, off the surface; the merged handle has none");
    // The Joy-Con 2 styles: a Joy-Con 2 pair shows JoyDual's Joy-Con 2 form to a game that supports only that
    h_hid_SetStyles(STYLE_JOYDUAL2); dm_joy_attach(0, 3 | DM_JOY_2);
    CHECK((h_hid_GetStyle(&(uint32_t){0}) & (STYLE_JOYDUAL | STYLE_JOYDUAL2)) == (STYLE_JOYDUAL | STYLE_JOYDUAL2), "a Joy-Con 2 pair: JoyDual with its Joy-Con 2 bit");
    dm_joy_attach(0, 3); CHECK(!(h_hid_GetStyle(&(uint32_t){0}) & STYLE_JOYDUAL2), "a plain pair: no Joy-Con 2 bit");
    h_hid_SetStyles(0xffffffffu); dm_joy_attach(0, 0);
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
    CHECK(h_hid_VibMountedOunce(&h[0]) && h_hid_VibMountedOunce(&h[1]) && !h_hid_VibMountedOunce(NULL), "the Switch 2's form answers the same");
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
    { int m; CHECK(!vib_targets(3, &m), "old docked handle is not valid in handheld mode"); }
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
    uint32_t h = 3; SixAxisState st;   // full key on No1: style index 3, as the SDK numbers six-axis and vibration handles
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
// Touch screen reads across docked <-> handheld: every read goes to hle_touch.c, which numbers the states of both modes from one count (t_touch checks the numbers), with
// touch points asked for only in handheld mode. Numbered apart (docked from the npad counter, 65536; handheld from 1), a game started docked dropped every touch after
// leaving the dock: Unity's touch reader (Aokana main+0xd2bde0) takes a state only if its sampling number is above the last one it took, empty states included.
static void t_touch_sampling(void) {
    uint8_t s[0x10 + 0x28 * 16]; int64_t prev = 0, v;
    const int modes[] = {1, 1, 0, 0, 1, 0};
    for (int i = 0; i < 6; i++) {
        g_mode = modes[i]; memset(s, 0, sizeof s);
        if (i & 1) CHECK(h_hid_TouchN_16(s, 1) == 1, "touch: GetTouchScreenStates gives one state");
        else h_hid_Touch1_16(s);
        memcpy(&v, s, 8);
        CHECK(touch_points == !modes[i], "touch: touch points are asked for in handheld mode only");
        CHECK(v > prev, i == 2 ? "touch: handheld after docked continues the sampling numbers" : i == 4 ? "touch: docked after handheld continues the sampling numbers"
                                : i == 5 ? "touch: handheld again continues the sampling numbers" : "touch: sampling numbers increase");
        prev = v;
    }
    g_mode = 1;
}

// Unintended Home button input protection: one flag per npad, on until the game turns it off (Nintendo Switch Sports Resort asks, then turns it off for each npad)

// Npad IMU (Switch 2): the host serves the layer the SDK's sensor fusion reads from. Enabled with the game's buffer, disabled again; no samples yet (the buffer is never written)
static void t_npad_imu(void) {
    uint8_t buf[64], samples[2][40]; memset(buf, 0xCD, sizeof buf); memset(samples, 0xAB, sizeof samples);
    CHECK(!h_hid_ImuIs(), "npad imu: off at first");
    CHECK(h_hid_ImuEnable(buf, sizeof buf) == 0 && h_hid_ImuIs(), "npad imu: enabled with the game's buffer");
    CHECK(h_hid_ImuEnable(buf, sizeof buf) != 0 && h_hid_ImuIs(), "npad imu: enabling twice fails, as the SDK's does, and it stays on");
    CHECK(h_hid_ImuSamples(samples, 2, 0x20) == 0 && samples[0][0] == 0xAB && buf[0] == 0xCD, "npad imu: a handle with no sensor (style bit 32) has no samples, nothing written");
    CHECK(h_hid_ImuDisable() == 0 && !h_hid_ImuIs() && h_hid_ImuSamples(samples, 2, 0) == 0, "npad imu: disabled, still no samples");
    CHECK(h_hid_ImuEnable(NULL, 0) == 0 && h_hid_ImuIs() && h_hid_ImuDisable() == 0 && !h_hid_ImuIs(), "npad imu: the state is its own, not the buffer pointer");
}

// Npad IMU samples (Switch 2): stateless, as the console's ring in shared memory. A call returns the newest min(n, 16) 5 ms ticks on the guest clock, oldest first, numbered by absolute
// tick + 1; the SDK's fusion skips numbers it has taken (sdk+0x6d7198), so a repeated tick is harmless and every reader gets every tick. Units are the fusion's: m/s^2 and rad/s, with the
// six-axis state's signs: the SDK negates the acceleration (sdk+0x6d7334) and takes the angular velocity as it is
typedef struct { int64_t sampling, delta_ns; float acc[3], gyro[3]; } TestImuSample;   // acceleration at +0x10, angular velocity at +0x1c (sdk+0x6d7320)
_Static_assert(sizeof(TestImuSample) == 0x28, "NpadImuSample");
static int imu_numbered(const TestImuSample *s, int k, int64_t first) {   // k samples numbered first, first + 1, ... each 5 ms apart
    for (int i = 0; i < k; i++) if (s[i].sampling != first + i || s[i].delta_ns != 5000000) return 0;
    return 1;
}
static uint32_t imu_handle; static _Atomic int imu_reader_ok;
static void *imu_reader(void *arg) {   // reads the newest 16 at a fixed guest time: ticks 585..600, numbers 586..601
    (void)arg; TestImuSample s[16]; int ok = 1;
    for (int r = 0; r < 50; r++) ok &= h_hid_ImuSamples(s, 16, imu_handle) == 16 && imu_numbered(s, 16, 586);
    if (ok) atomic_fetch_add(&imu_reader_ok, 1);
    return NULL;
}
static void t_npad_imu_samples(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0;
    setenv("DM_IMU_NOISE", "0", 1); unsetenv("DM_IMU_ACC_SIGN"); unsetenv("DM_IMU_GYRO_SIGN"); imu_env_read();   // exact values and the host's own signs here; t_imu_noise checks the noise
    atomic_store(&g_shake_t0, 0); atomic_store(&g_shake_end, 0);
    uint32_t h = 0x20003;   // the SDK's own handle (utility::GetSixAxisSensorHandle, sdk+0x6d8490): full key (style index 3) on No1, the whole controller (device 2)
    imu_handle = h;
    TestImuSample s[40], t[40]; memset(s, 0xAB, sizeof s);
    hwm_clear(), guest_now = 2000000000ull;   // tick 400
    CHECK(h_hid_ImuSamples(s, 16, h) == 0 && s[0].sampling == (int64_t)0xABABABABABABABABull, "imu: off, no samples and nothing written");
    uint8_t ring[64]; CHECK(h_hid_ImuEnable(ring, sizeof ring) == 0, "imu: on");
    CHECK(h_hid_ImuSamples(s, 1, h) == 1 && imu_numbered(s, 1, 401), "imu: numbered by absolute tick + 1");
    CHECK(s[0].acc[0] == 0 && s[0].acc[1] == 0 && fabsf(s[0].acc[2] + 9.80665f) < 1e-5f, "imu: at rest (0, 0, -1) G reads (0, 0, -9.80665) m/s^2");
    {   // the layout the fusion reads: acceleration at +0x10, angular velocity at +0x1c
        float raw[10]; memcpy(raw, &s[0], sizeof raw);
        CHECK(sizeof(TestImuSample) == 0x28 && fabsf(raw[(0x10 + 8) / 4] + 9.80665f) < 1e-5f && raw[(0x1c + 8) / 4] == 0, "imu: acceleration at +0x10 (z at +0x18), angular velocity at +0x1c");
    }
    CHECK(s[0].gyro[0] == 0 && s[0].gyro[1] == 0 && s[0].gyro[2] == 0, "imu: at rest no rotation");
    CHECK(h_hid_ImuSamples(s, 16, h) == 16 && imu_numbered(s, 16, 386), "imu: the newest 16 ending at the current tick, oldest first");
    CHECK(h_hid_ImuSamples(t, 16, h) == 16 && !memcmp(s, t, 16 * sizeof *s), "imu: the same tick again gives the same numbers (the fusion skips them)");   // NOLINT(bugprone-suspicious-memory-comparison): the same tick read again is the same bytes, padding included (both buffers zeroed by the host)
    CHECK(h_hid_ImuSamples(s, 4, h) == 4 && imu_numbered(s, 4, 398), "imu: at most n: the newest 4");
    CHECK(h_hid_ImuSamples(s, 40, h) == 16 && imu_numbered(s, 16, 386), "imu: never more than the 16 the SDK's ring holds");
    guest_now += 4000000; CHECK(h_hid_ImuSamples(s, 1, h) == 1 && imu_numbered(s, 1, 401), "imu: less than a tick later: the same newest tick");
    guest_now += 1000000 + 2 * 5000000; CHECK(h_hid_ImuSamples(s, 16, h) == 16 && imu_numbered(s, 16, 389), "imu: three ticks later the window moves by three");
    CHECK(h_hid_ImuSamples(s, 0, h) == 0 && h_hid_ImuSamples(NULL, 4, h) == 0, "imu: no room: none");
    hwm_clear(), guest_now = 2000000; CHECK(h_hid_ImuSamples(s, 16, h) == 1 && imu_numbered(s, 1, 1), "imu: at the clock's start, the first tick only, number 1");
    hwm_clear(), guest_now = 2015000000ull;

    // A live controller: its acceleration and rotation, converted. 0.5 rev/s about z is pi rad/s; G x 9.80665 m/s^2 (the fusion negates it into a specific force)
    float a[3] = {0.5f, 0, -0.8660254f}, w[3] = {0, 0, 3.14159265f};
    dm_motion_set(0, a, w);
    CHECK(h_hid_ImuSamples(s, 2, h) == 2 && imu_numbered(s, 2, 403), "imu: live motion keeps the numbering");
    CHECK(fabsf(s[1].acc[0] - 0.5f * 9.80665f) < 1e-4f && fabsf(s[1].acc[2] + 0.8660254f * 9.80665f) < 1e-4f, "imu: live acceleration, G x 9.80665");
    CHECK(fabsf(s[1].gyro[2] - 3.14159265f) < 1e-4f && s[1].gyro[0] == 0, "imu: live rotation, rev/s x 2 pi");
    // DM_IMU_ACC_SIGN / DM_IMU_GYRO_SIGN = -1 negate either
    setenv("DM_IMU_ACC_SIGN", "-1", 1); setenv("DM_IMU_GYRO_SIGN", "-1", 1); imu_env_read();
    CHECK(h_hid_ImuSamples(s, 1, h) == 1 && fabsf(s[0].acc[0] + 0.5f * 9.80665f) < 1e-4f && fabsf(s[0].gyro[2] + 3.14159265f) < 1e-4f, "imu: both signs flipped by the switches");
    setenv("DM_IMU_GYRO_SIGN", "1", 1); imu_env_read();
    CHECK(h_hid_ImuSamples(s, 1, h) == 1 && s[0].acc[0] < 0 && s[0].gyro[2] > 0, "imu: each sign on its own");
    setenv("DM_IMU_GYRO_SIGN", "-1", 1); imu_env_read();   // the rest of the tests: negated (main)
    dm_motion_clear(0);

    // Another device reads the same clock; a handle whose controller is not connected has no samples
    uint32_t right = 0x10003;   // another device index of the same controller
    CHECK(h_hid_ImuSamples(s, 2, right) == 2 && imu_numbered(s, 2, 403), "imu: another device index, the same absolute numbers");
    uint32_t no4 = 0x20003 | 3u << 8; memset(s, 0xAB, sizeof s);
    CHECK(h_hid_ImuSamples(s, 16, no4) == 0 && s[0].delta_ns == (int64_t)0xABABABABABABABABull, "imu: an npad with no controller: no samples, nothing written");

    // Two readers each get the same newest ticks, also at once from two threads
    hwm_clear(), guest_now = 3000000000ull;   // tick 600
    CHECK(h_hid_ImuSamples(s, 16, h) == 16 && h_hid_ImuSamples(t, 16, h) == 16 && !memcmp(s, t, 16 * sizeof *s) && imu_numbered(s, 16, 586), "imu: two readers, the same ticks");   // NOLINT(bugprone-suspicious-memory-comparison): two reads of one tick are the same bytes
    atomic_store(&imu_reader_ok, 0);
    pthread_t th[2]; for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, imu_reader, NULL);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    CHECK(atomic_load(&imu_reader_ok) == 2, "imu: concurrent readers each get every tick");

    // Off: none; on again, numbering goes on from absolute time
    CHECK(h_hid_ImuDisable() == 0 && h_hid_ImuSamples(s, 16, h) == 0, "imu: disabled, no samples");
    guest_now += 30 * 5000000ull; h_hid_ImuEnable(ring, sizeof ring);
    CHECK(h_hid_ImuSamples(s, 16, h) == 16 && imu_numbered(s, 16, 616), "imu: on again, numbered by absolute time");
    h_hid_ImuDisable(); hwm_clear(), guest_now = 1000000000ull; dm_pad_players(1); g_phone_mode = -1;
}

// The IMU handles the SDK makes itself (GetNpadImuHandles → utility::GetSixAxisSensorHandle, sdk+0x6d8490, not taken over): NpadStyleIndex in byte 0, device in byte 2. Nintendo
// Switch Sports Resort supports Handheld | JoyDual (0x6), so the keyboard's player 1 is a Joy-Con pair on No1 and its fusion reads 0x5 and 0x10005: those must have samples
static void t_imu_sdk_handles(void) {
    g_mode = 1; ids((uint32_t[]){0, 1, 2, 3}, 4); dm_pad_players(1); g_phone_mode = 0;
    setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    // The handle numbering (six_style_bit): style index 3..7 → style bits 0..4; device 2 only for full key and handheld (SDK 22's single sensor)
    CHECK(six_style_bit(0x20003) == 0 && six_style_bit(0x20004) == 1 && six_style_bit(0x10005) == 2 && six_style_bit(0x6) == 3 && six_style_bit(0x10007) == 4, "handles: style index to style bit");
    CHECK(six_style_bit(0x20005) == 2 && six_style_bit(0x20006) == 3 && six_style_bit(0x20007) == 4, "handles: device 2 is taken for every style (SDKs differ in the device they give)");
    CHECK(six_style_bit(0x0) < 0 && six_style_bit(0x2) < 0 && six_style_bit(0x8) < 0 && six_style_bit(0x30003) < 0, "handles: no style index the host knows, or device 3: none");
    // The host's own handles (GetSixAxisSensorHandles) are in the same numbering, so either kind reaches the IMU
    uint32_t h[2], id = 0;
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_FULLKEY) == 1 && h[0] == 0x3, "host handles: full key on No1 is style index 3");
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_JOYDUAL) == 2 && h[0] == 0x5 && h[1] == 0x10005, "host handles: a pair is 0x5 and 0x10005, as the SDK's");
    CHECK(h_hid_SixHandles(h, 2, &(uint32_t){0x20}, STYLE_HANDHELD) == 2 && h[0] == 0x2004 && h[1] == 0x12004, "host handles: handheld on 0x20, two sensors");
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_JOYLEFT) == 1 && h[0] == 0x6 && h_hid_SixHandles(h, 2, &(uint32_t){1}, STYLE_JOYRIGHT) == 1 && h[0] == 0x10107, "host handles: left 0x6, right device 1");
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_JOYDUAL2) == 2 && h[0] == 0x5 && h_hid_SixHandles(h, 2, &id, 1u << 9) == 0, "host handles: a Joy-Con 2 pair as a pair; a style without a sensor: none");
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    TestImuSample s[4];
    CHECK(h_hid_SixHandles(h, 1, &id, STYLE_FULLKEY) == 1 && h_hid_ImuSamples(s, 4, h[0]) == 4 && h_hid_ImuSamples(s, 4, 0x20003) == 4, "imu: the host's full-key handle and the SDK's (device 2) both have samples");
    h_hid_SetStyles(STYLE_HANDHELD | STYLE_JOYDUAL);   // the game's lobby (main+0xa9e940): the keyboard is shown as a pair
    CHECK(h_hid_ImuSamples(s, 4, 0x5) == 4 && h_hid_ImuSamples(s, 4, 0x10005) == 4, "imu: the pair's two SDK handles have samples");
    CHECK(h_hid_ImuSamples(s, 4, 0x20003) == 0, "imu: no full-key controller then: none");
    {   // an SDK-built pair handle with device 2 reads as the pair's device 0 (six_source takes the side from bit 0), through the six-axis states as through the IMU
        SixAxisState st6[2];
        CHECK(h_hid_ImuSamples(s, 4, 0x20005) == 4 && six_states(st6, 2, 0x20005) == 2 && st6[0].acc[2] == -1, "six-axis: a pair's device-2 handle has states and samples");
        uint32_t two = 0x20005; CHECK(h_hid_SixStates(st6, 1, &two) == 1, "GetSixAxisSensorStates through it");
    }
    // Several styles in one set: the style the npad is shown in, two handles when the set holds a two-sensor style
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_FULLKEY | STYLE_JOYDUAL) == 2 && h[0] == 0x5 && h[1] == 0x10005 && six_connected(h[0]), "host handles: FullKey | JoyDual while shown as a pair: the pair's two");
    CHECK(h_hid_SixHandles(h, 1, &id, STYLE_FULLKEY | STYLE_JOYDUAL) == 1, "host handles: never more than asked");
    g_mode = 0;
    CHECK(h_hid_ImuSamples(s, 4, 0x20004 | 0x20u << 8) == 4 && h_hid_ImuSamples(s, 4, 0x4 | 0x20u << 8) == 4, "imu: handheld on 0x20, the SDK's device 2 and the host's device 0");
    g_mode = 1; h_hid_SetStyles(0xffffffffu);
    CHECK(h_hid_ImuSamples(s, 4, 0x20003) == 4 && h_hid_ImuSamples(s, 4, 0x5) == 0, "imu: full key supported: the full-key handle, not the pair's");
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_FULLKEY | STYLE_JOYDUAL) == 2 && h[0] == 0x3 && h[1] == 0x10003 && six_connected(h[0]) && six_connected(h[1]), "host handles: FullKey | JoyDual while shown as full key: full key, two handles as before");
    // Player 2 on No2 with a live controller: its own motion; No3 without a player: none
    dm_pad_players(3);
    float a2[3] = {0, 0, -1}, w2[3] = {0, 0, 3.14159265f}; dm_motion_set(1, a2, w2);
    CHECK(h_hid_ImuSamples(s, 1, 0x20103) == 1 && fabsf(s[0].gyro[2] + 3.14159265f) < 1e-3f, "imu: player 2's npad (No2) reads player 2's motion");
    CHECK(h_hid_ImuSamples(s, 1, 0x20003) == 1 && s[0].gyro[2] == 0, "imu: player 1's npad does not");
    CHECK(h_hid_ImuSamples(s, 1, 0x20203) == 0, "imu: No3 has no controller: none");
    dm_motion_clear(1); dm_pad_players(1);
    // A pair split by single assignment: the left Joy-Con on No1 (JoyLeft, 0x6), the right one on No2 (JoyRight, 0x10107), each with its own sensor
    h_hid_SetStyles(STYLE_FULLKEY | STYLE_JOYDUAL | STYLE_JOYLEFT | STYLE_JOYRIGHT);
    dm_joy_attach(0, 3); h_hid_JoySingle(&(uint32_t){0});
    float al[3] = {0, 0, -1}, wl[3] = {1, 0, 0}, wr[3] = {0, 1, 0};
    dm_motion_set_joy(0, 0, al, wl, motion_now); dm_motion_set_joy(0, 1, al, wr, motion_now);
    CHECK(h_hid_ImuSamples(s, 1, 0x6) == 1 && fabsf(s[0].gyro[0] + 1) < 1e-3f && fabsf(s[0].gyro[1]) < 1e-3f, "imu: JoyLeft on No1 reads the left Joy-Con's sensor");
    CHECK(h_hid_ImuSamples(s, 1, 0x10107) == 1 && fabsf(s[0].gyro[1] + 1) < 1e-3f && fabsf(s[0].gyro[0]) < 1e-3f, "imu: JoyRight on No2 reads the right Joy-Con's sensor");
    CHECK(h_hid_ImuSamples(s, 1, 0x5) == 0, "imu: no pair on No1 then");
    h_hid_JoyDual(&(uint32_t){0}); dm_motion_clear_joy(0, 0); dm_motion_clear_joy(0, 1); dm_joy_attach(0, 0);
    h_hid_SetStyles(0xffffffffu); ids((uint32_t[]){0, 1, 2, 3, 4, 5, 6, 7}, 8);
    unsetenv("DM_IMU_NOISE"); imu_env_read();
    h_hid_ImuDisable(); g_phone_mode = -1;
}

// The IMU samples' white noise (the SDK's stillness detector tests the gyro for it, sdk+0x736fb0): on by default, the same for a tick read again, different per device, zero mean
// with the intended spread, uncorrelated from one tick to the next; DM_IMU_NOISE=0 leaves it out
static void t_imu_noise(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0;
    atomic_store(&g_shake_t0, 0); atomic_store(&g_shake_end, 0);
    unsetenv("DM_IMU_NOISE"); imu_env_read();
    uint32_t h[2] = {0x20003, 0x10003};   // the SDK's handles of player 1's full-key controller, two device indexes
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    enum { N = 4000 }; static float gx[N], az[N];
    double sg = 0, sg2 = 0, sa = 0, sa2 = 0, lag = 0; int differ = 0, again = 1;
    TestImuSample s[16], t[16];
    for (int i = 0; i < N; i++) {
        hwm_clear(), guest_now = 5000000000ull + (uint64_t)i * SIX_PERIOD_NS;
        if (h_hid_ImuSamples(s, 1, h[0]) != 1) { CHECK(0, "imu noise: a sample each tick"); break; }
        gx[i] = s[0].gyro[0]; az[i] = s[0].acc[2];
        sg += gx[i]; sg2 += (double)gx[i] * gx[i]; sa += az[i] - 9.80665; sa2 += (az[i] - 9.80665) * (az[i] - 9.80665);
        if (i) lag += (double)gx[i] * gx[i - 1];
        if (i < 50) { h_hid_ImuSamples(t, 1, h[1]); differ += t[0].gyro[0] != s[0].gyro[0]; }
    }
    double gsd = sqrt(sg2 / N), asd = sqrt(sa2 / N), want_g = 0.1 * M_PI / 180, want_a = 0.00005 * 9.80665;
    CHECK(fabs(sg / N) < 4 * want_g / sqrt(N) && fabs(gsd - want_g) < 0.1 * want_g, "imu noise: the gyro's is zero-mean, 0.1 deg/s per axis");
    CHECK(fabs(sa / N) < 4 * want_a / sqrt(N) && fabs(asd - want_a) < 0.1 * want_a, "imu noise: the acceleration's is zero-mean, 0.05 mG per axis");
    CHECK(fabs(lag / (N - 1) / (gsd * gsd)) < 0.08, "imu noise: white (no correlation from one tick to the next)");
    {   // the SDK's stillness test (sdk+0x736850): the lag-1 autocovariance of 64 accelerations (m/s^2) on x and z within 1e-6, for every window of a still controller
        int still = 1;
        for (int w = 0; w + 64 <= N; w += 16) {
            double m = 0, c = 0; for (int i = w; i < w + 64; i++) m += az[i]; m /= 64;
            for (int i = w; i < w + 63; i++) c += (az[i] - m) * (az[i + 1] - m);
            still &= fabs(c / 63) < 1e-6;
        }
        CHECK(still, "imu noise: small enough that the SDK's stillness detector finds a still controller still");
    }
    CHECK(differ >= 48, "imu noise: each device has its own");
    hwm_clear(), guest_now = 6000000000ull;
    h_hid_ImuSamples(s, 16, h[0]); guest_now += 3 * SIX_PERIOD_NS; h_hid_ImuSamples(t, 16, h[0]);
    for (int i = 3; i < 16; i++) again &= !memcmp(&s[i], &t[i - 3], sizeof s[i]);   // NOLINT(bugprone-suspicious-memory-comparison): a tick read again is the same bytes
    CHECK(again, "imu noise: a tick read again reads the same");
    setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    CHECK(h_hid_ImuSamples(s, 1, h[0]) == 1 && s[0].gyro[0] == 0 && s[0].acc[2] == 9.80665f, "imu noise: DM_IMU_NOISE=0 leaves it out");
    setenv("DM_IMU_NOISE", "0.5", 1); imu_env_reread();
    CHECK(g_imu_noise_dps == 0.05 && g_imu_noise_g == IMU_NOISE_G / 2, "imu noise: a single number scales the defaults");
    setenv("DM_IMU_NOISE", "lots", 1); imu_env_reread();
    CHECK(g_imu_noise_dps == IMU_NOISE_DPS && g_imu_noise_g == IMU_NOISE_G, "imu noise: a value that is no number keeps the defaults (and logs)");
    setenv("DM_IMU_NOISE", "1,-2", 1); imu_env_reread();
    CHECK(g_imu_noise_dps == IMU_NOISE_DPS && g_imu_noise_g == IMU_NOISE_G, "imu noise: a negative spread keeps the defaults");
    setenv("DM_IMU_NOISE", "1,0", 1); imu_env_reread();
    CHECK(h_hid_ImuSamples(s, 1, h[0]) == 1 && s[0].gyro[0] != 0 && s[0].acc[2] == 9.80665f, "imu noise: DM_IMU_NOISE=<deg/s>,<mG> sets each spread (gyro only here), read again on DM_CTL's env");
    setenv("DM_IMU_NOISE", "0,0.2", 1); imu_env_reread();
    CHECK(h_hid_ImuSamples(s, 1, h[0]) == 1 && s[0].gyro[0] == 0 && s[0].acc[2] != 9.80665f && fabsf(s[0].acc[2] - 9.80665f) < 0.02f, "imu noise: acceleration only");
    unsetenv("DM_IMU_NOISE"); imu_env_read();
    {   // DM_IMU_LOG=<n>: every n-th raw sample and IMU answer is logged (read once); unset or 0: none
        _Atomic uint64_t c = 0;
        atomic_store(&g_imu_log, -1); unsetenv("DM_IMU_LOG");
        CHECK(!imu_log_due(&c) && !imu_log_due(&c), "imu log: off by default");
        atomic_store(&g_imu_log, -1); setenv("DM_IMU_LOG", "3", 1); c = 0;
        int due[6]; for (int i = 0; i < 6; i++) due[i] = imu_log_due(&c);
        CHECK(due[0] && !due[1] && !due[2] && due[3] && !due[4] && !due[5], "imu log: every third");
        TestImuSample q[2]; CHECK(h_hid_ImuSamples(q, 2, h[0]) == 2, "imu log: answers unchanged while logging");
        float a1[3] = {0, 0, -1}, r1[3] = {0}; dm_motion_set(0, a1, r1); dm_motion_clear(0);
        atomic_store(&g_imu_log, -1); unsetenv("DM_IMU_LOG");
    }
    h_hid_ImuDisable(); hwm_clear(), guest_now = 1000000000ull; g_phone_mode = -1;
}

// Live motion through the history ring (live_ring): a source at 100 / 125 / 200 / 833 Hz with jittered sample times drives player 1; the IMU answers a game reads once a frame
// (60 Hz) number their ticks consecutively at 5 ms, end at the clock's tick (the newest sample held past its time), and carry the source's signal at each tick's own time,
// interpolated (a ramp in the gyro comes out as that ramp, without steps); GetSixAxisSensorStates' history follows the same ticks, newest first
void *writer(void *arg) {   // t_live_ring's input thread: 1,000 samples 0.1 ms apart, all at or before the fixed host clock
    (void)arg; float a[3] = {0, 0, -1};
    for (int i = 0; i < 1000; i++) { float w[3] = {0, 0, (float)i * 1e-3f}; dm_motion_set_hid(0, a, w, 90000000000ull - 100000000ull + (uint64_t)i * 100000ull); }
    return NULL;
}
static double ramp_rads(uint64_t t_ns) { return 0.5 + 2.0 * (double)(t_ns % 4000000000ull) / 1e9; }   // rad/s, rising 2 rad/s every second
static void t_live_ring(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0;
    setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    uint32_t h = 0x20003; uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    static const double rates[4] = {100, 125, 200, 833};
    for (int ri = 0; ri < 4; ri++) {
        uint64_t t0 = 20000000000ull + (uint64_t)ri * 10000000000ull, period = (uint64_t)(1e9 / rates[ri]);
        dm_motion_clear_hid(0); dm_motion_clear(0);
        uint64_t next_sample = t0, next_frame = t0 + 50000000, last_sample_t = 0, rng = 12345 + (uint64_t)ri;
        int64_t prev_last = -1; int ok_num = 1, ok_future = 1, ok_value = 1, ok_mono = 1, ok_delta = 1, ok_states = 1, answers = 0;
        for (uint64_t t = t0; t < t0 + 1000000000ull; t += 250000) {   // 0.25 ms steps of both clocks
            motion_now = t; guest_now = t;
            if (t >= next_sample) {
                rng = rng * 6364136223846793005ull + 1442695040888963407ull;
                uint64_t ts = t - (rng >> 33) % (period / 5 + 1);   // stamped up to a fifth of a period before it arrives
                if (ts > last_sample_t) {
                    float a[3] = {0, 0, -1}, w[3] = {0, 0, (float)ramp_rads(ts)};
                    dm_motion_set_hid(0, a, w, ts); last_sample_t = ts;
                }
                next_sample += period;
            }
            if (t >= next_frame) {
                next_frame += 16666667; answers++;
                TestImuSample o[16]; int k = h_hid_ImuSamples(o, 16, h);
                if (k <= 0) { ok_num = 0; continue; }
                for (int i = 0; i < k; i++) {
                    if (i && o[i].sampling != o[i - 1].sampling + 1) ok_num = 0;
                    if (o[i].delta_ns != 5000000) ok_delta = 0;
                    uint64_t tick_t = (uint64_t)(o[i].sampling - 1) * 5000000ull;
                    // inside the data the gyro is the ramp at the tick's own time (the IMU negates it: -rad/s); past the newest sample, that sample held
                    double want = ramp_rads(tick_t <= last_sample_t ? tick_t : last_sample_t);
                    if (tick_t > t0 + 100000000ull && fabs(o[i].gyro[2] + want) > 1e-3) ok_value = 0;
                }
                if (o[k - 1].sampling != (int64_t)(t / 5000000) + 1) ok_future = 0;   // the answer ends at the clock's tick
                if (o[k - 1].sampling < prev_last) ok_mono = 0;
                prev_last = o[k - 1].sampling;
                SixAxisState st[4]; uint32_t hh = 0x3;
                if (h_hid_SixStates(st, 4, &hh) != 4 || st[0].sampling != o[k - 1].sampling - 1 || st[1].sampling != st[0].sampling - 1 ||
                    fabs(st[0].gyro[2] * 2 * M_PI - ramp_rads((uint64_t)st[0].sampling * 5000000ull <= last_sample_t ? (uint64_t)st[0].sampling * 5000000ull : last_sample_t)) > 1e-3) ok_states = 0;
            }
        }
        char what[96];
        snprintf(what, sizeof what, "live ring at %.0f Hz: %d answers, consecutive 5 ms ticks", rates[ri], answers); CHECK(ok_num && ok_delta && answers > 50, what);
        snprintf(what, sizeof what, "live ring at %.0f Hz: answers end at the clock's tick, never going back", rates[ri]); CHECK(ok_future && ok_mono, what);
        snprintf(what, sizeof what, "live ring at %.0f Hz: each tick carries the signal at its own time (interpolated, no steps)", rates[ri]); CHECK(ok_value, what);
        snprintf(what, sizeof what, "live ring at %.0f Hz: the six-axis history follows the same ticks, newest first", rates[ri]); CHECK(ok_states, what);
    }
    {   // the input thread adds samples while a game thread reads answers (ThreadSanitizer checks the ring's lock in t_hid_tsan); fixed clocks, sample times below them
        static _Atomic int stop; atomic_store(&stop, 0);
        motion_now = guest_now = 90000000000ull; dm_motion_clear_hid(0);
        pthread_t wr; void *writer(void *); CHECK(!pthread_create(&wr, NULL, writer, NULL), "live ring: writer thread");
        int good = 1; for (int i = 0; i < 2000; i++) { TestImuSample q[16]; int k = h_hid_ImuSamples(q, 16, h); for (int j = 1; j < k; j++) good &= q[j].sampling == q[j - 1].sampling + 1; }
        pthread_join(wr, NULL);
        CHECK(good, "live ring: answers stay consecutive while samples arrive");
    }
    // A stream gone stale: back to the flat synthetic sensor at the current tick
    motion_now += MOTION_STALE_NS + 1; guest_now = motion_now;
    TestImuSample o[2]; CHECK(h_hid_ImuSamples(o, 2, h) == 2 && o[1].gyro[2] == 0 && o[1].sampling == (int64_t)(guest_now / 5000000) + 1, "live ring: stale, synthetic again at the current tick");
    // The ring keeps only LIVE_RING samples: memory is bounded however long a source runs
    CHECK(g_hid_motion[0].count <= LIVE_RING, "live ring: bounded");
    dm_motion_clear_hid(0); h_hid_ImuDisable(); unsetenv("DM_IMU_NOISE"); imu_env_read();
    guest_now = 1000000000ull; motion_now = 1000000000ull; g_phone_mode = -1;
}

// The sampling numbers a game reads never go back and keep up with the clock, whatever the live source does: synthetic → live with a first sample stamped late, a player's
// GameController and HID streams with different latencies taking turns, a game polling faster than the source, a source going stale and coming back, the guest clock's lag
// changing mid-stream (a freeze), equal and out-of-order stamps. Each read checks: IMU answers and six-axis histories consecutive, ending at the clock's tick or above the last
// one, and the newest number never below the previous read's
typedef struct { int64_t imu_last, st_last; int ok_mono, ok_consec, ok_clock, ok_finite; } edge_watch;
static void edge_read(edge_watch *w) {
    TestImuSample o[16]; int k = h_hid_ImuSamples(o, 16, 0x20003);
    int64_t clock_tick = (int64_t)(guest_now / 5000000) + 1;
    if (k <= 0) { w->ok_consec = 0; return; }
    for (int i = 1; i < k; i++) if (o[i].sampling != o[i - 1].sampling + 1) w->ok_consec = 0;
    for (int i = 0; i < k; i++) for (int a = 0; a < 3; a++) if (!isfinite(o[i].acc[a]) || !isfinite(o[i].gyro[a])) w->ok_finite = 0;
    if (o[k - 1].sampling < w->imu_last) w->ok_mono = 0;
    if (o[k - 1].sampling < clock_tick) w->ok_clock = 0;
    w->imu_last = o[k - 1].sampling;
    SixAxisState st[4]; uint32_t hh = 0x3;
    if (h_hid_SixStates(st, 4, &hh) != 4 || st[1].sampling != st[0].sampling - 1) w->ok_consec = 0;
    if (st[0].sampling < w->st_last) w->ok_mono = 0;
    if (st[0].sampling < clock_tick - 1) w->ok_clock = 0;
    w->st_last = st[0].sampling;
}
static void edge_clocks(uint64_t t) { motion_now = t; guest_now = t - (atomic_load(&g_test_lag)); }
static void t_live_edges(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    dm_motion_clear(0); dm_motion_clear_hid(0); hwm_clear();
    edge_watch w = {0, 0, 1, 1, 1, 1};
    float a[3] = {0, 0, -1}, w0[3] = {0, 0, 1};
    uint64_t t = 200000000000ull; atomic_store(&g_test_lag, 0);
    // synthetic, then a first live sample stamped 15 ms (3 ticks) late
    for (int i = 0; i < 10; i++) { edge_clocks(t += 1000000); edge_read(&w); }
    dm_motion_set_hid(0, a, w0, t - 15000000); edge_read(&w);
    for (int i = 0; i < 20; i++) { edge_clocks(t += 4000000); dm_motion_set_hid(0, a, w0, t - 15000000); edge_read(&w); }
    CHECK(w.ok_mono && w.ok_consec && w.ok_clock, "edges: synthetic to live with a late first sample, numbers never go back");
    // GameController (2 ms late) and HID (20 ms late) streams of player 1 taking turns
    for (int i = 0; i < 40; i++) {
        edge_clocks(t += 8000000);
        if (i % 10 < 5) dm_motion_set(0, a, w0); else dm_motion_clear(0);
        dm_motion_set_hid(0, a, w0, t - 20000000); edge_read(&w);
    }
    CHECK(w.ok_mono && w.ok_consec && w.ok_clock, "edges: GameController and HID streams with different latencies taking turns");
    dm_motion_clear(0);
    // polling every 1 ms against a 100 Hz source: the numbers follow the clock, not the source
    int64_t first = w.imu_last;
    for (int i = 0; i < 100; i++) { edge_clocks(t += 1000000); if (i % 10 == 0) dm_motion_set_hid(0, a, w0, t - 2000000); edge_read(&w); }
    CHECK(w.ok_mono && w.ok_clock && w.imu_last - first >= 19, "edges: a game polling faster than its source reads numbers that keep up with the clock");
    // live → stale (no samples for 400 ms) → live again
    for (int i = 0; i < 100; i++) { edge_clocks(t += 4000000); edge_read(&w); }
    for (int i = 0; i < 10; i++) { edge_clocks(t += 4000000); dm_motion_set_hid(0, a, w0, t - 10000000); edge_read(&w); }
    CHECK(w.ok_mono && w.ok_consec && w.ok_clock, "edges: live, stale, live again");
    // the guest clock frozen for 50 ms (its lag grows), samples arriving meanwhile, then running again
    uint64_t frozen = guest_now;
    for (int i = 0; i < 10; i++) { motion_now = (t += 5000000); dm_motion_set_hid(0, a, w0, t - 3000000); edge_read(&w); }
    atomic_store(&g_test_lag, motion_now - frozen);
    for (int i = 0; i < 20; i++) { edge_clocks(t += 4000000); dm_motion_set_hid(0, a, w0, t - 3000000); edge_read(&w); }
    CHECK(w.ok_mono && w.ok_consec && w.ok_clock && w.ok_finite, "edges: a lag change mid-stream (a freeze) keeps the numbers going");
    // equal and out-of-order stamps: dropped, nothing goes back
    edge_clocks(t += 4000000); uint64_t st0 = t - 2000000;
    dm_motion_set_hid(0, a, w0, st0); dm_motion_set_hid(0, a, w0, st0); dm_motion_set_hid(0, a, w0, st0 - 1000000); edge_read(&w);
    edge_clocks(t += 4000000); dm_motion_set_hid(0, a, w0, t - 1000000); edge_read(&w);
    CHECK(w.ok_mono && w.ok_consec && w.ok_clock && w.ok_finite, "edges: equal and out-of-order stamps");
    // the direction between two samples is a rotation (normalized lerp): orthonormal rows
    {
        motion_sample r2[2] = {{.t = 0, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}}, {.t = 10, .dir = {0, 1, 0, -1, 0, 0, 0, 0, 1}}}, o2;
        live_at(r2, 2, 5, &o2);
        float n0 = o2.dir[0] * o2.dir[0] + o2.dir[1] * o2.dir[1] + o2.dir[2] * o2.dir[2], d01 = o2.dir[0] * o2.dir[3] + o2.dir[1] * o2.dir[4] + o2.dir[2] * o2.dir[5];
        CHECK(fabsf(n0 - 1) < 1e-4f && fabsf(d01) < 1e-4f && fabsf(o2.dir[0] - 0.7071f) < 1e-3f && fabsf(o2.dir[1] - 0.7071f) < 1e-3f, "edges: the direction halfway between a quarter turn is the eighth turn, a rotation");
    }
    {   // half a turn apart (the blend vanishes) or 60 ms apart: the nearer sample's direction, a rotation
        motion_sample r3[2] = {{.t = 0, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}}, {.t = 10, .dir = {-1, 0, 0, 0, -1, 0, 0, 0, 1}}}, o3;
        live_at(r3, 2, 4, &o3); CHECK(o3.dir[0] == 1 && o3.dir[4] == 1 && o3.dir[8] == 1, "edges: half a turn apart, the nearer direction (the earlier one)");
        live_at(r3, 2, 6, &o3); CHECK(o3.dir[0] == -1 && o3.dir[4] == -1, "edges: and the later one past halfway");
        motion_sample r4[2] = {{.t = 0, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}}, {.t = 60000000, .dir = {0, 1, 0, -1, 0, 0, 0, 0, 1}}};
        live_at(r4, 2, 20000000, &o3); CHECK(o3.dir[0] == 1 && o3.dir[1] == 0, "edges: samples 60 ms apart: no blend, the nearer direction");
    }
    {   // the clock going back by more than a second (a reset): the marks start again from it instead of pinning the old numbers
        uint64_t was = guest_now;
        guest_now += 10000000000ull; edge_read(&w);
        guest_now = was; TestImuSample q[2];
        CHECK(h_hid_ImuSamples(q, 2, 0x20003) == 2 && q[1].sampling == (int64_t)(guest_now / 5000000) + 1, "edges: the clock 10 s back: the IMU numbers follow it");
        SixAxisState st5; uint32_t h5 = 0x3; CHECK(h_hid_SixStates(&st5, 1, &h5) == 1 && st5.sampling == (int64_t)(guest_now / 5000000), "edges: and the six-axis history's");
        guest_now += 3000000; CHECK(h_hid_ImuSamples(q, 2, 0x20003) == 2 && q[1].sampling == (int64_t)(guest_now / 5000000) + 1, "edges: then they follow it on");
        TestImuSample q2[2]; guest_now -= 500000000; CHECK(h_hid_ImuSamples(q2, 2, 0x20003) == 2 && q2[1].sampling == q[1].sampling, "edges: back by half a second only: held at the mark");
    }
    atomic_store(&g_test_lag, 0); dm_motion_clear_hid(0); h_hid_ImuDisable(); unsetenv("DM_IMU_NOISE"); imu_env_read();
    guest_now = 1000000000ull; motion_now = 1000000000ull; g_phone_mode = -1;
}

// What the real-controller session showed (Joy-Con 2 pair, DualSense): a first all-zero sample is no reading; a GameController's gyro offset is learned at rest and taken off;
// LoadSixAxisSensorCalibrationParameter claims a zero bias only for a sensor whose offset is taken off
static void t_live_rest(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear();
    uint64_t t = 300000000000ull; motion_now = guest_now = t;
    dm_motion_clear(0); dm_motion_clear_hid(0);
    float z[3] = {0, 0, 0}, a[3] = {0, 0, -1}, off[3] = {0.006f, 0.0035f, -0.0115f};
    unsigned before = g_motion[0].count; dm_motion_set(0, z, z); CHECK(g_motion[0].count == before && !motion_live(0, &(motion_state){0}), "session: an all-zero sample is dropped");
    uint8_t cal[0x744]; uint32_t h = 0x3; memset(cal, 0xAB, sizeof cal);
    dm_motion_set(0, a, off);
    CHECK(h_hid_SixCalLoad(cal, &h) == RES_SIX_CAL_NONE && cal[0] == 0 && cal[0x743] == 0, "session: a GameController with its offset not learned: no stored calibration (the buffer zeroed)");
    for (int i = 0; i < 150; i++) { motion_now = guest_now = (t += 15000000); dm_motion_set(0, a, off); }
    CHECK(g_motion[0].rest.have, "session: the GameController's offset learned at rest (two agreeing seconds)");
    for (int i = 0; i < 60; i++) { motion_now = guest_now = (t += 15000000); dm_motion_set(0, a, off); }   // past the ramp
    SixAxisState st; CHECK(six_states(&st, 1, h) == 1 && fabsf(st.gyro[0]) < 1e-4f && fabsf(st.gyro[2]) < 1e-4f, "session: and taken off its six-axis state");
    // a gap of 150 ms breaks the rest window; a stale stream forgets the offset
    unsigned n_before = g_motion[0].rest.n; motion_now = guest_now = (t += 150000000); dm_motion_set(0, a, off);
    CHECK(g_motion[0].rest.n <= 1 && n_before >= 1 && g_motion[0].rest.have, "session: a gap starts the rest window again, the offset kept");
    motion_now = guest_now = (t += MOTION_STALE_NS + 1); dm_motion_set(0, a, off);
    CHECK(!g_motion[0].rest.have, "session: a stale stream forgets the offset");
    for (int i = 0; i < 150; i++) { motion_now = guest_now = (t += 15000000); dm_motion_set(0, a, off); }
    memset(cal, 0xAB, sizeof cal); CHECK(h_hid_SixCalLoad(cal, &h) == 0 && cal[0] == 0 && cal[0x743] == 0, "session: then a zero bias");
    // a later all-zero sample on a fresh stream is a reading (a free fall), kept
    unsigned c0 = g_motion[0].count; motion_now = guest_now = (t += 15000000); dm_motion_set(0, z, z);
    CHECK(g_motion[0].count == c0 + 1 || g_motion[0].count == LIVE_RING, "session: zeros on a live stream are kept (free fall)");
    dm_motion_clear(0);
    CHECK(!g_motion[0].rest.have, "session: a controller leaving forgets the offset (another may take the player)");
    dm_motion_set_hid(0, a, off, t); CHECK(h_hid_SixCalLoad(cal, &h) == 0, "session: a HID stream (its front end takes the offset off): zero bias");
    dm_motion_clear_hid(0); CHECK(h_hid_SixCalLoad(cal, &h) == 0, "session: no live source: zero bias");
    {   // DM_IMU_AXES: the IMU samples' axes permuted, rotation times the map's determinant
        setenv("DM_IMU_NOISE", "0", 1); setenv("DM_IMU_AXES", "y,-x,z", 1); imu_env_read();
        float tilt[3] = {0.5f, 0, -0.8660254f}, turn[3] = {0, 0, 3.14159265f}; uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
        motion_now = guest_now = (t += 5000000); dm_motion_set_hid(0, tilt, turn, t);
        TestImuSample q[1];
        CHECK(h_hid_ImuSamples(q, 1, 0x20003) == 1 && fabsf(q[0].acc[1] - 0.5f * 9.80665f) < 1e-3f && fabsf(q[0].acc[0]) < 1e-3f && fabsf(q[0].gyro[2] + 3.14159265f) < 1e-3f,
              "imu axes: y,-x,z puts -x on y (+4.9 m/s2 there) and keeps the turn about z");
        setenv("DM_IMU_AXES", "y,x,z", 1); imu_env_read();
        CHECK(h_hid_ImuSamples(q, 1, 0x20003) == 1 && fabsf(q[0].gyro[2] - 3.14159265f) < 1e-3f, "imu axes: a mirror (y,x,z) turns the rotation's sign, so it stays a proper rotation");
        unsetenv("DM_IMU_AXES"); unsetenv("DM_IMU_NOISE"); imu_env_read(); h_hid_ImuDisable(); dm_motion_clear_hid(0);
    }
    guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// A single Joy-Con the game is shown as a pair (Switch Sports Resort supports Handheld | JoyDual): both pair handles read that Joy-Con's own stream (upright, a pair's frame), not
// the merged pad stream (a sideways Joy-Con's frame); shown as full key it reads the merged stream; a right Joy-Con split as JoyRight reads its own
static void t_single_joy_as_pair(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear(); setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    uint64_t t = 400000000000ull; motion_now = guest_now = t;
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    dm_joy_attach(0, 2);   // the right Joy-Con alone
    float a[3] = {0, 0, -1}, side[3] = {0.5f, 0, 0}, own[3] = {0, 0.7f, 0};
    dm_motion_set_hid(0, a, side, t); dm_motion_set_joy(0, 1, a, own, t);
    TestImuSample q[1];
    h_hid_SetStyles(STYLE_HANDHELD | STYLE_JOYDUAL);
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f && fabsf(q[0].gyro[0]) < 1e-4f, "single Joy-Con as a pair: device 0 reads the Joy-Con's own stream");
    CHECK(h_hid_ImuSamples(q, 1, 0x10005) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "and device 1 too");
    h_hid_SetStyles(STYLE_FULLKEY);
    CHECK(h_hid_ImuSamples(q, 1, 0x20003) == 1 && fabsf(q[0].gyro[0] + 0.5f) < 1e-4f, "shown as full key: the merged stream (a sideways Joy-Con)");
    h_hid_SetStyles(STYLE_FULLKEY | STYLE_JOYDUAL | STYLE_JOYLEFT | STYLE_JOYRIGHT);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYRIGHT && h_hid_ImuSamples(q, 1, 0x10007) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "a game with JoyRight: its own stream, as before");
    unsetenv("DM_IMU_NOISE"); imu_env_read();
    {   // interpolated live ticks are untouched; held ones (past the newest sample) get a dither under the real floor, so no two ticks in a row are alike
        dm_motion_clear_joy(0, 1); float w1[3] = {0, 0.7f, 0}, w2[3] = {0, 0.8f, 0};
        motion_now = guest_now = (t += 1000000000ull); dm_motion_set_joy(0, 1, a, w1, t - 20000000); dm_motion_set_joy(0, 1, a, w2, t - 10000000);
        TestImuSample r[4]; CHECK(h_hid_ImuSamples(r, 4, 0x10007) == 4, "dither: four ticks");
        // ticks at t-15, -10 (the newest sample), -5, 0 ms: the first is interpolated exactly, the newest-sample tick and those after it are held and dithered
        CHECK(fabsf(r[0].gyro[1] + 0.75f) < 1e-6f, "dither: an interpolated tick is the controller's own value, untouched");
        int alike = 0; for (int i = 1; i < 4; i++) alike |= !memcmp(&r[i].gyro, &r[i - 1].gyro, sizeof r[i].gyro) || !memcmp(&r[i].acc, &r[i - 1].acc, sizeof r[i].acc);   // NOLINT(bugprone-suspicious-memory-comparison): held ticks must differ in their bits (the dither)
        CHECK(!alike && fabsf(r[3].gyro[1] + 0.8f) < 1e-3f && fabsf(r[3].acc[2] - 9.80665f) < 1e-3f, "dither: held ticks differ from each other, and stay at the held value");
    }
    dm_motion_clear_joy(0, 1); dm_motion_clear_hid(0); dm_joy_attach(0, 0); h_hid_SetStyles(0xffffffffu); h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// An intact pair whose one half pauses (a Joy-Con 2 skipping reports past the stale time): that half's handle lies flat, not the pad's merged stream (a sideways frame); a pad
// whose Joy-Cons never reported their own motion still gets the merged one
static void t_pair_half_pause(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear(); setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    uint64_t t = 800000000000ull; motion_now = guest_now = t;
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    h_hid_SetStyles(STYLE_HANDHELD | STYLE_JOYDUAL);
    float a[3] = {0, 0, -1}, wl[3] = {0, 0.3f, 0}, wr[3] = {0, 0.7f, 0}, side[3] = {0.5f, 0, 0};
    TestImuSample q[1];
    dm_joy_attach(0, 3); dm_motion_set_joy(0, 0, a, wl, t); dm_motion_set_joy(0, 1, a, wr, t); dm_motion_set_hid(0, a, side, t);
    motion_now = guest_now = (t += 400000000ull);   // 400 ms on: the right one and the merged stream go on, the left one paused
    dm_motion_set_joy(0, 1, a, wr, t); dm_motion_set_hid(0, a, side, t);
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[0]) < 1e-4f && fabsf(q[0].gyro[1]) < 1e-4f, "paused half: flat, not the merged stream's sideways frame");
    CHECK(h_hid_ImuSamples(q, 1, 0x10005) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "paused half: the other half keeps its own");
    for (int i = 0; i < 50; i++) { motion_now = guest_now = (t += 100000000ull); dm_motion_set_joy(0, 1, a, wr, t); dm_motion_set_hid(0, a, side, t); }   // 5 s more
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[0]) < 1e-4f && fabsf(q[0].gyro[1]) < 1e-4f, "paused half: still flat after 5 s, no cliff to the merged stream");
    dm_motion_set_joy(0, 0, a, wl, t);
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1] + 0.3f) < 1e-4f, "paused half: its own again once it reports");
    dm_joy_attach(0, 1); motion_now = guest_now = (t += 1000000);   // the right one detached: the left alone (no single style, shown as a pair), its own stream on both handles
    dm_motion_set_joy(0, 0, a, wl, t); dm_motion_set_hid(0, a, side, t);
    CHECK(h_hid_ImuSamples(q, 1, 0x10005) == 1 && fabsf(q[0].gyro[0]) < 1e-4f, "a detached half: not the merged stream either (the pair lost it: flat)");
    dm_joy_attach(0, 0); dm_motion_clear_joy(0, 0); dm_motion_clear_joy(0, 1);
    dm_joy_attach(0, 3); motion_now = guest_now = (t += 1000000);   // Joy-Cons whose controller gives no per-Joy-Con motion: the merged stream
    dm_motion_set_hid(0, a, side, t);
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[0] + 0.5f) < 1e-4f, "no per-Joy-Con motion at all: the merged stream, as before");
    dm_joy_attach(0, 0); dm_motion_clear_hid(0); unsetenv("DM_IMU_NOISE"); imu_env_read();
    h_hid_SetStyles(0xffffffffu); h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// Playout delay (DM_IMU_DELAY, motion_lag): a live stream at 33 Hz (Joy-Con 2: sample times from the controller's clock, arrivals up to 4 ms late) or 67 Hz (GameController:
// sample times are the jittered arrivals). The signal is a ramp of the gyro (1 rad/s per second of sample time), so a tick reads its data time: exactly the ramp at its time
// minus the delay when it lies between two real samples, less when it holds one. The game reads 16 ticks every 16.7 ms and takes each tick once (by sampling number), as the
// fusion does
typedef struct { unsigned total, held, bad_seq, back, jump; uint64_t delay_min, delay_max; } delay_stats;
static uint32_t delay_rng(uint32_t *x) { *x = *x * 1664525u + 1013904223u; return *x >> 8; }
static uint64_t applied_delay(tick_hwm *w, uint32_t h) { int at = hwm_slot(w, h); return at < 0 ? 0 : atomic_load(&w->delay[at]); }
// delays: the DM_IMU_DELAY values to switch through, one per segment of seg_s seconds ("" for auto); counts the ticks after the first settle_s of each segment
static delay_stats live_delay_run(double hz, int joy_clock, const char *const *delays, int nd, double seg_s, double settle_s, uint64_t t0) {
    delay_stats st = {0, 0, 0, 0, 0, ~0ull, 0};
    const uint64_t period = (uint64_t)(1e9 / hz), jitter = 4000000, frame = 16666667, seg_ns = (uint64_t)(seg_s * 1e9);
    uint32_t rng = 12345; uint64_t stamp = t0, arr = t0 + (joy_clock ? delay_rng(&rng) % jitter : 0), nf = t0 + frame;
    if (!joy_clock) stamp = arr;
    dm_motion_clear_hid(0); hwm_clear();
    float a[3] = {0, 0, -1};
    int64_t last = 0; int seg = -1; double prev_data = -1; int prev_interp = 0;
    for (uint64_t t = t0; t < t0 + (uint64_t)nd * seg_ns; t += 500000) {
        int now_seg = (int)((t - t0) / seg_ns);
        if (now_seg >= nd) break;   // the loop's bound already ends at the last segment; said here so that delays[] is never read past it
        if (now_seg != seg) { seg = now_seg; if (*delays[seg]) setenv("DM_IMU_DELAY", delays[seg], 1); else unsetenv("DM_IMU_DELAY"); imu_env_read(); }
        motion_now = guest_now = t;
        while (arr <= t) {
            float w[3] = {0, (float)((double)(stamp - t0) / 1e9), 0}; dm_motion_set_hid(0, a, w, stamp);
            uint64_t nominal = t0 + (uint64_t)((stamp - t0 + period / 2) / period + 1) * period, late = delay_rng(&rng) % jitter;
            if (joy_clock) { stamp = nominal; arr = nominal + late; } else { stamp = arr = nominal + late; }
        }
        if (t < nf) continue;
        nf += frame;
        TestImuSample q[16]; int k = h_hid_ImuSamples(q, 16, 0x20003);
        uint64_t d = applied_delay(&g_hwm_imu, 0x20003);
        for (int i = 0; i < k; i++) {
            if (q[i].sampling <= last) continue;
            if (last && q[i].sampling != last + 1) st.bad_seq++;
            last = q[i].sampling;
            uint64_t tau = (uint64_t)(q[i].sampling - 1) * SIX_PERIOD_NS;
            double data = -q[i].gyro[1], expect = ((double)(tau - d) - (double)t0) / 1e9;
            int interp = fabs(data - expect) < 2e-5;
            if (prev_data >= 0 && data < prev_data - 2e-5) st.back++;   // the data a game takes never goes back (replayed)
            if (prev_data >= 0 && interp && prev_interp && data - prev_data > 0.006) st.jump++;   // nor skips ahead: 5 ms a tick, 5% more while the delay settles
            prev_data = data; prev_interp = interp;
            if (tau < t0 + (uint64_t)seg * seg_ns + (uint64_t)(settle_s * 1e9)) continue;
            st.total++; if (!interp) st.held++;
            if (d < st.delay_min) st.delay_min = d;
            if (d > st.delay_max) st.delay_max = d;
        }
    }
    dm_motion_clear_hid(0);
    return st;
}
static void t_live_delay(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; setenv("DM_IMU_NOISE", "0", 1);
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    char what[160];
    static const double rates[2] = {33, 67};
    for (int ri = 0; ri < 2; ri++) {
        const char *autod[] = {""};
        delay_stats st = live_delay_run(rates[ri], ri == 0, autod, 1, 3, 1, 700000000000ull + (uint64_t)ri * 10000000000ull);
        snprintf(what, sizeof what, "delay auto at %.0f Hz: %u of %u ticks held (under 2%%), the rest exactly between their two samples", rates[ri], st.held, st.total);
        CHECK(st.total > 350 && st.held * 50 < st.total, what);
        snprintf(what, sizeof what, "delay auto at %.0f Hz: %.3f..%.3f ms, about one sample interval, at most 40", rates[ri], st.delay_min / 1e6, st.delay_max / 1e6);
        CHECK(st.delay_min >= (uint64_t)(1e9 / rates[ri]) && st.delay_max <= 40000000ull && st.delay_max <= (uint64_t)(1e9 / rates[ri]) + 4500000ull, what);   // the spacing, the 4 ms jitter, the test's 0.5 ms step
        CHECK(!st.bad_seq && !st.back && !st.jump, "delay auto: the ticks contiguous, their data never replayed nor skipped (the delay slews in from the stream's start)");
        const char *off[] = {"0"};
        delay_stats so = live_delay_run(rates[ri], ri == 0, off, 1, 1, 0.5, 720000000000ull + (uint64_t)ri * 10000000000ull);
        snprintf(what, sizeof what, "DM_IMU_DELAY=0 at %.0f Hz: read at the present, %u of %u ticks hold the newest sample (the staircase)", rates[ri], so.held, so.total);
        CHECK(so.held * 3 > so.total, what);
    }
    const char *fixed[] = {"40"};
    delay_stats sf = live_delay_run(33, 1, fixed, 1, 1, 0.5, 740000000000ull);
    snprintf(what, sizeof what, "DM_IMU_DELAY=40: every tick reads the signal 40 ms late (%u of %u)", sf.total - sf.held, sf.total);
    CHECK(sf.total > 50 && !sf.held && sf.delay_min == 40000000ull, what);
    const char *sw[] = {"", "0", "40", "", "12"};
    delay_stats ss = live_delay_run(67, 0, sw, 5, 1, 0.9, 760000000000ull);
    CHECK(ss.total > 50 && !ss.bad_seq, "switching the delay (auto, 0, 40, auto, 12) keeps the sampling numbers contiguous, never back");
    CHECK(!ss.back && !ss.jump, "switching the delay: the data a game takes never goes back nor jumps ahead (slewed at 5%)");
    snprintf(what, sizeof what, "switching the delay: slewed to the last setting within its second (%.1f ms)", applied_delay(&g_hwm_imu, 0x20003) / 1e6);
    CHECK(applied_delay(&g_hwm_imu, 0x20003) == 12000000ull, what);
    setenv("DM_IMU_DELAY", "0", 1); unsetenv("DM_IMU_NOISE"); imu_env_read();
    h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}
// A stream swapped behind a handle (a one-player game: player 2's controller takes over from player 1's, motion_pick) with a different delay (33 Hz Joy-Con-like, then 67 Hz),
// the game reading on through the swap, or pausing its reads for just under or just over LIVE_DELAY_IDLE_NS (1 s): the data it takes never goes back, and while it reads on,
// never jumps ahead (the delay slews from 34 to 17 ms)
static void swap_run(double pause_s, delay_stats *st) {
    const uint64_t t0 = 820000000000ull + (uint64_t)(pause_s * 1e10), frame = 16666667, swap = t0 + 2000000000ull, pause_end = swap + (uint64_t)(pause_s * 1e9), end = pause_end + 1000000000ull;
    dm_motion_clear_hid(0); dm_motion_clear_hid(1); hwm_clear();
    float a[3] = {0, 0, -1};
    uint32_t rng = 777; uint64_t next = t0, nf = t0 + frame, prev_d = 0, prev_t = 0; int64_t last = 0; double prev_data = -1;
    for (uint64_t t = t0; t < end; t += 500000) {
        motion_now = guest_now = t;
        if (t >= next) {   // the sample's time: up to 4 ms before now (its transport)
            uint64_t stamp = t - delay_rng(&rng) % 4000000; float w[3] = {0, (float)((double)(int64_t)(stamp - t0) / 1e9), 0};
            dm_motion_set_hid(t < swap ? 0 : 1, a, w, stamp); next += t < swap ? 30303030ull : 14925373ull;
        }
        if (t < nf) continue;
        nf += frame;
        if (t >= swap && t < pause_end) continue;   // the game reads nothing
        TestImuSample q[16]; int k = h_hid_ImuSamples(q, 16, 0x20003);
        uint64_t d = applied_delay(&g_hwm_imu, 0x20003);
        // the delay moves at most 5% of the time since the last read (a read after more than 1 s takes its target at once)
        if (prev_t && t - prev_t <= 1000000000ull && (d > prev_d ? d - prev_d : prev_d - d) > (uint64_t)((double)(t - prev_t) * 0.05) + 1000) st->jump++;
        if (d < st->delay_min) st->delay_min = d;
        if (d > st->delay_max) st->delay_max = d;
        prev_d = d; prev_t = t;
        for (int i = 0; i < k; i++) {
            if (q[i].sampling <= last) continue;
            last = q[i].sampling;
            if (-q[i].gyro[1] < prev_data - 2e-5) st->back++;
            prev_data = -q[i].gyro[1]; st->total++;
        }
    }
    dm_motion_clear_hid(0); dm_motion_clear_hid(1);
}
static void t_live_delay_swap(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; setenv("DM_IMU_NOISE", "0", 1); unsetenv("DM_IMU_DELAY"); imu_env_read();
    uint32_t ids_was = atomic_load(&g_ids_supported);
    ids((const uint32_t[]){0, NPAD_HANDHELD}, 2); dm_pad_players(0x3);
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    static const double pauses[3] = {0, 0.99, 1.01};
    for (int i = 0; i < 3; i++) {
        delay_stats st = {0, 0, 0, 0, 0, ~0ull, 0}; swap_run(pauses[i], &st);
        char what[200]; snprintf(what, sizeof what, "stream swapped behind the handle, reads paused %.2f s: the data never goes back (%u), the delay (%.1f..%.1f ms) slewed between reads (%u jumps)",
                                 pauses[i], st.back, st.delay_min / 1e6, st.delay_max / 1e6, st.jump);
        CHECK(st.total > 100 && !st.back && !st.jump && st.delay_max > 30000000ull && applied_delay(&g_hwm_imu, 0x20003) < 20000000ull, what);
    }
    dm_pad_players(1); atomic_store(&g_ids_supported, ids_was);
    setenv("DM_IMU_DELAY", "0", 1); unsetenv("DM_IMU_NOISE"); imu_env_read();
    h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// The IMU conversion end to end, as the SDK's fusion takes it (UpdateSensorFusion feeds nn::imu the sample's acceleration, at +0x10, negated and its angular velocity, at +0x1c, as it is):
// a controller lying still, then tilted slowly about x (30 deg/s for 1.5 s), then still again. Integrating the fusion's angular velocity turns the first acceleration into the last
// one (no drift: the gyro and the accelerometer agree in axes, sign and scale), and the still stretches carry no rotation. DM_IMU_GYRO_SIGN=-1 (the other sign) shows as drift
static double imu_conversion_drift(int *still_ok) {
    hwm_clear(); dm_motion_clear_hid(0);
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    const uint64_t t0 = 840000000000ull, period = 15000000;   // a 67 Hz controller
    const double w = 30 * M_PI / 180, tilt0 = 1.0, tilt1 = 2.5, end = 3.5;
    uint64_t next = t0, nf = t0 + 16666667; int64_t last = 0; int have = 0; double f[3] = {0}, f_end[3] = {0};
    *still_ok = 1;
    for (uint64_t t = t0; t < t0 + (uint64_t)(end * 1e9); t += 500000) {
        motion_now = guest_now = t;
        if (t >= next) {
            double s = (double)(t - t0) / 1e9, th = s < tilt0 ? 0 : s < tilt1 ? w * (s - tilt0) : w * (tilt1 - tilt0);
            float acc[3] = {0, (float)-sin(th), (float)-cos(th)}, rot[3] = {s >= tilt0 && s < tilt1 ? (float)w : 0, 0, 0};
            dm_motion_set_hid(0, acc, rot, t); next += period;
        }
        if (t < nf) continue;
        nf += 16666667;
        TestImuSample q[16]; int k = h_hid_ImuSamples(q, 16, 0x20003);
        for (int i = 0; i < k; i++) {
            if (q[i].sampling <= last) continue;
            last = q[i].sampling;
            double a[3] = {q[i].acc[0], q[i].acc[1], q[i].acc[2]}, n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            double om[3] = {q[i].gyro[0], q[i].gyro[1], q[i].gyro[2]};   // what nn::imu gets: the angular velocity as it is (its acceleration is the sample's negated)
            for (int j = 0; j < 3; j++) a[j] = -a[j];
            double tick_s = (double)((uint64_t)(q[i].sampling - 1) * SIX_PERIOD_NS - t0) / 1e9;
            if ((tick_s > 0.3 && tick_s < tilt0 - 0.1) || tick_s > tilt1 + 0.3) *still_ok &= fabs(om[0]) < 1e-3 && fabs(om[1]) < 1e-3 && fabs(om[2]) < 1e-3;
            if (!have) { for (int j = 0; j < 3; j++) f[j] = a[j] / n; have = 1; continue; }
            double dt = q[i].delta_ns / 1e9, c[3] = {f[1] * om[2] - f[2] * om[1], f[2] * om[0] - f[0] * om[2], f[0] * om[1] - f[1] * om[0]};   // a world-fixed vector in the body: f' = f x w
            for (int j = 0; j < 3; j++) f[j] += c[j] * dt;
            double fn = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]); for (int j = 0; j < 3; j++) f[j] /= fn;
            for (int j = 0; j < 3; j++) f_end[j] = a[j] / n;
        }
    }
    dm_motion_clear_hid(0); h_hid_ImuDisable();
    double dot = f[0] * f_end[0] + f[1] * f_end[1] + f[2] * f_end[2];
    return acos(dot > 1 ? 1 : dot < -1 ? -1 : dot) * 180 / M_PI;
}
static void t_imu_conversion(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; unsetenv("DM_IMU_NOISE"); unsetenv("DM_IMU_DELAY"); unsetenv("DM_IMU_ACC_SIGN"); unsetenv("DM_IMU_GYRO_SIGN"); imu_env_read();
    int still_ok; double drift = imu_conversion_drift(&still_ok);
    char what[160]; snprintf(what, sizeof what, "IMU conversion: a 45 degree tilt integrated from the fusion's gyro ends on the accelerometer's (drift %.2f deg)", drift);
    CHECK(drift < 1.0, what);
    CHECK(still_ok, "IMU conversion: the still stretches carry no rotation");
    setenv("DM_IMU_GYRO_SIGN", "-1", 1); imu_env_read();
    double flipped = imu_conversion_drift(&still_ok);
    snprintf(what, sizeof what, "IMU conversion: the other gyro sign shows as drift (%.0f deg)", flipped);
    CHECK(flipped > 60, what);
    setenv("DM_IMU_ACC_SIGN", "-1", 1); setenv("DM_IMU_GYRO_SIGN", "-1", 1); setenv("DM_IMU_DELAY", "0", 1); imu_env_read();   // main's for the rest
    guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// The six-axis states path (GetSixAxisSensorStates) reads the newest state each frame: no delay by default (DM_SIX_DELAY=0), only the IMU path's; DM_SIX_DELAY sets its own
static void t_live_delay_paths(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear(); setenv("DM_IMU_NOISE", "0", 1);
    unsetenv("DM_IMU_DELAY"); unsetenv("DM_SIX_DELAY"); imu_env_read();
    CHECK(atomic_load(&g_live_delay_cfg[0]) == 0 && atomic_load(&g_live_delay_cfg[1]) == -1, "delay defaults: the states path none, the IMU path the stream's own");
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    uint64_t t0 = 780000000000ull; float a[3] = {0, 0, -1};
    for (int i = 0; i < 40; i++) { uint64_t t = t0 + (uint64_t)i * 30000000ull; motion_now = guest_now = t; float w[3] = {0, (float)(i * 0.03), 0}; dm_motion_set_hid(0, a, w, t); }
    motion_now = guest_now += 20000000;   // 20 ms after the newest sample (1.17 rad/s at its time)
    SixAxisState six[1]; TestImuSample q[1];
    const float rev = (float)(1 / (2 * M_PI));   // the states carry revolutions a second
    CHECK(six_states(six, 1, 0x20003) == 1 && fabsf(six[0].gyro[1] - 1.17f * rev) < 1e-5f, "states path: the newest sample, no delay");
    CHECK(h_hid_ImuSamples(q, 1, 0x20003) == 1 && q[0].gyro[1] > -1.17f + 1e-3f, "IMU path: delayed (data from before the newest sample)");
    setenv("DM_SIX_DELAY", "30", 1); setenv("DM_IMU_DELAY", "0", 1); imu_env_read(); hwm_clear();
    // 30 ms late: 10 ms before the newest sample, two thirds of the way from 1.14 to 1.17
    CHECK(six_states(six, 1, 0x20003) == 1 && fabsf(six[0].gyro[1] - 1.16f * rev) < 1e-4f, "DM_SIX_DELAY=30: the states path 30 ms late");
    CHECK(h_hid_ImuSamples(q, 1, 0x20003) == 1 && fabsf(q[0].gyro[1] + 1.17f) < 1e-4f, "DM_IMU_DELAY=0: the IMU path at the present (the newest sample)");
    setenv("DM_SIX_DELAY", "x", 1); imu_env_read();
    CHECK(atomic_load(&g_live_delay_cfg[0]) == 0, "DM_SIX_DELAY not a number: none, with a log line");
    unsetenv("DM_SIX_DELAY"); setenv("DM_IMU_DELAY", "0", 1); unsetenv("DM_IMU_NOISE"); imu_env_read();
    dm_motion_clear_hid(0); h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}
// A frozen or DM_DETCLOCK guest clock maps every sample to one time: the answers carry the newest sample, not the oldest, whatever the delay
static void t_live_pinned(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear(); setenv("DM_IMU_NOISE", "0", 1); unsetenv("DM_IMU_DELAY"); imu_env_read();
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    uint64_t t0 = 790000000000ull; float a[3] = {0, 0, -1};
    for (int i = 0; i < 20; i++) { uint64_t t = t0 + (uint64_t)i * 30000000ull; motion_now = guest_now = t; float w[3] = {0, (float)i, 0}; dm_motion_set_hid(0, a, w, t); }
    TestImuSample q[16]; h_hid_ImuSamples(q, 16, 0x20003);
    CHECK(applied_delay(&g_hwm_imu, 0x20003) > 0, "pinned: the delay was in use before the freeze");
    g_test_pinned = 1; motion_now += 10000000;   // the host goes on, the guest clock stays
    int k = h_hid_ImuSamples(q, 16, 0x20003), newest = 1;
    for (int i = 0; i < k; i++) newest &= fabsf(q[i].gyro[1] + 19.f) < 1e-4f;
    CHECK(k > 0 && newest, "pinned: every tick carries the newest sample, not the oldest");
    CHECK(applied_delay(&g_hwm_imu, 0x20003) == 0, "pinned: no delay");
    setenv("DM_IMU_DELAY", "40", 1); imu_env_read();
    k = h_hid_ImuSamples(q, 16, 0x20003); newest = 1;
    for (int i = 0; i < k; i++) newest &= fabsf(q[i].gyro[1] + 19.f) < 1e-4f;
    CHECK(k > 0 && newest, "pinned with DM_IMU_DELAY=40: still the newest sample");
    g_test_pinned = 0;
    setenv("DM_IMU_DELAY", "0", 1); unsetenv("DM_IMU_NOISE"); imu_env_read();
    dm_motion_clear_hid(0); h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// Joy-Cons shown as a pair (Switch Sports Resort's styles: Handheld | JoyDual). A real pair is KIND_DUAL, each handle its own half; a pair that loses a half leaves the missing
// half's handle flat (no jump to the other controller); a pair in another slot of a one-player game (merged into player 1) also gives each handle its own half
static void t_pair_halves(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear(); setenv("DM_IMU_NOISE", "0", 1); imu_env_read();
    uint32_t ids_was = atomic_load(&g_ids_supported);
    uint64_t t = 600000000000ull; motion_now = guest_now = t;
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    h_hid_SetStyles(STYLE_HANDHELD | STYLE_JOYDUAL);
    float a[3] = {0, 0, -1}, wl[3] = {0, 0.3f, 0}, wr[3] = {0, 0.7f, 0};
    TestImuSample q[1];
    dm_joy_attach(0, 3); dm_motion_set_joy(0, 0, a, wl, t); dm_motion_set_joy(0, 1, a, wr, t);
    CHECK(npad_resolve(0).kind == KIND_DUAL, "a real L + R pair in a JoyDual game resolves as KIND_DUAL");
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1] + 0.3f) < 1e-4f, "pair: device 0 reads the left Joy-Con");
    CHECK(h_hid_ImuSamples(q, 1, 0x10005) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "pair: device 1 reads the right Joy-Con");
    dm_joy_attach(0, 2);   // the left one goes away
    CHECK(h_hid_ImuSamples(q, 1, 0x10005) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "lost half: the right handle keeps its own Joy-Con");
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1]) < 1e-4f && fabsf(q[0].acc[2] - 9.80665f) < 1e-3f, "lost half: the left handle lies flat, not the right Joy-Con's stream");
    dm_joy_attach(0, 0); dm_joy_attach(0, 2);   // a right Joy-Con alone from the start
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "single Joy-Con from the start: both handles read it again");
    dm_joy_attach(0, 0); dm_motion_clear_joy(0, 0); dm_motion_clear_joy(0, 1);
    // A one-player game with the pair on player 2: player 1 (no Joy-Cons) is shown as JoyDual, each handle reads its half
    ids((const uint32_t[]){0, NPAD_HANDHELD}, 2); dm_pad_players(0x3);
    dm_joy_attach(1, 3); dm_motion_set_joy(1, 0, a, wl, t); dm_motion_set_joy(1, 1, a, wr, t); dm_motion_set_hid(1, a, (float[3]){0.5f, 0, 0}, t);
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1] + 0.3f) < 1e-4f && fabsf(q[0].gyro[0]) < 1e-4f, "merged pair: device 0 reads player 2's left Joy-Con");
    CHECK(h_hid_ImuSamples(q, 1, 0x10005) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f, "merged pair: device 1 reads its right Joy-Con");
    dm_joy_attach(1, 2);   // the left one goes away there too
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1]) < 1e-4f && fabsf(q[0].gyro[0]) < 1e-4f, "merged pair that lost a half: the left handle lies flat");
    dm_joy_attach(1, 0); dm_joy_attach(1, 2);   // a single right Joy-Con in player 2's slot: both handles read it, not the sideways merged stream
    CHECK(h_hid_ImuSamples(q, 1, 0x5) == 1 && fabsf(q[0].gyro[1] + 0.7f) < 1e-4f && fabsf(q[0].gyro[0]) < 1e-4f, "merged single Joy-Con: its own stream (the upright frame), not the merged one");
    dm_joy_attach(1, 0); dm_motion_clear_joy(1, 0); dm_motion_clear_joy(1, 1); dm_motion_clear_hid(1);
    dm_pad_players(1); atomic_store(&g_ids_supported, ids_was);
    unsetenv("DM_IMU_NOISE"); imu_env_read();
    h_hid_SetStyles(0xffffffffu); h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// A still, clean live source (identical samples, as a quantized sensor at rest) at 33 and 67 Hz: the game never gets two identical ticks in a row, and the values stay within the dither
static void t_live_still_dither(void) {
    g_mode = 1; dm_pad_players(1); g_phone_mode = 0; hwm_clear(); unsetenv("DM_IMU_NOISE"); imu_env_read();
    uint8_t ring[64]; h_hid_ImuEnable(ring, sizeof ring);
    static const double rates[2] = {33, 67};
    for (int ri = 0; ri < 2; ri++) {
        uint64_t t0 = 500000000000ull + (uint64_t)ri * 10000000000ull, period = (uint64_t)(1e9 / rates[ri]), next = t0, nf = t0 + 50000000;
        dm_motion_clear_hid(0); hwm_clear();
        float a[3] = {0.01f, -0.11f, -0.99f}, w[3] = {0.001f, -0.002f, 0.0005f};
        int run_ok = 1, val_ok = 1; TestImuSample prev; int have = 0; int64_t last_num = 0;
        for (uint64_t t = t0; t < t0 + 1000000000ull; t += 500000) {
            motion_now = guest_now = t;
            if (t >= next) { dm_motion_set_hid(0, a, w, t - 8000000); next += period; }
            if (t >= nf) {
                nf += 16666667; TestImuSample q[16]; int k = h_hid_ImuSamples(q, 16, 0x20003);
                for (int i = 0; i < k; i++) {
                    if (q[i].sampling <= last_num) continue;   // a tick the fusion took already
                    if (have && !memcmp(q[i].acc, prev.acc, sizeof prev.acc) && !memcmp(q[i].gyro, prev.gyro, sizeof prev.gyro)) run_ok = 0;   // NOLINT(bugprone-suspicious-memory-comparison): two ticks with the same bits are what the fusion counts as a held sample
                    // the rest learner may take the gyro offset away (an HID source), so check each tick against the one before: the dither is all that moves
                    if (have && (fabsf(q[i].acc[2] - prev.acc[2]) > 1e-3f || fabsf(q[i].gyro[0] - prev.gyro[0]) > 1e-3f)) val_ok = 0;
                    prev = q[i]; have = 1; last_num = q[i].sampling;
                }
            }
        }
        char what[96];
        snprintf(what, sizeof what, "still clean source at %.0f Hz: never two identical ticks in a row", rates[ri]); CHECK(run_ok, what);
        snprintf(what, sizeof what, "still clean source at %.0f Hz: ticks move by no more than the dither", rates[ri]); CHECK(val_ok, what);
    }
    dm_motion_clear_hid(0); h_hid_ImuDisable(); guest_now = motion_now = 1000000000ull; g_phone_mode = -1;
}

// Six-axis sensor information the fusion's initialization reads (sdk+0x6d7e28): 0xc8 bytes (sdk+0x6d7e58 copies them), gyro half then acceleration half
static void t_six_ic_info(void) {
    uint8_t ic[0xd0]; memset(ic, 0xAB, sizeof ic); uint32_t h = 0;
    float f[0xc8 / 4];
    CHECK(h_hid_SixIcInfo(ic, &h) == 0, "ic information: success");
    memcpy(f, ic, sizeof f);
    CHECK(f[0] == 2000 && f[0x64 / 4] == 8, "ic information: gyro range 2000 deg/s, acceleration range 8 G");
    CHECK(f[0x04 / 4] == -10 && f[0x10 / 4] == 10 && f[0x68 / 4] == -0.0612f && f[0x74 / 4] == 0.0612f, "ic information: zero-offset bounds");
    CHECK(f[0x1c / 4] == 0.95f && f[0x20 / 4] == -0.003f && f[0x40 / 4] == 1.05f && f[0x44 / 4] == 0.003f && f[0x60 / 4] == 1.05f, "ic information: gyro sensitivity bounds");
    CHECK(f[0x80 / 4] == 0.95f && f[0xa4 / 4] == 1.05f && f[0xa8 / 4] == 0.003f && f[0xc4 / 4] == 1.05f, "ic information: acceleration sensitivity bounds");
    CHECK(ic[0xc8] == 0xAB, "ic information: nothing past 0xc8 written");
    CHECK(!h_hid_SixNewly(&h), "newly assigned: no");
    h_hid_SixNewlyReset(&h);
    // Calibration: nothing stored reads the ideal sensor's (all zero: zero bias, so the fusion counts its bias as calibrated); a stored parameter reads back for its device only
    uint8_t cal[0x744], back[0x744], zero[0x744] = {0}; memset(back, 0xAB, sizeof back);
    for (size_t i = 0; i < sizeof cal; i++) cal[i] = (uint8_t)i;
    CHECK(h_hid_SixCalLoad(back, &h) == 0 && !memcmp(back, zero, sizeof zero), "calibration: none stored, the ideal sensor's (zero bias), all 0x744 bytes");
    CHECK(h_hid_SixCalStore(&h, cal) == 0, "calibration: stored");
    CHECK(h_hid_SixCalLoad(back, &h) == 0 && !memcmp(back, cal, sizeof cal), "calibration: reads back");
    uint32_t other = 1u << 16; memset(back, 0xAB, sizeof back);
    CHECK(h_hid_SixCalLoad(back, &other) == 0 && !memcmp(back, zero, sizeof zero), "calibration: another device has its own (none stored: zero bias)");
    memset(back, 0xAB, sizeof back);
    {   // one sensor, one parameter: the host's full-key handle (device 0) and the SDK's (device 2) store and load the same
        uint32_t host0 = 0x3, sdk2 = 0x20003; uint8_t cal2[0x744]; memset(cal2, 0x5A, sizeof cal2);
        CHECK(h_hid_SixCalStore(&sdk2, cal2) == 0 && h_hid_SixCalLoad(back, &host0) == 0 && !memcmp(back, cal2, sizeof cal2), "calibration: stored through device 2, loaded through device 0");
        CHECK(h_hid_SixCalStore(&host0, cal) == 0 && h_hid_SixCalLoad(back, &sdk2) == 0 && !memcmp(back, cal, sizeof cal), "calibration: and the other way");
        memset(back, 0xAB, sizeof back);
    }
    uint32_t bad = 9u << 8, dev3 = 3u << 16;   // as the SDK's VerifyValidSixAxisSensorHandle (sdk+0x6d8440)
    CHECK(h_hid_SixCalStore(&bad, cal) == 0x58aca && h_hid_SixCalLoad(back, &bad) == 0x58aca, "calibration: an id that is no npad is refused");
    CHECK(h_hid_SixCalStore(&dev3, cal) == 0xd6ca && back[0] == 0xAB, "calibration: a device index past 2 is refused");
}

// Joy-Con icon decoration colour (Switch 2): no controller of the host is a decorated Joy-Con, so each npad answers 0x4b6ca, which the SDK's own wrapper returns to the game, and the
// colours are not written; an id that is no npad is refused
static void t_icon_decoration(void) {
    uint8_t a[12], b[12]; memset(a, 0xAB, sizeof a); memset(b, 0xAB, sizeof b); const uint32_t no1 = 0, handheld = 0x20, bad = 9;
    CHECK(h_hid_IconColor(a, &no1) == 0x4b6ca && h_hid_IconColor(a, &handheld) == 0x4b6ca && a[0] == 0xAB, "icon decoration: none for a host controller, colour not written");
    CHECK(h_hid_IconColors(a, b, &no1) == 0x4b6ca && a[11] == 0xAB && b[0] == 0xAB, "icon decoration: none for either half");
    CHECK(h_hid_IconColor(a, &bad) != 0x4b6ca && h_hid_IconColor(a, &bad) != 0 && a[0] == 0xAB, "icon decoration: an id that is no npad is refused");
}

// A game that supports no full-key style (Nintendo Switch Sports Resort's lobby: Handheld | JoyDual, 0x6): a controller that is not a Joy-Con pair (the keyboard, a Pro-like pad)
// is shown on No1..No8 as a dual Joy-Con pair, the supported style that has its buttons and both sticks, so GetNpadStyleSet only answers supported styles; the states, six-axis
// sensors and IMU samples, vibration handles and the controller applet's answer follow, and a style change signals the npad's style event. The Handheld npad and a game that
// supports full-key (or no style with a whole controller's buttons): unchanged
static void t_unsupported_style(void) {
    static uint8_t ev0[0x40];
    g_mode = 1; ids((uint32_t[]){0, 1, 2, 3, NPAD_HANDHELD}, 5); dm_pad_players(1);
    h_hid_SetStyles(0xffffffffu);
    g_signals = 0; h_hid_SetStyles(0x1f);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY && g_signals == 0, "full-key supported (0xffffffff, then 0x1f): player 1 is full-key, as before, and no style event");
    h_hid_BindStyleEv(&(uint32_t){0}, ev0, 0);
    g_signals = 0; h_hid_SetStyles(STYLE_HANDHELD | STYLE_JOYDUAL);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYDUAL, "full-key not supported: player 1's controller is a JoyDual npad on No1");
    CHECK(g_signals == 1 && g_signalled == ev0, "No1's style event signalled when its style changes with the supported styles");
    dm_input_set(DM_BTN_A, -12000, 0, 0, 9000);
    NpadState st = npad_of(0, STYLE_JOYDUAL);
    CHECK((st.buttons & DM_BTN_A) && st.lx == -12000 && st.ry == 9000 && st.attributes == (ATTR_CONNECTED | ATTR_LEFT_CONN | ATTR_RIGHT_CONN), "its JoyDual state: the keyboard's buttons and both sticks, both halves connected");
    CHECK(!npad_of(0, STYLE_FULLKEY).attributes, "no full-key state for it");
    dm_input_set(0, 0, 0, 0, 0); npad_of(0, STYLE_JOYDUAL);

    // Six-axis sensors and the npad IMU: a sensor per half, each with samples
    uint32_t h[2], id = 0;
    CHECK(h_hid_SixHandles(h, 2, &id, STYLE_JOYDUAL) == 2 && six_connected(h[0]) && six_connected(h[1]), "two six-axis sensors, connected");
    { uint64_t was = guest_now; guest_now = 2000000000ull; uint8_t ring[64]; TestImuSample s[4];
      h_hid_ImuEnable(ring, sizeof ring);
      CHECK(h_hid_ImuSamples(s, 4, 0x5) == 4 && imu_numbered(s, 4, 398) && h_hid_ImuSamples(s, 4, 0x10005) == 4 && imu_numbered(s, 4, 398), "IMU samples for the SDK's pair handles (style index 5), device index 0 and 1");
      h_hid_ImuDisable(); guest_now = was; }

    // Vibration: type 5 handles, routed to player 1's motors, as are the full-key handles it got before
    dm_haptics_init(vibration_sink, 1); dm_haptics_route(0, 1);
    CHECK(h_hid_VibHandles(h, 2, &id, STYLE_JOYDUAL) == 2 && (h[0] & 0xff) == 5 && h[1] == (h[0] | 1u << 16), "JoyDual vibration handles: type 5, two motors");
    { int m; CHECK(vib_targets(h[1], &m) == 1 && m == 1 && vib_targets(0x0003, &m) == 1 && m == 0, "they reach player 1's motors, and so do the full-key handles it got before"); }
    dm_haptics_init(NULL, 0);

    int32_t info[2]; int8_t arg[8] = {1, 1};
    CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 1 && (h_hid_GetStyle(&(uint32_t){(uint32_t)info[1]}) & h_hid_GetStyles()), "the controller applet selects an npad whose style the game supports");
    dm_pad_players(3);
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYDUAL, "player 2's controller: JoyDual on No2 too");
    g_mode = 0;
    CHECK(h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == STYLE_HANDHELD && h_hid_GetStyle(&(uint32_t){1}) == STYLE_JOYDUAL, "handheld mode: the Handheld npad stays Handheld, player 2 is JoyDual on No2");
    g_mode = 1; dm_pad_players(1);

    // Back to a set with full-key: full-key again, the event signalled again
    g_signals = 0; h_hid_SetStyles(0x1f);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY && g_signals == 1 && g_signalled == ev0, "0x6 back to 0x1f: full-key again, No1's style event signalled again");
    // Other sets: Handheld only, and the Joy-Con styles without full-key
    h_hid_SetStyles(STYLE_HANDHELD);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY, "Handheld only: docked No1 keeps full-key (no supported style has a whole controller's buttons on No1)");
    g_mode = 0; CHECK(h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == STYLE_HANDHELD, "Handheld only, handheld mode: the Handheld npad"); g_mode = 1;
    h_hid_SetStyles(STYLE_JOYDUAL | STYLE_JOYLEFT | STYLE_JOYRIGHT);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_JOYDUAL, "JoyDual | JoyLeft | JoyRight: JoyDual");
    h_hid_SetStyles(STYLE_JOYLEFT | STYLE_JOYRIGHT);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY, "JoyLeft | JoyRight only: no style with a whole controller's buttons supported, full-key as before");
    h_hid_SetStyles(0xffffffffu); pthread_mutex_lock(&g_style_lock); g_style_ev[0] = NULL; pthread_mutex_unlock(&g_style_lock);
}

int main(void) {
    CHECK(setenv("DM_RUMBLE_LOG", "1", 1) == 0, "enable rumble diagnostics");
    setenv("DM_IMU_DELAY", "0", 1);   // the tests of the live history read it at the present; t_live_delay checks the playout delay
    // The plumbing tests (streams, interpolation, dither, pairs) were written with the samples negated: they keep that (DM_IMU_*_SIGN=-1); t_npad_imu_samples and
    // t_imu_conversion check the host's own signs
    setenv("DM_IMU_ACC_SIGN", "-1", 1); setenv("DM_IMU_GYRO_SIGN", "-1", 1);
    t_npad_imu();
    t_icon_decoration();
    t_slots();
    t_npads();
    t_script();
    t_vibration();
    t_opmode();
    t_joycons();
    t_unsupported_style();
    t_jcms();
    t_rumble_log();
    t_motion_threads();
    t_motion_live();
    t_motion_hid();
    t_npad_imu_samples();
    t_imu_noise();
    t_imu_sdk_handles();
    t_live_ring();
    t_live_edges();
    t_live_rest();
    t_single_joy_as_pair();
    t_live_still_dither();
    t_pair_halves();
    t_live_delay();
    t_live_delay_paths();
    t_live_delay_swap();
    t_imu_conversion();
    t_live_pinned();
    t_pair_half_pause();
    t_six_ic_info();
    t_touch_sampling();
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
    {   // DM_PAD_PRESS_TO_JOIN=1: the lowest free slot at the first press, not the slot a controller of that name held before
        pad_slots q = {0}; static char x, y, z;   // distinct addresses
        CHECK(pad_slot_take_lowest(&q, &x, "Pro Controller") == 0, "press: the first to press is player 1");
        CHECK(pad_slot_take_lowest(&q, &y, "Pro Controller") == 1, "press: the next is player 2, even of the same model");
        CHECK(pad_slot_take_lowest(&q, &x, "Pro Controller") == 0, "press: pressing again keeps the slot");
        pad_slot_free(&q, &x);
        CHECK(pad_slot_take_lowest(&q, &z, "Xbox Wireless Controller") == 0, "press: a freed player 1 goes to whoever presses next, whatever its name");
        pad_slots f = {0}; static char o[DM_PLAYERS + 1];
        for (int i = 0; i < DM_PLAYERS; i++) pad_slot_take_lowest(&f, &o[i], "pad");
        CHECK(pad_slot_take_lowest(&f, &o[DM_PLAYERS], "pad") == -1, "press: none left when every player is held");
        CHECK(pad_slot_take_lowest(&f, NULL, "pad") == -1, "press: no owner, no slot");
    }
    printf("hid players: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
