// What t_present_hdr takes of a front end and the loader: strong definitions in a file of their own, which replace the weak defaults in hle_vi.c
#include <stdatomic.h>
#include <string.h>
float t_hdr_headroom = 1.0f;   // the screen's current EDR headroom (front_hdr_headroom: the automatic peak, DM_HDR_PEAK=auto, and the roll-off's target)
float front_hdr_headroom(void) { return t_hdr_headroom; }
float t_hdr_potential;         // its potential headroom (front_hdr_potential: the automatic peak's first answer); 0 unknown
float front_hdr_potential(void) { return t_hdr_potential; }
int t_ounce = 1;               // a Switch 2 game (dm_sdk_ounce: loader.c reads the sdk module's spec); 0 checks a Switch 1 game
int t_imports_hdr = 1;         // one of the game's modules imports QueryDisplayHdrStatus (dm_game_any_imports, loader.c)
int dm_sdk_ounce(void) { return t_ounce; }
int dm_game_any_imports(const char *prefix) { return t_imports_hdr && !strncmp("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", prefix, strlen(prefix)); }
int dm_opmode(void) { return 0; }      // handheld (hle_vi.c's display buffer answer)
