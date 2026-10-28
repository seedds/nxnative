// Runtime game profile: every constant the host used to take from game_profile.h at compile time, read once at start.
// Loading order: the compiled profile (game_profile.h, the macros) → the file DM_PROFILE names (the key/value form of a
// game profile, dm/games/game.py profile <id> <out.tsv>) → DM_PROF_<KEY> environment variables.
// The per-knob switches the host already reads (DM_FPS_TARGET, DM_OPMODE, DM_PRESENT_INTERVAL, DM_NVN_API, ...) still win over
// all of these: each site reads them after the profile.
// A build with no DM_PROFILE set behaves exactly as the compile-time profile did.
#pragma once
#include <stdint.h>

#define DM_PROF_MAX_MOD 8
#define DM_PROF_MAX_VER 64
#define DM_PROF_STR 128

struct dm_profile {
    char id[24];                 // base game title ID, hex text
    char name[128];
    char romfs_marker[256];
    uint64_t program_id;
    char modbuf[DM_PROF_MAX_MOD][16];
    const char *modules[DM_PROF_MAX_MOD];   // point into modbuf
    int nmod;
    uint64_t mem_reserve, mem_alias, mem_stack, app_pool, mem_used, sys_resource;
    int mem_map_limit;
    struct { char build[40], ver[40]; } ver[DM_PROF_MAX_VER];   // display version by main build ID
    int nver;
    int present_display, core_preempt, alloc_big_align, fps_target, fps_hold, present_snap, pso_mtl4;
    int shared_font_ttf;         // -1 = decide by the SDK version (hle_sdk16.c font_skip)
    int pad_layout, opmode, perf_boost;
    char feedback_snap[DM_PROF_STR];
    int tex_flush_upload, render_cond, tex_dirty_upload, tex_dyn_slots, gpu_ts_segment, vconv_range;
    int nvn_api_major, nvn_api_minor;   // major 0 = decide by the SDK version (hle_nvn.c nvn_api)
    char args[512], genv[1024];  // game_args / game_env
    char source[512];            // DM_PROFILE, or "compiled defaults"
};

const struct dm_profile *dm_profile(void);   // loaded once; never NULL
void dm_profile_log(void);                   // one line per differing value (called after loading)
