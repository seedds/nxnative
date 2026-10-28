// Linux and Windows front end (front.h) on SDL3, the counterpart of front_mac.m, whose behaviour it follows (the reasons are in the comments there). With DM_WINDOW=1 a window (made with
// SDL_WINDOW_VULKAN for the Vulkan presenter of Phase 4) and keyboard, mouse and controllers -> dm_input; the guest runs on a 64 MiB-stack thread and the calling (main) thread runs SDL's
// event loop. Without DM_WINDOW the guest runs directly on the calling thread and SDL's video never starts (windowless tests).
// The window is 1280x720 and resizable; F11 or Alt+Enter toggles fullscreen (borderless, at the desktop's mode), DM_FULLSCREEN=1 starts in it; closing it quits; Escape does nothing.
// Key map (front_mac.m's, by key position: SDL scancodes are positions, as the Mac's virtual key codes are): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR,
// F=press left stick G=press right stick, Return=+ Tab=−, Space=shake the controller. The left mouse button is the touchscreen.
// Controllers (SDL_Gamepad): face buttons by position as on the Mac and the iPhone (right A, bottom B, top X, left Y: on a Nintendo-layout pad these are its printed labels, SDL's
// south / east / west / north being positions), shoulders L R, triggers ZL ZR past half travel, Start + Back −, both sticks and their presses; the guide button is not used.
// Players as front_mac.m (pad_slots.h; DM_PAD_MERGE=1, DM_KBD_PLAYER=own). Gyro and accelerometer (DM_MOTION=0 leaves them off), rumble (DM_RUMBLE=0 off, DM_RUMBLE_GAIN and
// DM_RUMBLE_MIN as in haptics_apple.m), the screen's refresh rate matched to the game's frame rate in fullscreen (DM_SCREEN_MATCH=0 keeps it).
// Threads: every SDL window, event and renderer call is made on the main thread (the one that started SDL's video). The front_* calls the host makes from its other threads post their
// work there (SDL_RunOnMainThread), as front_mac.m does with dispatch_async, and the ones that must wait for it (the text box) block their caller, never the main thread
#include "hle.h"
#include "dm_input.h"
#include "front.h"
#include "hle_haptics.h"
#include "pad_slots.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>

static SDL_Window *g_win;      // the game's window: made on the main thread before the guest starts, NULL without one
static int g_vk_window;        // it was made with SDL_WINDOW_VULKAN
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;
static int g_input_on = 1;     // main thread: the window has the keyboard, so its keyboard, mouse and controllers reach the game (input_release / input_resume)
static int g_has_focus = 1, g_minimized;   // main thread: the window's state as its events last told
static void SDLCALL wake_cb(void *flag) { atomic_store((_Atomic int *)flag, 0); }   // the main thread is awake: its after-events step (after_events) does the posted work

void *front_metal_layer(void) { return NULL; }   // no Metal here: the Vulkan presenter takes the window through front_vk_create_surface

// ---------- Screen refresh rate (front.h; front_mac.m screen_match_apply has the reasons) ----------
// front_screen_max_fps is the rate the window's screen runs at now (its current mode; in fullscreen with a matched mode, that mode), as the Mac's maximumFramesPerSecond tracks it:
// the highest rate of all the screen's modes would report a 144 Hz monitor set to 60 Hz as 144, which the window never gets
static _Atomic int g_screen_fps;
static _Atomic int g_match_want;   // the frame rate to match (0 = not known yet), written by the present thread only
static int g_match_on;             // main thread: this front end gave the fullscreen window a mode of its own (SDL_SetWindowFullscreenMode)
static float mode_hz(const SDL_DisplayMode *m) { return m && m->refresh_rate > 0 ? m->refresh_rate : 0; }   // 0 = not known (SDL's dummy driver reports none)
static int hz_multiple(double hz, int fps) { double k = round(hz / fps); return k >= 1 && fabs(hz / fps - k) < 0.01; }   // 59.94 Hz counts as 60
static void screen_fps_update(void) {
    SDL_DisplayID d = g_win ? SDL_GetDisplayForWindow(g_win) : 0;
    g_screen_fps = (int)lroundf(mode_hz(d ? SDL_GetCurrentDisplayMode(d) : NULL));
}
int front_screen_max_fps(void) { return g_screen_fps; }
// Main thread: on a new frame rate, on entering and leaving fullscreen and on a move to another screen. In fullscreen only, a screen whose rate is not a multiple of the game's gets the
// mode of the same size with the highest rate that is; SDL switches the screen back to its desktop mode when the window leaves fullscreen. A window keeps the screen's rate (switching
// changes the whole screen for every program). X11 keeps a mode a program set after the program ends, so front_atexit gives the desktop mode back (a crash cannot); Windows restores it
// when the program ends; Wayland lets no program change the screen's mode, so there the rate stays the desktop's
static void screen_match_apply(void) {
    static int off = -1; if (off < 0) { const char *e = getenv("DM_SCREEN_MATCH"); off = e && !strcmp(e, "0"); if (off) LOG("front end: the screen keeps its refresh rate in fullscreen too (DM_SCREEN_MATCH=0)"); }
    if (!g_win) return;
    int full = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0, want = g_match_want;
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_win);
    if (g_match_on && !full) {   // left fullscreen: the next fullscreen starts at the desktop mode again
        SDL_SetWindowFullscreenMode(g_win, NULL); g_match_on = 0;
        LOG("front end: screen back at %.0f Hz", mode_hz(d ? SDL_GetDesktopDisplayMode(d) : NULL));
    }
    screen_fps_update();
    if (off || !full || want <= 0 || !d) return;
    const SDL_DisplayMode *cur = SDL_GetCurrentDisplayMode(d); float hz = mode_hz(cur);
    if (hz <= 0 || hz_multiple(hz, want)) return;   // already a multiple, or not known
    int n = 0; SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &n);
    const SDL_DisplayMode *pick = NULL;
    for (int i = 0; modes && i < n; i++) {
        const SDL_DisplayMode *m = modes[i];
        if (m->w != cur->w || m->h != cur->h || m->pixel_density != cur->pixel_density) continue;
        if (hz_multiple(m->refresh_rate, want) && (!pick || m->refresh_rate > pick->refresh_rate)) pick = m;
    }
    if (!pick) LOG("front end: screen at %.0f Hz has no mode of this size at a multiple of %d Hz: frames are paced on the nearest refreshes", hz, want);
    else {
        int ok = SDL_SetWindowFullscreenMode(g_win, pick);
        LOG("front end: screen at %.0f Hz %s %.0f Hz for %d fps in fullscreen (DM_SCREEN_MATCH=0 keeps it)", hz, ok ? "switched to" : "could not be switched to", pick->refresh_rate, want);
        if (ok) { g_match_on = 1; g_screen_fps = (int)lroundf(pick->refresh_rate); }
    }
    SDL_free(modes);
}
static void SDLCALL screen_match_cb(void *u) { (void)u; screen_match_apply(); }
void front_screen_match(int fps) {   // the present thread
    if (!g_win || g_match_want == fps) return;   // no window (a windowless run's main thread runs the guest), or the rate as before
    g_match_want = fps; SDL_RunOnMainThread(screen_match_cb, NULL, false);
}

