// The game profile as the host reads it: one struct, one accessor. Step 1 of docs/runtime-profile.md: the values are the ones
// game_profile.h compiles in (generated from game.tsv), so dm_profile() changes nothing. A later step lets a profile file fill the
// same struct at start-up, and the call sites that read DM_GAME_* move to dm_profile()->field one subsystem at a time.
#pragma once
#include <stdint.h>

struct dm_profile {
    const char *id, *name, *romfs_marker, *args, *genv;   // title ID, display name, RomFS marker path, game_args, game_env
    uint64_t program_id;
    uint64_t mem_reserve, mem_alias, mem_stack, app_pool, mem_used, sys_resource, stdalloc_cap;
    int mem_map_limit;
    int present_display, core_preempt, alloc_big_align, fps_target, fps_hold, present_snap, pso_mtl4;
    int shared_font_ttf;   // -1 = decided by the SDK version (hle_sdk16.c)
    int pad_layout, opmode, perf_boost;
    int tex_flush_upload, render_cond, tex_dirty_upload, tex_dyn_slots, gpu_ts_segment, vconv_range;
    int nvn_api_major, nvn_api_minor;   // major 0 = decided by the SDK version (hle_nvn.c nvn_api)
};

const struct dm_profile *dm_profile(void);   // never NULL; the same pointer for the life of the process
