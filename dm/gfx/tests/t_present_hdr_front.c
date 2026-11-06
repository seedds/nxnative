// What t_present_hdr takes of a front end and the loader: strong definitions in a file of their own, which replace the weak defaults in hle_vi.c
#include <stdatomic.h>
#include <string.h>
int t_hdr_screen = 1;          // whether the window's screen can show EDR (front_hdr_screen)
unsigned t_hdr_lo, t_hdr_hi;   // the last range the game declared (front_hdr_content_range)
int front_hdr_screen(void) { return t_hdr_screen; }
void front_hdr_content_range(unsigned lo, unsigned hi) { t_hdr_lo = lo; t_hdr_hi = hi; }
int dm_sdk_ounce(void) { return 1; }   // a Switch 2 game (loader.c reads the sdk module's spec)
int dm_opmode(void) { return 0; }      // handheld (hle_vi.c's display buffer answer)