// ---------- Keyboard ----------
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, 0);
}
static int key_bit(SDL_Scancode c) {
    switch (c) {
        case SDL_SCANCODE_J: return 0;  case SDL_SCANCODE_K: return 1;  case SDL_SCANCODE_U: return 2;  case SDL_SCANCODE_I: return 3;    // J K U I → A B X Y
        case SDL_SCANCODE_F: return 4;  case SDL_SCANCODE_G: return 5;                                                                  // F G → press stick
        case SDL_SCANCODE_Q: return 6;  case SDL_SCANCODE_E: return 7;  case SDL_SCANCODE_Z: return 8;  case SDL_SCANCODE_C: return 9;    // Q E Z C → L R ZL ZR
        case SDL_SCANCODE_RETURN: return 10; case SDL_SCANCODE_TAB: return 11;                                                          // Return Tab → + −
        case SDL_SCANCODE_LEFT: return 12; case SDL_SCANCODE_UP: return 13; case SDL_SCANCODE_RIGHT: return 14; case SDL_SCANCODE_DOWN: return 15;   // ← ↑ → ↓
        default: return -1;
    }
}
static void on_key(SDL_Scancode c, int down) {
    if (!g_input_on) return;
    switch (c) { case SDL_SCANCODE_W: g_w = down; break; case SDL_SCANCODE_A: g_a = down; break; case SDL_SCANCODE_S: g_s = down; break; case SDL_SCANCODE_D: g_d = down; break; default: break; }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == SDL_SCANCODE_SPACE ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
    push();
}

// ---------- Mouse as a single-point touchscreen (front.h dm_touch_set): left button down / drag = finger 0, release = lifted ----------
// Normalized on the centered 16:9 picture, as front_mac.m: its present fits the game's picture into the view with black bars. The Vulkan presenter (Phase 4) must fit it the same way,
// or change this with it; until then nothing is drawn there and the picture area is the window's content, letterboxed to 16:9
static int g_mouse_down;
static void mouse_touch(float x, float y) {
    if (!g_input_on || !g_win) return;
    int ww = 0, wh = 0; SDL_GetWindowSize(g_win, &ww, &wh);
    double w = ww, h = wh;
    if (w <= 0 || h <= 0) return;
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;
    double x0 = (ww - w) / 2, y0 = (wh - h) / 2;
    dm_touch t = {0, (float)fmin(fmax((x - x0) / w, 0), 0.999), (float)fmin(fmax((y - y0) / h, 0), 0.999)};   // SDL's window origin is at the top left, as the touchscreen's
    dm_touch_set(&t, 1); g_mouse_down = 1;
}

// ---------- Controllers ----------
// Each SDL gamepad holds a slot of its own (pad_slots.h, keyed by its entry here; player 1 first, a reconnected controller gets its player back by name), and hle_hid.c reports player n on
// its own npad while the slot is held (dm_pad_players). With DM_PAD_MERGE=1 every controller is player 1 (the last one moved wins). A controller that found every player taken gets the
// first one freed. Each event marks its pad, and after the events of one pass each marked pad is read whole and published once (dm_pad_set_player), as GameController's value handler does
typedef struct {
    SDL_Gamepad *pad; SDL_JoystickID id;
    int player;                 // slot, -1 = none (every player taken); unused with DM_PAD_MERGE
    int dirty;                  // an event since it was last read
    int rumble;                 // it has rumble motors
    int motion, live, have_acc; // its gyro and accelerometer are on; a nonzero sample came (motion_apple.h); acc holds an accelerometer sample
    float acc[3];               // the latest acceleration in dm_motion_set's frame
    char name[64];
} gpad;
enum { MAX_PADS = 16, TRIGGER_HALF = 16384 };   // SDL's trigger axes run 0 .. 32767
static gpad g_pads[MAX_PADS];
static pad_slots g_slots;
static gpad *g_held[DM_PLAYERS];   // the pad of each slot
static int g_merge, g_kbd_own;
static gpad *pad_find(SDL_JoystickID id) { for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].pad && g_pads[i].id == id) return &g_pads[i]; return NULL; }
static int any_pad(void) { for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].pad) return 1; return 0; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }

