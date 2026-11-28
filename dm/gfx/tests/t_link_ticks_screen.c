// Strong screen-rate and Low Power Mode hooks for t_link_ticks.m, over the graphics layer's weak defaults (no screen known, Low Power Mode off): the 40 fps screen check
// (hle_nvn.c fps40_refused) is judged against a screen the test sets, including a display whose native capability stays 120 Hz while front_screen_match holds a 60 Hz mode
extern int test_screen_hz, test_capable_hz, test_low_power;
int front_screen_max_fps(void) { return test_screen_hz; }
int front_screen_capable_fps(void) { return test_capable_hz > 0 ? test_capable_hz : test_screen_hz; }
int dm_low_power_now(void) { return test_low_power; }
