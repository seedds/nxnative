// The SDL3 front end (front_sdl.c, Linux and Windows) on SDL's dummy video driver, with virtual gamepads (SDL_AttachVirtualJoystick): the key map, key repeats, the mouse as touch,
// focus and minimize, players and their slots, buttons, triggers and sticks, rumble from hle_haptics.c (the real one) into the pad's motors, motion in dm_motion_set's frame, the
// progress screen, the save-failure notice (SDL_ShowSimpleMessageBox is recorded here, never shown), the text box from another thread, and what the front end answers without a window.
// The host functions the front end calls are recording stubs. The event loop is driven with front_sdl_pump, never front_run's endless loop
#include <SDL3/SDL.h>
static bool rec_box(SDL_MessageBoxFlags flags, const char *title, const char *msg, SDL_Window *w);
#define SDL_ShowSimpleMessageBox rec_box
#include "../front_sdl.c"
#undef SDL_ShowSimpleMessageBox

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

// ---- Recording stubs of the host ----
static struct { uint64_t btn; int32_t lx, ly; int n; } kbd;
void dm_input_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { (void)rx; (void)ry; kbd.btn = b; kbd.lx = lx; kbd.ly = ly; kbd.n++; }
static struct { uint64_t btn; int32_t lx, ly, rx, ry; int n; } pl[DM_PLAYERS];
void dm_pad_set_player(int p, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (p < 0 || p >= DM_PLAYERS) return;
    pl[p].btn = b; pl[p].lx = lx; pl[p].ly = ly; pl[p].rx = rx; pl[p].ry = ry; pl[p].n++;
}
void dm_pad_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { dm_pad_set_player(0, b, lx, ly, rx, ry); }
static uint32_t players = 0xffffffffu;
void dm_pad_players(uint32_t mask) { players = mask; }
uint32_t dm_player_npad(int p) { return p <= 0 ? 0 : (uint32_t)p; }
static struct { int p, n; float acc[3], rot[3]; } mot = {-1};
static int mot_cleared = -1;
void dm_motion_set(int p, const float acc[3], const float rot[3]) { mot.p = p; mot.n++; memcpy(mot.acc, acc, sizeof mot.acc); memcpy(mot.rot, rot, sizeof mot.rot); }
void dm_motion_clear(int p) { mot_cleared = p; }
static struct { int n, calls; dm_touch t; } touch;
void dm_touch_set(const dm_touch *t, int n) { touch.n = n; touch.calls++; if (n > 0) touch.t = t[0]; }
static int focus_st = 1, audio_susp, gfx_susp;
void dm_focus_set(int st) { focus_st = st; }
void dm_audio_suspend(int on) { audio_susp = on; }
void dm_gfx_suspend(int on) { gfx_susp = on; }
int g_swkbd_min_chars = -1;   // hle_swkbd.c's
static int boxes; static char box_title[64], box_msg[256];
static bool rec_box(SDL_MessageBoxFlags flags, const char *title, const char *msg, SDL_Window *w) {
    (void)flags; (void)w; boxes++; snprintf(box_title, sizeof box_title, "%s", title); snprintf(box_msg, sizeof box_msg, "%s", msg); return true;
}

// ---- Virtual gamepads ----
typedef struct { int calls, stops; Uint16 lo, hi; int sensors; } vrec;
static bool SDLCALL v_rumble(void *u, Uint16 lo, Uint16 hi) { vrec *r = u; r->calls++; r->stops += !lo && !hi; r->lo = lo; r->hi = hi; return true; }
static bool SDLCALL v_sensors(void *u, bool on) { ((vrec *)u)->sensors = on; return true; }
static SDL_JoystickID attach(const char *name, vrec *r) {
    static const SDL_VirtualJoystickSensorDesc sensors[] = {{SDL_SENSOR_ACCEL, 250.f}, {SDL_SENSOR_GYRO, 250.f}};
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1; d.button_mask = (1u << d.nbuttons) - 1;   // the virtual driver numbers its buttons in SDL's gamepad order: index = SDL_GamepadButton
    d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.nsensors = 2; d.sensors = sensors; d.name = name; d.userdata = r; d.Rumble = v_rumble; d.SetSensorsEnabled = v_sensors;
    return SDL_AttachVirtualJoystick(&d);
}
static void pump(void) { for (int i = 0; i < 3; i++) front_sdl_pump(); }
static SDL_Joystick *joy(SDL_JoystickID id) { gpad *q = pad_find(id); return q ? SDL_GetGamepadJoystick(q->pad) : NULL; }
static void button(SDL_JoystickID id, SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(joy(id), b, down); pump(); }
static void axis(SDL_JoystickID id, SDL_GamepadAxis a, Sint16 v) { SDL_SetJoystickVirtualAxis(joy(id), a, v); pump(); }