// Rumble (hle_haptics.h). The sink runs on the game's thread under the haptics state lock, so it only keeps each player's newest state and wakes the main thread, which drives SDL.
// SDL_RumbleGamepad takes one 16-bit intensity per motor (the low-frequency strong one and the high-frequency weak one) and a duration after which SDL stops them. The game's two
// motors map onto them as haptics_apple.m drives a DualShock 4 (ds4_apply, the way emulators feed SDL): the strong motor follows the low band, mostly from the left motor, the weak motor
// the high band, mostly from the right one; DM_RUMBLE_GAIN and DM_RUMBLE_MIN apply as there. What SDL cannot express: the frequencies of the two bands (the Switch's HD rumble plays each
// at its own; SDL's motors take only an intensity) and Core Haptics' intensity / sharpness envelope (dm_haptics_level). A nonzero state is sent again every RUMBLE_RESEND_MS with
// RUMBLE_MS of duration, so the motors never stop between two sends while the game holds it; a zero state stops them at once
enum { RUMBLE_RESEND_MS = 100, RUMBLE_MS = 150 };
static pthread_mutex_t g_rumble_mu = PTHREAD_MUTEX_INITIALIZER;
static dm_haptic_state g_rumble_in[DM_PLAYERS]; static uint32_t g_rumble_new;   // under g_rumble_mu: the newest state of each player, and which are new
static _Atomic int g_rumble_posted;
static struct { gpad *to; uint16_t lo, hi; uint64_t sent; } g_rumble[DM_PLAYERS];   // main thread: the pad each player's rumble goes to, its levels and when they were last sent
static float g_gain = 1, g_rumble_min;
static void rumble_sink(dm_haptic_state s) {
    if (s.player < 0 || s.player >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_rumble_mu); g_rumble_in[s.player] = s; g_rumble_new |= 1u << s.player; pthread_mutex_unlock(&g_rumble_mu);
    if (!SDL_IsMainThread() && !atomic_exchange(&g_rumble_posted, 1)) SDL_RunOnMainThread(wake_cb, &g_rumble_posted, false);   // on the main thread its pass picks it up
}
static float controller_level(float x) {
    x = isfinite(x) ? fmaxf(0, fminf(1, x)) : 0;
    return x <= g_rumble_min ? 0 : (x - g_rumble_min) / (1 - g_rumble_min);
}
static void rumble_send(int p, uint64_t now) {
    gpad *q = g_rumble[p].to;
    if (!q) return;
    uint16_t lo = g_rumble[p].lo, hi = g_rumble[p].hi;
    SDL_RumbleGamepad(q->pad, lo, hi, lo || hi ? RUMBLE_MS : 0);
    g_rumble[p].sent = now;
}
static void rumble_apply(uint64_t now) {
    pthread_mutex_lock(&g_rumble_mu);
    uint32_t m = g_rumble_new; g_rumble_new = 0; dm_haptic_state in[DM_PLAYERS];
    if (m) memcpy(in, g_rumble_in, sizeof in);
    pthread_mutex_unlock(&g_rumble_mu);
    for (int p = 0; p < DM_PLAYERS; p++) if (m >> p & 1) {
        const dm_vibration *l = &in[p].motor[0], *r = &in[p].motor[1];
        uint16_t lo = (uint16_t)lrintf(controller_level((0.85f * l->amplitude_low + 0.15f * r->amplitude_low) * g_gain) * 65535);
        uint16_t hi = (uint16_t)lrintf(controller_level((0.15f * l->amplitude_high + 0.85f * r->amplitude_high) * g_gain) * 65535);
        if (lo == g_rumble[p].lo && hi == g_rumble[p].hi) continue;
        g_rumble[p].lo = lo; g_rumble[p].hi = hi; rumble_send(p, now);
    }
}
static void rumble_tick(uint64_t now) {
    for (int p = 0; p < DM_PLAYERS; p++) if (g_rumble[p].to && (g_rumble[p].lo || g_rumble[p].hi) && now - g_rumble[p].sent >= RUMBLE_RESEND_MS) rumble_send(p, now);
}
// Main thread: player p's rumble goes to q (NULL: none). A change stops the old pad and clears the player's state, which then needs a fresh signal from the game (dm_haptics_route, as
// haptics_apple.m reroutes a player)
static void rumble_target(int p, gpad *q) {
    if (g_rumble[p].to == q) return;
    if (g_rumble[p].to && (g_rumble[p].lo || g_rumble[p].hi)) SDL_RumbleGamepad(g_rumble[p].to->pad, 0, 0, 0);
    g_rumble[p].to = q; g_rumble[p].lo = g_rumble[p].hi = 0; g_rumble[p].sent = 0;
    dm_haptics_route(p, q && q->rumble);
}
static void haptics_start(void) {
    const char *e = getenv("DM_RUMBLE_GAIN");
    if (e) { float v = strtof(e, NULL); if (isfinite(v)) g_gain = fmaxf(0, fminf(3, v)); }
    e = getenv("DM_RUMBLE_MIN");
    if (e) { float v = strtof(e, NULL); if (isfinite(v)) g_rumble_min = fmaxf(0, fminf(0.9f, v)); }
    e = getenv("DM_RUMBLE");
    dm_haptics_init(rumble_sink, !e || atoi(e) != 0);
}

// Motion: SDL's gamepad frame (x right, y up out of the face, z toward the player; the accelerometer in m/s² as the specific force, +1 G up at rest; the gyro in rad/s, counter-clockwise
// positive) into the frame dm_motion_set takes (dm_input.h, motion_fusion.h: Apple's controller frame, x right, y forward, z up out of the face; acceleration in G toward gravity,
// (0, 0, -1) face up at rest; rad/s). The axes map by a proper rotation, (x, y, z) -> (x, -z, y), so the rotation rate maps as a vector; the acceleration also changes sign and unit
static void motion_from_sdl(const float in[3], float out[3], float scale) { out[0] = scale * in[0]; out[1] = -scale * in[2]; out[2] = scale * in[1]; }
static int motion_on(gpad *q) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return 0;
    if (!SDL_GamepadHasSensor(q->pad, SDL_SENSOR_GYRO) || !SDL_GamepadHasSensor(q->pad, SDL_SENSOR_ACCEL)) return 0;
    if (!SDL_SetGamepadSensorEnabled(q->pad, SDL_SENSOR_ACCEL, true) || !SDL_SetGamepadSensorEnabled(q->pad, SDL_SENSOR_GYRO, true)) {
        LOG("[RED] controller '%s': its motion sensors did not start (%s)", q->name, SDL_GetError()); return 0;
    }
    return 1;
}
static void on_sensor(const SDL_GamepadSensorEvent *e) {
    gpad *q = pad_find(e->which);
    if (!q || !q->motion) return;
    int p = g_merge ? 0 : q->player;   // merged: the last one moved drives player 1's six-axis sensor
    if (p < 0) return;
    if (e->sensor == SDL_SENSOR_ACCEL) { motion_from_sdl(e->data, q->acc, -1.f / SDL_STANDARD_GRAVITY); q->have_acc = 1; return; }
    if (e->sensor != SDL_SENSOR_GYRO || !q->have_acc) return;   // one sample per gyro reading, with the newest acceleration
    float rot[3]; motion_from_sdl(e->data, rot, 1.f);
    // Some drivers advertise motion but send only zeroes (motion_apple.h, #296): no source until data arrives; later zeroes can be a real free-fall sample
    if (!q->live) {
        if (q->acc[0] == 0 && q->acc[1] == 0 && q->acc[2] == 0 && rot[0] == 0 && rot[1] == 0 && rot[2] == 0) return;
        q->live = 1;
    }
    dm_motion_set(p, q->acc, rot);
}

