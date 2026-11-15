// Strong focus, fullscreen and frame rate hooks override the graphics layer's weak, always-active test defaults.
#include <stdint.h>
extern int test_focus;
extern uint64_t test_focus_since;
int dm_focus_now(void) { return test_focus; }
uint64_t dm_focus_in_since(void) { return test_focus_since; }
extern int test_fullscreen;
int front_fullscreen(void) { return test_fullscreen; }
extern int test_variable_refresh;
int front_screen_variable_refresh(void) { return test_variable_refresh; }
extern int test_game_fps;
int test_game_fps_now(void) { return test_game_fps; }
extern int test_screen_hz;
int front_screen_max_fps(void) { return test_screen_hz; }
extern int test_match_fps;
void front_screen_match(int fps) { test_match_fps = fps; }   // the rate link_sync asks the screen for