// ---- Events pushed as the system would send them ----
static void key(SDL_Scancode sc, int down, int repeat, SDL_Keymod mod) {
    SDL_Event e; SDL_zero(e); e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc; e.key.down = down; e.key.repeat = repeat; e.key.mod = mod; e.key.windowID = SDL_GetWindowID(g_win);
    SDL_PushEvent(&e); pump();
}
static void mouse(Uint32 type, float x, float y, int left) {
    SDL_Event e; SDL_zero(e); e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) { e.motion.windowID = SDL_GetWindowID(g_win); e.motion.x = x; e.motion.y = y; e.motion.state = left ? SDL_BUTTON_LMASK : 0; }
    else { e.button.windowID = SDL_GetWindowID(g_win); e.button.button = SDL_BUTTON_LEFT; e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.x = x; e.button.y = y; }
    SDL_PushEvent(&e); pump();
}
static void window_event(Uint32 type) { SDL_Event e; SDL_zero(e); e.type = type; e.window.windowID = SDL_GetWindowID(g_win); SDL_PushEvent(&e); pump(); }
static void text_event(const char *s) { SDL_Event e; SDL_zero(e); e.type = SDL_EVENT_TEXT_INPUT; e.text.windowID = SDL_GetWindowID(g_win); e.text.text = s; SDL_PushEvent(&e); pump(); }
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void t_windowless(void) {
    char out[64] = "x"; unsigned n = 4; const char *names[4];
    CHECK(front_text_input("Name", "", "", "Mario", 0, out, sizeof out) == -1 && !strcmp(out, "x"), "the text box answers -1 without a window and leaves the buffer alone");
    CHECK(front_vk_instance_extensions(names, &n) == 0 && n == 0, "no Vulkan instance extensions without a window");
    void *surf = (void *)1; CHECK(front_vk_create_surface((void *)1, &surf) == -1 && surf == (void *)1, "no Vulkan surface without a window");
    CHECK(front_screen_max_fps() == 0, "no screen rate without a window");
    front_progress("Preparing shaders", 1, 2); front_progress(NULL, 2, 2); front_screen_match(60);
    front_save_failed(ENOSPC);
    CHECK(boxes == 0, "without a window a save failure is only logged");
    CHECK(front_metal_layer() == NULL, "no Metal layer here");
}