static int32_t pad_axis(SDL_Gamepad *g, SDL_GamepadAxis a, int up) {   // up: SDL's y axes grow downward, the Switch's upward
    int32_t v = SDL_GetGamepadAxis(g, a); if (up) v = -v;
    return v > DM_STICK_MAX ? DM_STICK_MAX : v < -DM_STICK_MAX ? -DM_STICK_MAX : v;
}
static void pad_read(gpad *q) {
    if (!g_input_on) return;
    int p = g_merge ? 0 : q->player;
    if (p < 0) return;
    if (g_merge) rumble_target(0, q);   // the last one moved rumbles, as front_mac.m hands player 1's haptics to the controller it reads
    SDL_Gamepad *g = q->pad; uint64_t m = 0;
    #define K(btn, bit) if (SDL_GetGamepadButton(g, btn)) m |= bit
    K(SDL_GAMEPAD_BUTTON_EAST, DM_BTN_A); K(SDL_GAMEPAD_BUTTON_SOUTH, DM_BTN_B); K(SDL_GAMEPAD_BUTTON_NORTH, DM_BTN_X); K(SDL_GAMEPAD_BUTTON_WEST, DM_BTN_Y);   // by position: right A, bottom B, top X, left Y
    K(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DM_BTN_L); K(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DM_BTN_R);
    K(SDL_GAMEPAD_BUTTON_START, DM_BTN_PLUS); K(SDL_GAMEPAD_BUTTON_BACK, DM_BTN_MINUS); K(SDL_GAMEPAD_BUTTON_LEFT_STICK, DM_BTN_LS); K(SDL_GAMEPAD_BUTTON_RIGHT_STICK, DM_BTN_RS);
    K(SDL_GAMEPAD_BUTTON_DPAD_LEFT, DM_BTN_LEFT); K(SDL_GAMEPAD_BUTTON_DPAD_UP, DM_BTN_UP); K(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT); K(SDL_GAMEPAD_BUTTON_DPAD_DOWN, DM_BTN_DOWN);
    #undef K
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= TRIGGER_HALF) m |= DM_BTN_ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= TRIGGER_HALF) m |= DM_BTN_ZR;
    // The sticks as SDL reads them: the dead zone is the host's (hle_hid.c), as it is for GameController's values
    dm_pad_set_player(p, m, pad_axis(g, SDL_GAMEPAD_AXIS_LEFTX, 0), pad_axis(g, SDL_GAMEPAD_AXIS_LEFTY, 1), pad_axis(g, SDL_GAMEPAD_AXIS_RIGHTX, 0), pad_axis(g, SDL_GAMEPAD_AXIS_RIGHTY, 1));
}
static void pad_assign(gpad *q) {   // a slot for a pad that has none
    char nb[16];
    int p = pad_slot_take(&g_slots, q, q->name);
    if (p < 0) { LOG("controller '%s' connected: all %d players are taken, not used", q->name, DM_PLAYERS); return; }
    q->player = p; g_held[p] = q;
    rumble_target(p, q);
    npad_name(p, nb, sizeof nb); LOG("controller '%s' connected: player %d (npad %s)%s", q->name, p + 1, nb, q->motion ? ", with motion (gyro)" : "");
    pad_read(q);
}
static void pad_added(SDL_JoystickID id) {
    if (pad_find(id)) return;
    gpad *q = NULL; for (int i = 0; i < MAX_PADS && !q; i++) if (!g_pads[i].pad) q = &g_pads[i];
    if (!q) { LOG("controller connected: more than %d controllers at once, not used", MAX_PADS); return; }
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { LOG("[RED] controller %u: SDL could not open it (%s)", (unsigned)id, SDL_GetError()); return; }
    *q = (gpad){.pad = g, .id = id, .player = -1};
    // The device's own name (as GameController's vendorName on the Mac), the key of its slot: SDL's gamepad name is its mapping's, which two different controllers can share
    snprintf(q->name, sizeof q->name, "%s", SDL_GetJoystickName(SDL_GetGamepadJoystick(g)) ?: SDL_GetGamepadName(g) ?: "?");
    q->rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    q->motion = motion_on(q);
    if (g_merge) { LOG("controller connected: %s%s", q->name, q->motion ? ", with motion (gyro)" : ""); pad_read(q); return; }
    pad_assign(q);
    dm_pad_players(pad_slot_mask(&g_slots));
}
static void pad_removed(SDL_JoystickID id) {
    gpad *q = pad_find(id); char nb[16];
    if (!q) return;
    for (int p = 0; p < DM_PLAYERS; p++) if (g_rumble[p].to == q) rumble_target(p, NULL);
    if (g_merge) {
        LOG("controller disconnected: %s", q->name);
        if (q->motion) dm_motion_clear(0);
    } else if (q->player >= 0) {
        int p = q->player;
        npad_name(p, nb, sizeof nb); LOG("controller '%s' disconnected: player %d (npad %s) is free", q->name, p + 1, nb);
        if (q->motion) dm_motion_clear(p);
        pad_slot_free(&g_slots, q); g_held[p] = NULL;
        dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    }
    SDL_CloseGamepad(q->pad); *q = (gpad){0};
    if (g_merge) { if (!any_pad()) dm_pad_set(0, 0, 0, 0, 0); return; }
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].pad && g_pads[i].player < 0) pad_assign(&g_pads[i]);
    dm_pad_players(pad_slot_mask(&g_slots));
}
static void pads_start(void) {
    const char *e = getenv("DM_PAD_MERGE"); g_merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); g_kbd_own = !g_merge && e && !strcmp(e, "own");
    static const char kbd = 0;
    if (g_kbd_own) pad_slot_take(&g_slots, &kbd, "keyboard");   // holds player 1 for good
    LOG("front end: %s", g_merge ? "every controller is player 1 (DM_PAD_MERGE)" : g_kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)"
                                                                           : "each controller is a player of its own, the keyboard plays with player 1");
    dm_pad_players(pad_slot_mask(&g_slots));   // the controllers already connected arrive as SDL_EVENT_GAMEPAD_ADDED
}

