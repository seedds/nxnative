// Shaders the split missed (dm/gfx/tools/shader_split.py), translated at run time by maxmetal (dm/third_party/maxmetal), on the Mac and the iPhone / iPad: nvn_maxmetal.m.
// A translation is the offline chain's <key>_b.metal + <key>_b.json, kept in the directory nvn_mm_init names (beside pso_user.tsv) and in memory; its library is compiled from the MSL
// on the device (Metal's own cache serves the later starts). DM_NVN_MAXMETAL=0 turns it off (the stage is dropped, as before).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>

// The directory for the translations (nil: memory only). Call once, before the others
void nvn_mm_init(NSString *dir);
int nvn_mm_on(void);
// Translate shader key (16 hex digits) from its code area (ncode bytes, the control's +0x704) and the control fields it needs (nvn_obj.h prog_ctl_t, passed as its fields):
// the stage (0 vertex, 1 pixel, 5 compute), or -1 with why set. A key that failed once fails again at once
int nvn_mm_translate(NSString *key, const uint8_t *code, uint32_t ncode, uint32_t c1_off, const uint32_t local[3], int early_z, char *why, size_t nwhy);
// The stage of key's translation (kept from this run or an earlier one with the same maxmetal and chain), or -1
int nvn_mm_stage(NSString *key);
// <key>_b's reflection and library (pc: the library name, <key>_b), nil when key has no translation. layer: the MSL writes render_target_array_index (as lib_from_file's t_lib_layer)
NSDictionary *nvn_mm_json(NSString *pc);
id<MTLLibrary> nvn_mm_library(id<MTLDevice> dev, NSString *pc, int *layer, NSError **err);
// Translations and library compiles this run, and their time
void nvn_mm_stats(uint64_t *translated, uint64_t *translate_ns, uint64_t *compiled, uint64_t *compile_ns);
