// Strong front-end hooks for the production link policy in the paired-present test.
extern int test_pair_screen_hz, test_pair_match_hz;
int front_screen_max_fps(void) { return test_pair_screen_hz; }
void front_screen_match(int fps) { test_pair_match_hz=fps; }