// Focus (front_mac.m): a window that is no longer focused sees no key-up, mouse-up or controller release, so whatever is held when it loses focus would stay pressed in the game. The front
// end releases only what it pressed itself; out of focus a controller's changes are ignored, and back in focus each controller is read as it is now
static int ti_active(void);
static void input_release(void) {
    g_input_on = 0;
    g_btn = 0; g_w = g_a = g_s = g_d = 0; push();
    if (g_mouse_down) { g_mouse_down = 0; dm_touch_set(NULL, 0); }
    if (g_merge) { if (any_pad()) dm_pad_set(0, 0, 0, 0, 0); }
    else for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
}
static void input_resume(void) {
    if (ti_active()) return;   // the text box keeps the keyboard until it closes
    g_input_on = 1;
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].pad) pad_read(&g_pads[i]);
}

// ---------- Progress screen of the prepare step (front.h front_progress) ----------
// A dark screen with the title, a bar and "done of total" in the window, drawn with SDL's software renderer, and the same text in the window title. Declared limits until the Vulkan
// presenter draws it (Phase 4): a renderer cannot share the window with the presenter's swapchain, so once the presenter has made its surface (front_vk_create_surface) only the title
// shows it; Wayland gives SDL no software window surface, so there it is always the title; the text is SDL's built-in 8x8 debug font (ASCII; other characters show as boxes)
static pthread_mutex_t g_prog_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char title[128]; long done, total; int shown, changed; } g_prog;   // under g_prog_mu: the newest front_progress call
static _Atomic int g_prog_posted;
static SDL_Renderer *g_ren;                   // main thread: the progress screen's renderer while it is up
static int g_ren_failed, g_vk_surface, g_prog_up;   // no software surface for the window; the presenter made its surface; the progress screen is up
static uint64_t g_prog_drawn;                 // SDL_GetTicks of the last draw
enum { PROG_MS = 33 };   // about 30 draws a second at most: the prepare step reports every pipeline it builds, and a draw for each would take CPU from the compiles
static void group(char *out, size_t cap, long v) {   // 27297 -> "27,297", as the Mac's decimal number style
    char d[32]; snprintf(d, sizeof d, "%ld", v < 0 ? -v : v); size_t n = strlen(d), k = 0;
    if (v < 0 && k + 1 < cap) out[k++] = '-';
    for (size_t i = 0; i < n && k + 2 < cap; i++) { if (i && (n - i) % 3 == 0) out[k++] = ','; out[k++] = d[i]; }
    out[k] = 0;
}
static void prog_text(const char *s, float scale, float cx, float y) {   // centered on cx, top at y (pixels)
    SDL_SetRenderScale(g_ren, scale, scale);
    SDL_RenderDebugText(g_ren, cx / scale - (float)strlen(s) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE / 2, y / scale, s);
    SDL_SetRenderScale(g_ren, 1, 1);
}
static void prog_draw(const char *title, long done, long total) {
    char a[32], b[32], count[80], t[256];
    group(a, sizeof a, done); group(b, sizeof b, total); snprintf(count, sizeof count, "%s of %s", a, b);
    snprintf(t, sizeof t, "%s - %s %s", DM_GAME_NAME, title, count); SDL_SetWindowTitle(g_win, t);
    g_prog_up = 1;
    if (!g_ren && !g_ren_failed && !g_vk_surface) {
        g_ren = SDL_CreateRenderer(g_win, SDL_SOFTWARE_RENDERER);
        if (!g_ren) { g_ren_failed = 1; LOG("front end: the progress screen shows in the window title only (no software surface for the window: %s)", SDL_GetError()); }
    }
    if (!g_ren) return;
    int w = 0, h = 0; SDL_GetCurrentRenderOutputSize(g_ren, &w, &h);
    float k = SDL_GetWindowDisplayScale(g_win); if (!(k > 0)) k = 1;
    float top = floorf((h - 96 * k) / 2), bw = 360 * k, bh = 20 * k, bx = floorf((w - bw) / 2), by = top + 40 * k;   // front_mac.m's 400x96 box: title, bar, count
    float f = total > 0 ? fminf(1, fmaxf(0, (float)done / (float)total)) : 0;
    SDL_FRect frame = {bx, by, bw, bh}, fill = {bx + 2 * k, by + 2 * k, (bw - 4 * k) * f, bh - 4 * k};
    SDL_SetRenderDrawColor(g_ren, 15, 15, 15, 255); SDL_RenderClear(g_ren);   // the Mac's 0.06 white
    SDL_SetRenderDrawColor(g_ren, 96, 96, 96, 255); SDL_RenderRect(g_ren, &frame);
    SDL_SetRenderDrawColor(g_ren, 10, 132, 255, 255); SDL_RenderFillRect(g_ren, &fill);
    SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255); prog_text(title, roundf(3 * k), w / 2.f, top + 7 * k);
    SDL_SetRenderDrawColor(g_ren, 184, 184, 184, 255); prog_text(count, roundf(2 * k), w / 2.f, top + 73 * k);
    SDL_RenderPresent(g_ren);
}
static void prog_hide(void) {
    if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; SDL_DestroyWindowSurface(g_win); }
    if (g_prog_up && !ti_active()) SDL_SetWindowTitle(g_win, DM_GAME_NAME);
    g_prog_up = 0;
}
static void prog_tick(uint64_t now, int force) {   // main thread: the newest call, drawn when its time has come; force: draw again now (the window changed)
    char title[128]; long done, total; int shown;
    pthread_mutex_lock(&g_prog_mu);
    if (!g_prog.changed && !(force && g_prog_up)) { pthread_mutex_unlock(&g_prog_mu); return; }
    shown = g_prog.shown;
    if (shown && !force && now - g_prog_drawn < PROG_MS) { pthread_mutex_unlock(&g_prog_mu); return; }   // next_wait wakes the loop for it
    snprintf(title, sizeof title, "%s", g_prog.title); done = g_prog.done; total = g_prog.total; g_prog.changed = 0;
    pthread_mutex_unlock(&g_prog_mu);
    if (!shown) { prog_hide(); return; }
    if (ti_active()) g_prog_up = 1;   // the text box has the title bar meanwhile: drawn when it closes (ti_end)
    else prog_draw(title, done, total);
    g_prog_drawn = now;
}
void front_progress(const char *title, long done, long total) {   // any thread; does not block
    if (!g_win) return;
    pthread_mutex_lock(&g_prog_mu);
    g_prog.shown = title != NULL; if (title) snprintf(g_prog.title, sizeof g_prog.title, "%s", title);
    g_prog.done = done; g_prog.total = total; g_prog.changed = 1;
    pthread_mutex_unlock(&g_prog_mu);
    if (!SDL_IsMainThread() && !atomic_exchange(&g_prog_posted, 1)) SDL_RunOnMainThread(wake_cb, &g_prog_posted, false);
}

