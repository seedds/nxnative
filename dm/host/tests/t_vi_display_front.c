// The screen t_vi_display tells hle_vi.c of (its own file, to win over hle_vi.c's weak front_display_pixels): 0 x 0 for not known
int g_w, g_h;
int front_display_pixels(int *w, int *h) { if (!g_w) return 0; *w = g_w; *h = g_h; return 1; }
// what else hle_vi.c calls, which these checks do not reach: no frames, no EDR
#include <stdint.h>
uint64_t dm_nvn_frame_count(void) { return 0; }
void front_screen_edr(float *potential, float *current) { *potential = *current = 1; }
float front_launch_edr_potential(void) { return 1; }
