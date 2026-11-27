#import <GameController/GameController.h>
#import "DuoHarnessRuntime.h"
#include "dm.h"
#include "dm_input.h"
#include "haptics_apple.h"
#include "pad_switch2_apple.h"
#include <pthread.h>
#include <string.h>

NSString * const DuoHarnessInputDidChangeNotification = @"DuoHarnessInputDidChange";

static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;
static DuoHarnessState g_state;

DuoHarnessInput duo_harness_input(void) {
    pthread_mutex_lock(&g_input_lock);
    DuoHarnessInput value = g_state.input;
    pthread_mutex_unlock(&g_input_lock);
    return value;
}

DuoHarnessState duo_harness_state(void) {
    pthread_mutex_lock(&g_input_lock);
    DuoHarnessState value = g_state;
    pthread_mutex_unlock(&g_input_lock);
    return value;
}

void duo_harness_clear(void) {
    pthread_mutex_lock(&g_input_lock);
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_unlock(&g_input_lock);
}

void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    pthread_mutex_lock(&g_input_lock);
    g_state.input.buttons = buttons;
    g_state.input.lx = lx; g_state.input.ly = ly; g_state.input.rx = rx; g_state.input.ry = ry;
    pthread_mutex_unlock(&g_input_lock);
    [[NSNotificationCenter defaultCenter] postNotificationName:DuoHarnessInputDidChangeNotification object:nil];
}

void dm_touch_set(const dm_touch *touches, int count) {
    pthread_mutex_lock(&g_input_lock);
    g_state.input.touch_count = count > 16 ? 16 : count;
    g_state.input.touch_update_count++;
    memset(g_state.input.touches, 0, sizeof(g_state.input.touches));
    if (touches && g_state.input.touch_count > 0) memcpy(g_state.input.touches, touches, (size_t)g_state.input.touch_count * sizeof(*touches));
    if (touches && g_state.input.touch_count > 0) {
        g_state.input.last_touch_count = g_state.input.touch_count;
        memset(g_state.input.last_touches, 0, sizeof(g_state.input.last_touches));
        memcpy(g_state.input.last_touches, touches, (size_t)g_state.input.touch_count * sizeof(*touches));
    }
    pthread_mutex_unlock(&g_input_lock);
    [[NSNotificationCenter defaultCenter] postNotificationName:DuoHarnessInputDidChangeNotification object:nil];
}

void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    dm_pad_set_player(0, buttons, lx, ly, rx, ry);
}

void dm_pad_set_player(int player, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (player < 0 || player >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_input_lock);
    g_state.players[player] = (DuoHarnessPlayerInput){buttons, lx, ly, rx, ry};
    pthread_mutex_unlock(&g_input_lock);
}

void dm_pad_players(uint32_t mask) {
    pthread_mutex_lock(&g_input_lock);
    g_state.connected_players = mask;
    pthread_mutex_unlock(&g_input_lock);
}
int dm_players_reported(void) { return __builtin_popcount(duo_harness_state().connected_players | 1u); }

void dm_motion_set(int player, const float acceleration[3], const float rotation[3]) {
    if (player < 0 || player >= DM_PLAYERS || !acceleration || !rotation) return;
    pthread_mutex_lock(&g_input_lock);
    memcpy(g_state.controller_motion[player].acceleration, acceleration, sizeof(g_state.controller_motion[player].acceleration));
    memcpy(g_state.controller_motion[player].rotation, rotation, sizeof(g_state.controller_motion[player].rotation));
    g_state.controller_motion_mask |= 1u << player;
    pthread_mutex_unlock(&g_input_lock);
}

void dm_motion_set_switch(int player, const float acceleration[3], const float rotation[3]) {
    (void)player; (void)acceleration; (void)rotation;
}

void dm_motion_clear(int player) {
    if (player < 0 || player >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_input_lock);
    memset(&g_state.controller_motion[player], 0, sizeof(g_state.controller_motion[player]));
    g_state.controller_motion_mask &= ~(1u << player);
    pthread_mutex_unlock(&g_input_lock);
}

uint32_t dm_player_npad(int player) { return player == 0 ? 0x20 : (uint32_t)player; }

static int g_opmode;   // handheld, until a test or the in-game menu docks it
int dm_opmode(void) { return g_opmode; }
void dm_set_opmode(int mode) { g_opmode = mode; }
static int g_rumble = 1;
void dm_haptics_set_enabled(int on) { g_rumble = on; }
int duo_harness_rumble(void) { return g_rumble; }
void dm_haptics_apple_init(void) {}
void dm_haptics_apple_controller(int player, GCController *controller) { (void)player; (void)controller; }
void dm_haptics_apple_set_sink(int player, void (*sink)(const dm_haptic_state *state)) { (void)player; (void)sink; }
void dm_joy_set(int player, int side, uint64_t buttons, int32_t sx, int32_t sy) { (void)player; (void)side; (void)buttons; (void)sx; (void)sy; }
int dm_switch2_start(const dm_switch2_front *front) { (void)front; return 0; }
void dm_switch2_active(int on) { (void)on; }
s2_pad *dm_switch2_pads(void) { return NULL; }
void dm_switch2_rumble(const dm_haptic_state *state) { (void)state; }

int front_ios_boot_ok(void) { return 1; }
void front_ios_boot(void) {}
bool dm_show_launcher(void *viewController, void (*done)(void)) { (void)viewController; (void)done; return false; }

uint64_t g_t0_ns;
uint64_t now_ns(void) { return 1; }
uint64_t mtl_present_count(void) { return 0; }
__thread _Atomic int dm_host_work;
void dm_gfx_suspend(int on) { (void)on; }
void dm_fstep_settle(void) {}
void dm_audio_suspend(int on) { (void)on; }
void dm_audio_reset(void) {}
void dm_audio_route_changed(void) {}
int dm_audio_route(void) { return 2; }
void dm_movie_active(int active) { (void)active; }
void dm_focus_set(int state) { (void)state; }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
const char *front_ios_boot_message(void) { return NULL; }
const char *front_ios_device_check(void) { return NULL; }   // a supported GPU
bool front_ios_hud_enabled_from_saved_choice(void) { return false; }
void dm_cloud_saves_env(void) {}
int save_sync_flush(int timeout_ms) { (void)timeout_ms; return 1; }
int front_web_touch(void) { return 0; }