// ---------- Text input box (front.h front_text_input, for the software keyboard swkbd) ----------
// SDL has no native text box, so the window is a one-line editor while the box is up: SDL's text input (the system's keyboard layout and input methods) types into it, Backspace deletes
// a character, Ctrl+V pastes, Return confirms, Escape cancels, and the text shows in the window title (SDL has no font for the window itself; a declared limit until the presenter can
// draw it). The calling thread blocks until then, as front_mac.m's box. As there, the box takes the keyboard from the game: input is released and the game sees focus lost while it is up
extern int g_swkbd_min_chars;   // hle_swkbd.c: this input's minimum length from ShowKeyboardArg; the applet keeps OK disabled until the text is long enough
static struct {
    pthread_mutex_t mu; pthread_cond_t cv;
    int active, result, min_chars, too_short;   // result: -2 while the box is up, then 1 confirmed / 0 cancelled
    char title[256], sub[256], guide[256], text[2048];
    size_t cap;                                 // the text stays below cap bytes (the caller's buffer, at most sizeof text)
} g_ti = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
static pthread_mutex_t g_ti_one = PTHREAD_MUTEX_INITIALIZER;   // one box at a time
static int ti_active(void) { return g_ti.active; }
static int utf16_units(const char *s) {   // the length games count (UTF-16 units): a 4-byte UTF-8 sequence is two
    int n = 0; for (const unsigned char *p = (const unsigned char *)s; *p; p++) if ((*p & 0xC0) != 0x80) n += *p >= 0xF0 ? 2 : 1;
    return n;
}
static void ti_append(const char *s) {   // whole UTF-8 sequences while they fit; control characters (a pasted newline) are left out
    size_t n = strlen(g_ti.text);
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        size_t len = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1;
        size_t have = 0; while (have < len && p[have]) have++;
        if (have < len) break;   // a cut sequence
        if (*p < 0x20 || *p == 0x7F) { p += len; continue; }
        if (n + len >= g_ti.cap) break;
        memcpy(g_ti.text + n, p, len); n += len; g_ti.text[n] = 0; p += len;
    }
}
static void ti_show(void) {
    char t[4096];
    snprintf(t, sizeof t, "%s%s%s%s: %s_%s%s%s", g_ti.title[0] ? g_ti.title : "Input", g_ti.sub[0] ? " (" : "", g_ti.sub, g_ti.sub[0] ? ")" : "", g_ti.text,
             !g_ti.text[0] && g_ti.guide[0] ? "   [" : "", !g_ti.text[0] && g_ti.guide[0] ? g_ti.guide : "", !g_ti.text[0] && g_ti.guide[0] ? "]" : "");
    size_t n = strlen(t);
    if (g_ti.too_short && n < sizeof t) n += (size_t)snprintf(t + n, sizeof t - n, "   (at least %d characters are needed here)", g_ti.min_chars);
    if (n < sizeof t) snprintf(t + n, sizeof t - n, "   Return: OK, Escape: cancel");
    SDL_SetWindowTitle(g_win, t);
}
static void ti_end(int r) {   // main thread
    SDL_StopTextInput(g_win); g_ti.active = 0;
    SDL_SetWindowTitle(g_win, DM_GAME_NAME);
    if (g_prog_up) prog_tick(SDL_GetTicks(), 1);   // the progress screen takes the title back
    if (g_has_focus && !g_minimized) { input_resume(); dm_focus_set(1); }   // as front_mac.m's window becoming key again
    pthread_mutex_lock(&g_ti.mu); g_ti.result = r; pthread_cond_signal(&g_ti.cv); pthread_mutex_unlock(&g_ti.mu);
}
static void SDLCALL ti_begin(void *u) {   // main thread
    (void)u;
    // The box takes the keyboard and the mouse, so the game never sees the key-up of the button or the touch-up of the click that opened it: nothing may stay pressed while it is up
    // (front_mac.m: Stardew Valley's character creation re-opened the keyboard endlessly)
    input_release(); if (!g_minimized) dm_focus_set(2);
    g_ti.active = 1; SDL_StartTextInput(g_win); ti_show();
}
static void ti_key(const SDL_KeyboardEvent *k) {   // main thread, a key while the box is up
    switch (k->scancode) {
    case SDL_SCANCODE_BACKSPACE: {
        size_t n = strlen(g_ti.text);
        if (n) { n--; while (n && ((unsigned char)g_ti.text[n] & 0xC0) == 0x80) n--; g_ti.text[n] = 0; }
        g_ti.too_short = 0; ti_show(); break;
    }
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
        if (k->repeat) break;
        // On hardware the applet would not have accepted the shorter text (OK stayed disabled); a game that rejects it re-opens the keyboard for ever, so the box stays with the rule
        if (g_ti.min_chars > 0 && utf16_units(g_ti.text) < g_ti.min_chars) { g_ti.too_short = 1; ti_show(); break; }
        ti_end(1); break;
    case SDL_SCANCODE_ESCAPE: if (!k->repeat) ti_end(0); break;
    case SDL_SCANCODE_V:
        if (k->mod & SDL_KMOD_CTRL) { char *c = SDL_GetClipboardText(); if (c) { ti_append(c); SDL_free(c); g_ti.too_short = 0; ti_show(); } }
        break;
    default: break;
    }
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)max_chars;   // truncation is done uniformly in hle_swkbd.c in UTF-16 units
    if (!g_win || !out || out_cap <= 0) return -1;
    if (SDL_IsMainThread()) { LOG("[RED] front end: text input asked on the main thread, which has to run the box: treated as no window"); return -1; }
    pthread_mutex_lock(&g_ti_one);
    pthread_mutex_lock(&g_ti.mu);
    snprintf(g_ti.title, sizeof g_ti.title, "%s", title ? title : ""); snprintf(g_ti.sub, sizeof g_ti.sub, "%s", sub ? sub : "");
    snprintf(g_ti.guide, sizeof g_ti.guide, "%s", guide ? guide : "");
    g_ti.cap = (size_t)out_cap < sizeof g_ti.text ? (size_t)out_cap : sizeof g_ti.text; g_ti.text[0] = 0; ti_append(initial ? initial : "");
    g_ti.min_chars = g_swkbd_min_chars > 0 ? g_swkbd_min_chars : 0; g_ti.too_short = 0; g_ti.result = -2;
    pthread_mutex_unlock(&g_ti.mu);
    SDL_RunOnMainThread(ti_begin, NULL, false);
    pthread_mutex_lock(&g_ti.mu);
    while (g_ti.result == -2) pthread_cond_wait(&g_ti.cv, &g_ti.mu);
    int r = g_ti.result;
    if (r == 1) snprintf(out, (size_t)out_cap, "%s", g_ti.text);
    pthread_mutex_unlock(&g_ti.mu);
    pthread_mutex_unlock(&g_ti_one);
    return r;
}