static void t_keys(void) {
    static const struct { SDL_Scancode sc; uint64_t bit; } map[] = {
        {SDL_SCANCODE_J, DM_BTN_A}, {SDL_SCANCODE_K, DM_BTN_B}, {SDL_SCANCODE_U, DM_BTN_X}, {SDL_SCANCODE_I, DM_BTN_Y}, {SDL_SCANCODE_F, DM_BTN_LS}, {SDL_SCANCODE_G, DM_BTN_RS},
        {SDL_SCANCODE_Q, DM_BTN_L}, {SDL_SCANCODE_E, DM_BTN_R}, {SDL_SCANCODE_Z, DM_BTN_ZL}, {SDL_SCANCODE_C, DM_BTN_ZR}, {SDL_SCANCODE_RETURN, DM_BTN_PLUS}, {SDL_SCANCODE_TAB, DM_BTN_MINUS},
        {SDL_SCANCODE_LEFT, DM_BTN_LEFT}, {SDL_SCANCODE_UP, DM_BTN_UP}, {SDL_SCANCODE_RIGHT, DM_BTN_RIGHT}, {SDL_SCANCODE_DOWN, DM_BTN_DOWN}, {SDL_SCANCODE_SPACE, DM_BTN_SHAKE}};
    int good = 1;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
        key(map[i].sc, 1, 0, 0); int down = kbd.btn == map[i].bit;
        key(map[i].sc, 0, 0, 0); int up = kbd.btn == 0;
        if (!down || !up) { good = 0; fprintf(stderr, "key %d: btn %#llx after down\n", (int)map[i].sc, (unsigned long long)kbd.btn); }
    }
    CHECK(good, "J K U I F G Q E Z C Return Tab and the arrows press and release their buttons");
    key(SDL_SCANCODE_SPACE, 1, 0, 0); CHECK(kbd.btn == DM_BTN_SHAKE, "Space shakes the controller"); key(SDL_SCANCODE_SPACE, 0, 0, 0);
    key(SDL_SCANCODE_W, 1, 0, 0); CHECK(kbd.ly == 32767 && kbd.lx == 0, "W tilts the left stick up");
    key(SDL_SCANCODE_D, 1, 0, 0); CHECK(kbd.ly == 32767 && kbd.lx == 32767, "W+D tilts it up and right");
    key(SDL_SCANCODE_W, 0, 0, 0); key(SDL_SCANCODE_D, 0, 0, 0); key(SDL_SCANCODE_S, 1, 0, 0); key(SDL_SCANCODE_A, 1, 0, 0);
    CHECK(kbd.ly == -32767 && kbd.lx == -32767, "S+A tilts it down and left");
    key(SDL_SCANCODE_S, 0, 0, 0); key(SDL_SCANCODE_A, 0, 0, 0); CHECK(kbd.lx == 0 && kbd.ly == 0, "released, the stick is centered");
    int n = kbd.n; key(SDL_SCANCODE_K, 1, 1, 0);
    CHECK(kbd.n == n && kbd.btn == 0, "a key repeat is ignored");
    key(SDL_SCANCODE_RETURN, 1, 0, SDL_KMOD_LALT); CHECK(!(kbd.btn & DM_BTN_PLUS), "Alt+Enter toggles fullscreen and does not press +"); key(SDL_SCANCODE_RETURN, 0, 0, SDL_KMOD_LALT);
    if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) toggle_fullscreen();
    n = kbd.n; key(SDL_SCANCODE_ESCAPE, 1, 0, 0); key(SDL_SCANCODE_ESCAPE, 0, 0, 0); key(SDL_SCANCODE_F11, 1, 0, 0); key(SDL_SCANCODE_F11, 0, 0, 0);
    CHECK(kbd.btn == 0, "Escape and F11 press nothing");
    if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) toggle_fullscreen();
}

static void t_mouse(void) {
    SDL_SetWindowSize(g_win, 1280, 720); SDL_SyncWindow(g_win); pump();
    mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 640, 360, 1);
    CHECK(touch.n == 1 && touch.t.id == 0 && near(touch.t.x, .5f) && near(touch.t.y, .5f), "a click in the middle of a 1280x720 window touches (0.5, 0.5)");
    mouse(SDL_EVENT_MOUSE_MOTION, 320, 540, 1);
    CHECK(touch.n == 1 && near(touch.t.x, .25f) && near(touch.t.y, .75f), "a drag moves finger 0 (origin top left)");
    mouse(SDL_EVENT_MOUSE_MOTION, 2000, -50, 1);
    CHECK(touch.n == 1 && near(touch.t.x, .999f) && near(touch.t.y, 0), "a drag out of the window stays on its edge");
    mouse(SDL_EVENT_MOUSE_BUTTON_UP, 320, 540, 0); CHECK(touch.n == 0, "releasing the button lifts the finger");
    int calls = touch.calls; mouse(SDL_EVENT_MOUSE_MOTION, 100, 100, 0);
    CHECK(touch.calls == calls, "moving the mouse without the button touches nothing");
    SDL_SetWindowSize(g_win, 1600, 720); SDL_SyncWindow(g_win);
    int w = 0, h = 0; SDL_GetWindowSize(g_win, &w, &h);
    if (w == 1600 && h == 720) {
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 160, 360, 1);
        CHECK(touch.n == 1 && near(touch.t.x, 0) && near(touch.t.y, .5f), "in a wider window the touch is on the centered 16:9 picture");
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, 160, 360, 0);
    } else printf("note: the dummy driver kept the window at %dx%d: letterbox check skipped\n", w, h);
    SDL_SetWindowSize(g_win, 1280, 720); SDL_SyncWindow(g_win);
}

