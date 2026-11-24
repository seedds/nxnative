// Virtual Switch 2 Nmouse (dm_host_mouse in hle_hid.c): relative delta accumulation, button mapping,
// 2.5 s idle lift-off, physical Joy-Con 2 priority preemption and detach recovery, and DM_VIRTUAL_NMOUSE=0 disable.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../hle_hid.c"

static int checks, fails, g_mode = 1;
static uint64_t g_test_now = 1000000000ull;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int dm_opmode(void) { return g_mode; }
uint64_t guest_ns(void) { return g_test_now; }
uint64_t now_ns(void) { return g_test_now; }
uint64_t guest_ns_at_host_time(uint64_t host_ns) { return host_ns; }
int guest_clock_pinned(void) { return 0; }
uint64_t dm_guest_frame(void) { return 0; }
void dm_oe_execute(int index, const void *param, size_t n) { (void)index; (void)param; (void)n; }
void front_web_shot(const char *path) { (void)path; }
int front_ctrl_support(int min, int max) { (void)min; (void)max; return -1; }
void (*_Atomic dm_touch_observer)(const dm_touch *t, int n);
void dm_touch_set(const dm_touch *t, int n) { (void)t; (void)n; }
void dm_touch_script(const dm_touch *t, int n) { (void)t; (void)n; }
int touch_fill_n(void *out, int max, uint64_t now, int points) { (void)out; (void)max; (void)now; (void)points; return 0; }
int gesture_fill(void *out, int n, uint64_t now) { (void)out; (void)n; (void)now; return 0; }
void dm_gfx_shot_at(long frame, const char *path) { (void)frame; (void)path; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
int hle_signal_bound_system_event(void *g) { (void)g; return 1; }
void dm_gfx_texlist(int target) { (void)target; }
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
void dm_gfx_addr_note(uint64_t a, char *note, size_t n) { (void)a; if (n) note[0] = 0; }
void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path) { (void)key; (void)a; (void)n; (void)path; }
void probe_atrace(uint64_t lo, uint64_t n) { (void)lo; (void)n; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}

static void step_ms(uint64_t ms) { g_test_now += ms * 1000000ull; }