// ---------- Save not written (front.h front_save_failed; the iOS front end's wording) ----------
// A message box over the window, shown by the main thread; the game's thread does not wait. SDL's box is modal on the main thread, which pumps no events while it is up, so first the
// input is released (the events that would release it come only after the box closes) and the motors are stopped (SDL ends a timed rumble only while the main thread pumps); one notice
// at a time: a failure while one is up or queued is only logged
static _Atomic int g_box_pending;
static void SDLCALL save_box(void *arg) {
    int err = (int)(intptr_t)arg; char m[256];
    if (err == ENOSPC) snprintf(m, sizeof m, "Storage is full, so progress was not saved. Free up space and save again.");
    else snprintf(m, sizeof m, "Saving failed (errno %d: %s); progress was not saved.", err, strerror(err));
    input_release();
    dm_haptics_suspend(DM_HAPTICS_INTERRUPTED, 1); rumble_apply(SDL_GetTicks());   // the cleared states go out at once
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Save failed", m, g_win)) LOG("[RED] front end: the save-failure notice could not be shown (%s)", SDL_GetError());
    dm_haptics_suspend(DM_HAPTICS_INTERRUPTED, 0);
    if (g_has_focus && !g_minimized) input_resume();
    g_box_pending = 0;
}
void front_save_failed(int err) {   // the game's thread; does not block
    LOG("front end: save data write failed errno=%d, telling the player", err);
    if (!g_win) return;
    if (atomic_exchange(&g_box_pending, 1)) { LOG("front end: a save-failure notice is already up, not shown again"); return; }
    SDL_RunOnMainThread(save_box, (void *)(intptr_t)err, false);
}

// ---------- Vulkan surface (front.h; the Vulkan presenter of Phase 4) ----------
typedef struct { const char *const *names; Uint32 n; } vk_exts;
static void SDLCALL vk_exts_cb(void *arg) { vk_exts *v = arg; v->names = SDL_Vulkan_GetInstanceExtensions(&v->n); if (!v->names) { v->n = 0; LOG("[RED] front end: SDL gives no Vulkan instance extensions (%s)", SDL_GetError()); } }
int front_vk_instance_extensions(const char **names, unsigned *n) {
    vk_exts v = {NULL, 0};
    if (g_win && g_vk_window) SDL_RunOnMainThread(vk_exts_cb, &v, true);
    unsigned k = 0;
    if (names && n) for (; k < *n && k < v.n; k++) names[k] = v.names[k];
    if (n) *n = k;
    return (int)v.n;
}
typedef struct { void *instance; VkSurfaceKHR surface; int ok; } vk_surf;
static void SDLCALL vk_surface_cb(void *arg) {
    vk_surf *v = arg;
    if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; }
    SDL_DestroyWindowSurface(g_win);   // the progress screen's software surface goes before the swapchain's
    g_vk_surface = 1;
    v->ok = SDL_Vulkan_CreateSurface(g_win, (VkInstance)v->instance, NULL, &v->surface);
    if (!v->ok) LOG("[RED] front end: no Vulkan surface for the window (%s)", SDL_GetError());
}
int front_vk_create_surface(void *vk_instance, void **vk_surface_out) {
    if (!g_win || !g_vk_window || !vk_instance || !vk_surface_out) return -1;
    vk_surf v = {vk_instance, 0, 0};
    SDL_RunOnMainThread(vk_surface_cb, &v, true);
    if (!v.ok) return -1;
    *vk_surface_out = (void *)v.surface;
    return 0;
}