static void t_focus(void) {
    key(SDL_SCANCODE_J, 1, 0, 0); mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, 640, 360, 1);
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    CHECK(focus_st == 2, "focus lost: the game hears focus state 2");
    CHECK(kbd.btn == 0 && touch.n == 0, "focus lost releases the held key and the touch");
    key(SDL_SCANCODE_K, 1, 0, 0); CHECK(kbd.btn == 0, "out of focus the keys do not reach the game"); key(SDL_SCANCODE_K, 0, 0, 0);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED); CHECK(focus_st == 1, "focus regained: state 1");
    key(SDL_SCANCODE_K, 1, 0, 0); CHECK(kbd.btn == DM_BTN_B, "back in focus the keys reach the game"); key(SDL_SCANCODE_K, 0, 0, 0); key(SDL_SCANCODE_J, 0, 0, 0);
    window_event(SDL_EVENT_WINDOW_MINIMIZED);
    CHECK(focus_st == 3 && audio_susp == 1 && gfx_susp == 1, "minimized: state 3, audio paused, graphics suspended");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST); CHECK(focus_st == 3, "focus lost while minimized keeps state 3");
    window_event(SDL_EVENT_WINDOW_RESTORED);
    CHECK(focus_st == 2 && audio_susp == 0 && gfx_susp == 0, "restored without the focus: resumed, state 2");
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED); CHECK(focus_st == 1, "and focus back: state 1");
}