int main(void) {
    unsetenv("DM_VIRTUAL_NMOUSE");
    uint32_t ids[2] = {0, NPAD_HANDHELD};
    h_hid_SetIds(ids, 2);
    dm_pad_players(1);

    // 1. Before any host mouse event: no handles, no standard mouse, no merged state
    uint64_t nh[4] = {0}, std_h = 7;
    NmouseState st = {0};
    int rw = 0, rh = 0;
    CHECK(!dm_joy_mouse_wanted(), "nmouse not wanted before init");
    h_hid_NmouseInit(NULL, 0, 1920, 1080);
    h_hid_NmouseGetRange(&rw, &rh);
    CHECK(dm_joy_mouse_wanted() && rw == 1920 && rh == 1080, "4-arg InitializeNmouse enables wanted and records range");
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 0, "no virtual handle before first host mouse event");
    CHECK(!h_hid_NmouseStd(&std_h) && std_h == 7, "no standard mouse handle before first event");
    CHECK(!h_hid_NmouseState(&st, 0), "no merged state before first event");

    // 2. Absolute placement + relative delta / wheel accumulation across multiple host events
    dm_host_mouse(0.5f, 0.5f, 0.f, 0.f, 0, 0, 0, DM_HOST_MOUSE_FLAG_ABS | DM_HOST_MOUSE_FLAG_SURFACE);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && nh[0] == NMOUSE_HOST_HANDLE, "virtual handle exposed for player 1");
    CHECK(h_hid_NmouseStd(&std_h) && std_h == NMOUSE_HOST_HANDLE, "standard mouse handle exposes virtual handle");
    CHECK(h_hid_NmouseEnable(NMOUSE_HOST_HANDLE, 1), "EnableNmousePointer accepts active virtual handle");
    uint32_t types = 0;
    CHECK(h_hid_NmouseTypes(&types, NMOUSE_HOST_HANDLE) == 1 && types == 4u, "GetNmouseTypeSet reports standard mouse type");

    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE), "initial state readable");
    CHECK(st.x == 960 && st.y == 540 && st.dx == 0 && st.dy == 0 && st.attributes == 3u, "first abs placement has zero jump delta and is on surface");

    // Accumulate two relative moves + wheel before the game reads
    dm_host_mouse(0.f, 0.f, 12.f, -5.f, 1, 2, DM_HOST_MOUSE_LEFT, DM_HOST_MOUSE_FLAG_REL | DM_HOST_MOUSE_FLAG_SURFACE);
    dm_host_mouse(0.f, 0.f, 8.f, 3.f, -1, 4, DM_HOST_MOUSE_LEFT | DM_HOST_MOUSE_RIGHT, DM_HOST_MOUSE_FLAG_REL | DM_HOST_MOUSE_FLAG_SURFACE);
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE), "accumulated state readable");
    CHECK(st.x == 980 && st.y == 538 && st.dx == 20 && st.dy == -2, "relative deltas accumulated across events");
    CHECK(st.wheel_x == 0 && st.wheel_y == 6, "wheel deltas accumulated across events");
    CHECK(st.buttons == (DM_HOST_MOUSE_LEFT | DM_HOST_MOUSE_RIGHT) && st.attributes == 3u, "left and right buttons mapped");

    // Second read with no new events drains deltas while preserving position and buttons
    CHECK(h_hid_NmouseState(&st, 0), "merged handle 0 reads active virtual pointer");
    CHECK(st.x == 980 && st.y == 538 && st.dx == 0 && st.dy == 0 && st.wheel_y == 0, "deltas cleared after read");
    CHECK(st.buttons == (DM_HOST_MOUSE_LEFT | DM_HOST_MOUSE_RIGHT), "buttons preserved");

    // Middle button and ring history via GetNmouseStates (newest first)
    dm_host_mouse(0.f, 0.f, -4.f, 6.f, 0, 0, DM_HOST_MOUSE_MIDDLE, DM_HOST_MOUSE_FLAG_REL | DM_HOST_MOUSE_FLAG_SURFACE);
    NmouseState hist[4] = {0}; int cnt = 0;
    CHECK(h_hid_NmouseStates(&cnt, hist, 4, 0) == 1 && cnt == 4, "GetNmouseStates returns 4 newest samples");
    CHECK(hist[0].sampling == 4 && hist[0].dx == -4 && hist[0].dy == 6 && hist[0].buttons == DM_HOST_MOUSE_MIDDLE, "newest ring sample matches");
    CHECK(hist[1].sampling == 3 && hist[1].dx == 8 && hist[1].dy == 3, "second newest ring sample matches");

    // SetNmousePointerPosition updates virtual position clamped to range
    CHECK(h_hid_NmousePos(NMOUSE_HOST_HANDLE, 3000, -10), "SetNmousePointerPosition accepted");
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE) && st.x == 1919 && st.y == 0, "SetNmousePointerPosition clamps to pointer range");

    // 3. 2.5 s idle automatic lift-off (held button prevents lift-off; releasing starts 2.5 s idle timer)
    step_ms(3000);
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE) && (st.attributes & 3u) == 3u, "held button keeps pointer on surface past 2.5 s");
    // Release buttons while still on surface
    dm_host_mouse(0.f, 0.f, 0.f, 0.f, 0, 0, 0, DM_HOST_MOUSE_FLAG_REL | DM_HOST_MOUSE_FLAG_SURFACE);
    step_ms(2499);
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE) && (st.attributes & 3u) == 3u, "still on surface at 2499 ms idle");
    step_ms(1);
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE) && st.attributes == 2u, "auto lift-off at 2500 ms clears on-surface bit and keeps connected");
    CHECK(h_hid_NmouseStates(&cnt, hist, 2, NMOUSE_HOST_HANDLE) == 1 && cnt == 2 && hist[0].attributes == 2u, "GetNmouseStates also reflects auto lift-off");

    // Next movement immediately puts pointer back on surface
    dm_host_mouse(0.f, 0.f, 3.f, 0.f, 0, 0, 0, DM_HOST_MOUSE_FLAG_REL | DM_HOST_MOUSE_FLAG_SURFACE);
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE) && (st.attributes & 3u) == 3u && st.dx == 3, "movement restores on-surface state");

    // RESET flag clears buttons and lifts off immediately
    dm_host_mouse(0.f, 0.f, 0.f, 0.f, 0, 0, DM_HOST_MOUSE_LEFT, DM_HOST_MOUSE_FLAG_REL | DM_HOST_MOUSE_FLAG_SURFACE);
    dm_host_mouse(0.f, 0.f, 0.f, 0.f, 0, 0, 0, DM_HOST_MOUSE_FLAG_RESET);
    CHECK(h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE) && st.buttons == 0 && st.attributes == 2u, "RESET clears buttons and lifts off");

    // 4. Physical Joy-Con 2 priority preemption (must never fight physical JC2 hardware)
    dm_joy_attach(0, 3 | DM_JOY_2);
    // Releasing an unreported side must not fabricate a JCMS sample
    dm_joy_mouse(0, 0, 0, 0, 0);
    JcmsState jst[2] = {0}; uint32_t jhl = h_hid_JcmsHandle(&(uint32_t){0}, &(uint32_t){0});
    CHECK(h_hid_JcmsStates(jst, 2, &jhl) == 0, "state=0 release before first sample does not create a JCMS sample");

    dm_joy_mouse(0, 1, 10, -4, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && nh[0] != NMOUSE_HOST_HANDLE && (nh[0] & 0xffffu) == 0x0001u, "physical JC2 preempts virtual handle");
    std_h = 9;
    CHECK(!h_hid_NmouseStd(&std_h) && std_h == 9, "standard mouse handle yields while physical JC2 is connected");
    CHECK(!h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE), "virtual handle inactive while physical JC2 is connected");
    CHECK(h_hid_NmouseState(&st, nh[0]) && (st.attributes & 3u) == 3u, "physical JC2 handle reads physical sensor state");
    CHECK(h_hid_NmouseStates(&cnt, hist, 2, 0) == 1 && cnt == 1 && hist[0].sampling == 1, "merged handle 0 serves physical JC2 while connected");

    // Releasing physical JC2 via dm_joy_mouse(..., 0) (s2_joy_release path) restores virtual pointer
    dm_joy_mouse(0, 1, 0, 0, 0);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && nh[0] == NMOUSE_HOST_HANDLE, "dm_joy_mouse release clears disconnected JC2 and restores virtual handle");
    CHECK(h_hid_NmouseStd(&std_h) && std_h == NMOUSE_HOST_HANDLE, "virtual standard mouse resumes after dm_joy_mouse release");

    // Disconnect physical JC2 via dm_joy_attach(0, 0) ONLY (no manual dm_joy_mouse(..., 0)):
    // mirrors pad_switch2_apple.m teardown / leavePair / joinPadOf detach paths and must restore virtual pointer
    dm_joy_mouse(0, 1, 4, 2, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && (nh[0] & 0xffffu) == 0x0001u, "reconnected physical JC2 preempts virtual handle again");
    dm_joy_attach(0, 0);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && nh[0] == NMOUSE_HOST_HANDLE, "dm_joy_attach(0, 0) clears dead physical JC2 handle and restores virtual handle");
    CHECK(h_hid_NmouseStd(&std_h) && std_h == NMOUSE_HOST_HANDLE, "virtual standard mouse resumes after dm_joy_attach(0, 0)");

    // Dual physical JC2: detaching right side only keeps left side active; detaching both restores virtual pointer
    dm_joy_attach(0, 3 | DM_JOY_2);
    dm_joy_mouse(0, 0, 2, 1, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE);
    dm_joy_mouse(0, 1, 5, -2, DM_JOYMOUSE_CONNECTED | DM_JOYMOUSE_ON_SURFACE);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 2 && (nh[0] & 1) == 1, "dual JC2 reports right first as last on surface");
    dm_joy_attach(0, 1 | DM_JOY_2);
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 1 && (nh[0] & 0xffffu) == 0x0000u, "detaching right JC2 keeps left JC2 handle only");
    CHECK(!h_hid_NmouseStd(&std_h), "virtual pointer still yields while left JC2 remains connected");
    dm_joy_attach(0, 0);
    dm_host_mouse(0.25f, 0.25f, 0.f, 0.f, 0, 0, 0, DM_HOST_MOUSE_FLAG_ABS | DM_HOST_MOUSE_FLAG_SURFACE);
    CHECK(h_hid_NmouseStd(&std_h) && std_h == NMOUSE_HOST_HANDLE, "virtual pointer resumes after all physical JC2 detach");

    // 5. Negative check: DM_VIRTUAL_NMOUSE=0 disables virtual Nmouse completely
    setenv("DM_VIRTUAL_NMOUSE", "0", 1);
    std_h = 11;
    CHECK(!h_hid_NmouseStd(&std_h) && std_h == 11, "DM_VIRTUAL_NMOUSE=0 hides standard mouse handle");
    CHECK(!h_hid_NmouseState(&st, NMOUSE_HOST_HANDLE), "DM_VIRTUAL_NMOUSE=0 rejects virtual state reads");
    CHECK(h_hid_NmouseHandles(nh, 4, 0) == 0, "DM_VIRTUAL_NMOUSE=0 hides virtual handle from GetNmouseHandles");
    unsetenv("DM_VIRTUAL_NMOUSE");
    CHECK(h_hid_NmouseStd(&std_h) && std_h == NMOUSE_HOST_HANDLE, "re-enabling DM_VIRTUAL_NMOUSE restores virtual handle");

    // 6. FinalizeNmouse clears wanted flag
    h_hid_NmouseFini();
    CHECK(!dm_joy_mouse_wanted(), "FinalizeNmouse clears wanted flag");

    printf("t_hid_nmouse: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