// ---------- The event loop ----------
static void toggle_fullscreen(void) { SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)); }
static void handle(const SDL_Event *e) {   // main thread
    switch (e->type) {
    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: exit(0);
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        const SDL_KeyboardEvent *k = &e->key;
        if (g_ti.active) { if (k->down) ti_key(k); break; }   // the text box has the keyboard
        if (k->down && (k->scancode == SDL_SCANCODE_F11 || (k->scancode == SDL_SCANCODE_RETURN && (k->mod & SDL_KMOD_ALT)))) { if (!k->repeat) toggle_fullscreen(); break; }   // a key-up still reaches the game, or it would stay pressed
        if (!k->repeat) on_key(k->scancode, k->down);
        break;
    }
    case SDL_EVENT_TEXT_INPUT: if (g_ti.active) { ti_append(e->text.text); g_ti.too_short = 0; ti_show(); } break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: if (e->button.button == SDL_BUTTON_LEFT && !g_ti.active) mouse_touch(e->button.x, e->button.y); break;
    case SDL_EVENT_MOUSE_MOTION: if (g_mouse_down && (e->motion.state & SDL_BUTTON_LMASK)) mouse_touch(e->motion.x, e->motion.y); break;
    case SDL_EVENT_MOUSE_BUTTON_UP: if (e->button.button == SDL_BUTTON_LEFT && g_mouse_down) { g_mouse_down = 0; dm_touch_set(NULL, 0); } break;
    // Lifecycle (hle.h / dm.h, as front_mac.m): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resumed, then 1 when it has the focus
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_has_focus = 0; input_release(); dm_haptics_suspend(DM_HAPTICS_INACTIVE, 1);
        if (!g_minimized) dm_focus_set(2);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        g_has_focus = 1; dm_haptics_suspend(DM_HAPTICS_INACTIVE, 0);
        if (!g_minimized && !g_ti.active) { input_resume(); dm_focus_set(1); }
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        if (g_minimized) break;
        g_minimized = 1; input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1);
        break;
    case SDL_EVENT_WINDOW_RESTORED: case SDL_EVENT_WINDOW_MAXIMIZED:   // out of the dock or taskbar, at its old size or maximized; RESTORED also follows a maximized window shrinking back
        if (!g_minimized) break;
        g_minimized = 0; dm_gfx_suspend(0); dm_audio_suspend(0);
        if (g_has_focus && !g_ti.active) { input_resume(); dm_focus_set(1); } else dm_focus_set(2);
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
        screen_match_apply(); break;
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:   // the software renderer takes the new surface by itself
        prog_tick(SDL_GetTicks(), 1); break;
    case SDL_EVENT_JOYSTICK_ADDED:
        if (!SDL_IsGamepad(e->jdevice.which)) LOG("controller connected: %s (no gamepad mapping in SDL: not used)", SDL_GetJoystickNameForID(e->jdevice.which) ?: "?");
        break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_added(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_REMOVED: pad_removed(e->gdevice.which); break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: { gpad *q = pad_find(e->gbutton.which); if (q) q->dirty = 1; break; }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: { gpad *q = pad_find(e->gaxis.which); if (q) q->dirty = 1; break; }
    case SDL_EVENT_GAMEPAD_REMAPPED: { gpad *q = pad_find(e->gdevice.which); if (q) q->dirty = 1; break; }
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: on_sensor(&e->gsensor); break;
    default: break;
    }
}
static void after_events(void) {   // main thread, after each pass over the queued events: what they and the other threads left to do
    uint64_t now = SDL_GetTicks();
    for (int i = 0; i < MAX_PADS; i++) if (g_pads[i].pad && g_pads[i].dirty) { g_pads[i].dirty = 0; pad_read(&g_pads[i]); }
    rumble_apply(now); rumble_tick(now);
    prog_tick(now, 0);
}
// One pass: every queued event (SDL also runs the work other threads posted, SDL_RunOnMainThread), then after_events. front_run's loop waits for events between passes; the unit test
// (tests/t_front_sdl.c) drives the front end with this alone
static void front_sdl_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle(&e);
    after_events();
}
static int next_wait_ms(void) {   // how long the loop may sleep: until the next rumble resend or the next progress draw; -1 = until an event or a post wakes it
    uint64_t now = SDL_GetTicks(); int64_t t = -1;
    for (int p = 0; p < DM_PLAYERS; p++) if (g_rumble[p].to && (g_rumble[p].lo || g_rumble[p].hi)) {
        int64_t r = RUMBLE_RESEND_MS - (int64_t)(now - g_rumble[p].sent); if (t < 0 || r < t) t = r;
    }
    pthread_mutex_lock(&g_prog_mu);
    if (g_prog.changed && g_prog.shown) { int64_t r = PROG_MS - (int64_t)(now - g_prog_drawn); if (t < 0 || r < t) t = r; }
    pthread_mutex_unlock(&g_prog_mu);
    return t < 0 ? -1 : t < 1 ? 1 : (int)t;
}

// At exit: the screen's own mode back (X11 keeps a mode after the program that set it ends) and the motors stopped, on the main thread. exit() can come from the guest thread, while
// the main thread waits for events (it then runs this within its wait) or holds a modal notice (the process ends without it after half a second)
static void SDLCALL exit_cb(void *done) {
    if (g_match_on) { SDL_SetWindowFullscreenMode(g_win, NULL); SDL_SyncWindow(g_win); g_match_on = 0; }
    for (int p = 0; p < DM_PLAYERS; p++) if (g_rumble[p].to && (g_rumble[p].lo || g_rumble[p].hi)) SDL_RumbleGamepad(g_rumble[p].to->pad, 0, 0, 0);
    if (done) plat_sema_signal(done);
}
static void front_atexit(void) {
    if (SDL_IsMainThread()) { exit_cb(NULL); return; }
    plat_sema *s = plat_sema_create(0);
    if (!SDL_RunOnMainThread(exit_cb, s, false)) { plat_sema_destroy(s); return; }
    if (plat_sema_wait(s, 500000000) == 0) plat_sema_destroy(s);   // timed out: s stays for the late callback
}

// Main thread: SDL, the window, rumble and the controllers (front_run; tests/t_front_sdl.c calls it directly)
static void front_sdl_open(void) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // the host owns SIGINT and SIGTERM (plat_on_terminate); SDL would turn them into a quit event
    // The progress screen draws into a plain window surface, never through a GPU renderer standing in for one: that would make the window an OpenGL one and take it from the Vulkan presenter
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    SDL_SetAppMetadata(DM_GAME_NAME, NULL, NULL);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) die("front end: SDL could not start (%s)", SDL_GetError());   // SDL keeps the screen saver off while it runs
    int v = SDL_GetVersion();
    LOG("front end: SDL %d.%d.%d, video driver %s", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetCurrentVideoDriver() ?: "?");
    SDL_WindowFlags fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;   // the swapchain gets the window's real pixels on a scaled screen
    g_win = SDL_CreateWindow(DM_GAME_NAME, 1280, 720, fl | SDL_WINDOW_VULKAN);   // the game profile's name
    g_vk_window = g_win != NULL;
    if (!g_win) {
        const char *drv = SDL_GetCurrentVideoDriver(); int dummy = drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen"));
        LOG("%sfront end: no Vulkan window (%s): a plain window instead%s", dummy ? "" : "[RED] ", SDL_GetError(), dummy ? " (SDL's dummy video driver has no Vulkan)" : "; the Vulkan presenter cannot use it");
        g_win = SDL_CreateWindow(DM_GAME_NAME, 1280, 720, fl);
        if (!g_win) die("front end: SDL could not open the window (%s)", SDL_GetError());
    }
    const char *fs = getenv("DM_FULLSCREEN");   // 1 (or any value but 0): start in fullscreen; 0 or unset: a window
    if (fs && strcmp(fs, "0")) SDL_SetWindowFullscreen(g_win, true);
    screen_fps_update();
    haptics_start();
    pads_start();
}

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
#if !defined(_WIN32)
    static char alt[65536];   // the signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
#endif
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}
void front_run(void (*guest)(void)) {
    if (!getenv("DM_WINDOW")) { guest(); return; }
    front_sdl_open();
    atexit(front_atexit);
    // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
    static guest_arg ga; ga.fn = guest;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, next_wait_ms())) handle(&e);
        front_sdl_pump();
    }
}