static vrec r1, r2;
static void t_pads(void) {
    SDL_JoystickID a = attach("Virtual Pad 1", &r1); pump();
    gpad *qa = pad_find(a);
    CHECK(a && qa && qa->player == 0 && players == 1u, "the first gamepad is player 1");
    CHECK(qa && qa->motion && r1.sensors, "its gyro and accelerometer are switched on");
    CHECK(qa && qa->rumble && (dm_haptics_available() & 1u), "its motors take player 1's rumble");
    static const struct { SDL_GamepadButton b; uint64_t bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_EAST, DM_BTN_A}, {SDL_GAMEPAD_BUTTON_SOUTH, DM_BTN_B}, {SDL_GAMEPAD_BUTTON_NORTH, DM_BTN_X}, {SDL_GAMEPAD_BUTTON_WEST, DM_BTN_Y},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DM_BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DM_BTN_R}, {SDL_GAMEPAD_BUTTON_START, DM_BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, DM_BTN_MINUS},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, DM_BTN_LS}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, DM_BTN_RS}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DM_BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, DM_BTN_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DM_BTN_DOWN}};
    int good = 1;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
        button(a, map[i].b, true); int down = pl[0].btn == map[i].bit;
        button(a, map[i].b, false); int up = pl[0].btn == 0;
        if (!down || !up) { good = 0; fprintf(stderr, "gamepad button %d: %#llx while down\n", (int)map[i].b, (unsigned long long)pl[0].btn); }
    }
    CHECK(good, "east south north west = A B X Y by position; shoulders, Start, Back, stick presses and the D-pad map to the Switch's");
    button(a, SDL_GAMEPAD_BUTTON_GUIDE, true); CHECK(pl[0].btn == 0, "the guide button is not used"); button(a, SDL_GAMEPAD_BUTTON_GUIDE, false);
    axis(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767); CHECK(pl[0].btn == DM_BTN_ZL, "the left trigger pressed is ZL");
    axis(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -8000); CHECK(pl[0].btn == 0, "a trigger short of half travel is not");
    axis(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 8000); CHECK(pl[0].btn == DM_BTN_ZL, "past half travel it is");
    axis(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768); axis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767); CHECK(pl[0].btn == DM_BTN_ZR, "the right trigger is ZR");
    axis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    axis(a, SDL_GAMEPAD_AXIS_LEFTX, 32767); axis(a, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    CHECK(pl[0].lx == 32767 && pl[0].ly == 32767, "left stick right and up (SDL's y grows downward) is +32767, +32767");
    axis(a, SDL_GAMEPAD_AXIS_LEFTX, -32768); axis(a, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    CHECK(pl[0].lx == -32767 && pl[0].ly == -20000, "left and down: -32768 is held to -32767, y flipped");
    axis(a, SDL_GAMEPAD_AXIS_RIGHTX, 12345); axis(a, SDL_GAMEPAD_AXIS_RIGHTY, -321);
    CHECK(pl[0].rx == 12345 && pl[0].ry == 321, "the right stick as SDL reads it, no dead zone here");
    axis(a, SDL_GAMEPAD_AXIS_LEFTX, 0); axis(a, SDL_GAMEPAD_AXIS_LEFTY, 0); axis(a, SDL_GAMEPAD_AXIS_RIGHTX, 0); axis(a, SDL_GAMEPAD_AXIS_RIGHTY, 0);

    // Motion
    SDL_SendJoystickVirtualSensorData(joy(a), SDL_SENSOR_ACCEL, 1000, (const float[]){0, SDL_STANDARD_GRAVITY, 0}, 3);
    SDL_SendJoystickVirtualSensorData(joy(a), SDL_SENSOR_GYRO, 1000, (const float[]){0, 0, 0}, 3); pump();
    CHECK(mot.n >= 1 && mot.p == 0 && near(mot.acc[0], 0) && near(mot.acc[1], 0) && near(mot.acc[2], -1) && mot.rot[0] == 0 && mot.rot[1] == 0 && mot.rot[2] == 0,
          "face up at rest: acc (0, 0, -1) G and no rotation reach player 1");
    SDL_SendJoystickVirtualSensorData(joy(a), SDL_SENSOR_ACCEL, 2000, (const float[]){0, 0, SDL_STANDARD_GRAVITY}, 3);
    SDL_SendJoystickVirtualSensorData(joy(a), SDL_SENSOR_GYRO, 2000, (const float[]){1, 2, 3}, 3); pump();
    CHECK(near(mot.acc[0], 0) && near(mot.acc[1], 1) && near(mot.acc[2], 0), "standing on its back edge (gravity away from the player): acc (0, 1, 0) G");
    CHECK(near(mot.rot[0], 1) && near(mot.rot[1], -3) && near(mot.rot[2], 2), "rotation (x, y, z) in SDL's frame is (x, -z, y) in the host's");

    // Rumble
    dm_vibration_update u = {0, 0, {0.5f, 160, 0, 320}}; dm_haptics_send(&u, 1); pump();
    CHECK(r1.lo == 27852 && r1.hi == 0, "the left motor's low band at 0.5 drives the strong motor at 0.85 x 0.5");
    u = (dm_vibration_update){0, 1, {0, 160, 1.f, 320}}; dm_haptics_send(&u, 1); pump();
    CHECK(r1.lo == 27852 && r1.hi == 55705, "the right motor's high band at 1 drives the weak motor at 0.85");
    int stops = r1.stops;
    for (int i = 0; i < 40; i++) { SDL_Delay(10); front_sdl_pump(); }   // 400 ms, more than twice SDL's 150 ms duration, with the loop's resends
    CHECK(r1.lo == 27852 && r1.hi == 55705 && r1.stops == stops, "a held state is sent again before SDL's duration ends: the motors never stop");
    u = (dm_vibration_update){0, 0, {0, 160, 0, 320}}; dm_haptics_send(&u, 1); u.motor = 1; dm_haptics_send(&u, 1); pump();
    CHECK(r1.lo == 0 && r1.hi == 0, "a zero state stops the motors");
    u = (dm_vibration_update){0, 0, {1.f, 160, 0, 320}}; dm_haptics_send(&u, 1); pump();
    int boxes0 = boxes; front_save_failed(EIO);
    CHECK(boxes == boxes0 + 1 && r1.lo == 0 && r1.hi == 0, "the modal save-failure notice stops the motors before it holds the main thread");
    u.value.amplitude_low = 0.5f; dm_haptics_send(&u, 1); pump();
    CHECK(r1.lo == 27852, "after it the game's rumble plays again");
    u.value.amplitude_low = 0; dm_haptics_send(&u, 1); pump();

    // A second player, and the slots
    SDL_JoystickID b = attach("Virtual Pad 2", &r2); pump();
    gpad *qb = pad_find(b);
    CHECK(qb && qb->player == 1 && players == 3u, "the second gamepad is player 2 and dm_pad_players shows both");
    button(b, SDL_GAMEPAD_BUTTON_EAST, true); CHECK(pl[1].btn == DM_BTN_A && pl[0].btn == 0, "player 2's A reaches player 2 only"); button(b, SDL_GAMEPAD_BUTTON_EAST, false);
    button(a, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_DetachVirtualJoystick(a); pump();
    CHECK(!pad_find(a) && players == 2u && pl[0].btn == 0, "the first one gone: player 1 is free and nothing stays pressed");
    CHECK(mot_cleared == 0, "its motion source is cleared");
    CHECK(!(dm_haptics_available() & 1u), "player 1 has no rumble any more");
    a = attach("Virtual Pad 1", &r1); pump(); qa = pad_find(a);
    CHECK(qa && qa->player == 0 && players == 3u, "it reconnects as player 1 again");
    SDL_DetachVirtualJoystick(a); SDL_DetachVirtualJoystick(b); pump();
    CHECK(players == 0u && !any_pad(), "both gone: no player has a controller (hle_hid.c always counts player 1)");
    b = attach("Virtual Pad 2", &r2); pump(); qb = pad_find(b);
    CHECK(qb && qb->player == 1 && players == 2u, "the second controller, reconnected first, gets its own player 2 back by name");
    a = attach("Virtual Pad 1", &r1); pump(); qa = pad_find(a);
    CHECK(qa && qa->player == 0 && players == 3u, "and the first one player 1");
    SDL_DetachVirtualJoystick(a); SDL_DetachVirtualJoystick(b); pump();

    // DM_PAD_MERGE: every controller is player 1
    g_merge = 1;
    a = attach("Virtual Pad 1", &r1); b = attach("Virtual Pad 2", &r2); pump();
    button(b, SDL_GAMEPAD_BUTTON_NORTH, true); CHECK(pl[0].btn == DM_BTN_X && pl[1].btn == 0, "merged: the second controller's X is player 1's"); button(b, SDL_GAMEPAD_BUTTON_NORTH, false);
    SDL_DetachVirtualJoystick(a); SDL_DetachVirtualJoystick(b); pump();
    g_merge = 0;
}

static void t_screen(void) {
    CHECK(front_screen_max_fps() == 0, "the dummy driver reports no refresh rate: 0");
    front_screen_match(60); pump();
    CHECK(!g_match_on, "a window keeps the screen's mode");
}

static void t_progress(void) {
    front_progress("Preparing shaders", 1234, 27297); pump();
    const char *t = SDL_GetWindowTitle(g_win);
    CHECK(t && strstr(t, "Preparing shaders") && strstr(t, "1,234 of 27,297"), "the progress shows in the window title");
    CHECK(g_ren != NULL, "and on the dummy driver's software surface");
    front_progress(NULL, 27297, 27297); pump();
    t = SDL_GetWindowTitle(g_win);
    CHECK(t && !strcmp(t, DM_GAME_NAME) && g_ren == NULL, "hidden: the title is the game's again and the renderer is gone");
}

static void *save_thread(void *p) { (void)p; front_save_failed(EIO); return NULL; }
static void t_save_failed(void) {
    int b0 = boxes; front_save_failed(ENOSPC);
    CHECK(boxes == b0 + 1 && !strcmp(box_title, "Save failed") && strstr(box_msg, "Storage is full"), "out of space: the notice says the storage is full");
    pthread_t th; pthread_create(&th, NULL, save_thread, NULL); pthread_join(th, NULL);
    CHECK(boxes == b0 + 1, "from the game's thread the notice is posted, not shown there");
    pump();
    CHECK(boxes == b0 + 2 && strstr(box_msg, "errno 5"), "the main thread shows it with the error number");
}

typedef struct { const char *initial; int r; char out[64]; } ti_call;
static void *ti_thread(void *p) { ti_call *c = p; c->r = front_text_input("Name", "", "Your name", c->initial, 10, c->out, sizeof c->out); return NULL; }
static int ti_wait_open(void) { for (int i = 0; i < 200 && !g_ti.active; i++) { pump(); SDL_Delay(5); } return g_ti.active; }
static void t_text(void) {
    char out[8];
    CHECK(front_text_input("Name", "", "", "", 0, out, sizeof out) == -1, "on the main thread the box cannot run: -1");
    ti_call c = {"Mar", -9, ""};
    pthread_t th; pthread_create(&th, NULL, ti_thread, &c);
    CHECK(ti_wait_open(), "the box opens on the main thread");
    CHECK(focus_st == 2, "the game loses the focus while the box is up, as on the Mac");
    const char *t = SDL_GetWindowTitle(g_win); CHECK(t && strstr(t, "Name: Mar_"), "the text shows in the window title");
    key(SDL_SCANCODE_J, 1, 0, 0); CHECK(kbd.btn == 0, "typing does not press the game's buttons"); key(SDL_SCANCODE_J, 0, 0, 0);
    text_event("io"); key(SDL_SCANCODE_BACKSPACE, 1, 0, 0); text_event("\xC3\xB1");   // "Mari", then an n with a tilde (UTF-8)
    key(SDL_SCANCODE_BACKSPACE, 1, 0, 0); text_event("o");
    key(SDL_SCANCODE_RETURN, 1, 0, 0); pthread_join(th, NULL);
    CHECK(c.r == 1 && !strcmp(c.out, "Mario"), "typed, a two-byte character deleted whole, Return confirms: 1 and the text");
    CHECK(focus_st == 1 && !g_ti.active, "closed: the game has the focus again");
    t = SDL_GetWindowTitle(g_win); CHECK(t && !strcmp(t, DM_GAME_NAME), "and the window its title");
    ti_call d = {"", -9, ""}; g_swkbd_min_chars = 3;
    pthread_create(&th, NULL, ti_thread, &d); ti_wait_open();
    text_event("ab"); key(SDL_SCANCODE_RETURN, 1, 0, 0); pump();
    CHECK(g_ti.active, "shorter than the game's minimum, Return keeps the box up");
    t = SDL_GetWindowTitle(g_win); CHECK(t && strstr(t, "at least 3"), "and says why");
    key(SDL_SCANCODE_ESCAPE, 1, 0, 0); pthread_join(th, NULL);
    CHECK(d.r == 0, "Escape cancels: 0");
    g_swkbd_min_chars = -1;
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    // Only the test's virtual gamepads: no real controller on the machine takes a player (Linux's evdev driver has no switch; the containers have no input devices)
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0"); SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0"); SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0"); SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0"); SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    unsetenv("DM_PAD_MERGE"); unsetenv("DM_KBD_PLAYER"); unsetenv("DM_MOTION"); unsetenv("DM_RUMBLE"); unsetenv("DM_RUMBLE_GAIN"); unsetenv("DM_RUMBLE_MIN");
    unsetenv("DM_FULLSCREEN"); unsetenv("DM_SCREEN_MATCH");
    t_windowless();
    front_sdl_open();
    CHECK(g_win != NULL, "the window opens on the dummy driver");
    pump();
    { unsigned n = 4; const char *names[4]; CHECK(g_vk_window || (front_vk_instance_extensions(names, &n) == 0 && n == 0), "a plain window (the dummy driver has no Vulkan) has no Vulkan extensions"); }
    focus_st = 1; g_has_focus = 1;
    t_keys();
    t_mouse();
    t_focus();
    t_pads();
    t_screen();
    t_progress();
    t_save_failed();
    t_text();
    SDL_Quit();
    printf("== front_sdl: %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
