// The compiled game profile behind dm_profile(); see hle_profile.h and docs/runtime-profile.md.
#include "game_profile.h"
#include "hle_profile.h"

static const struct dm_profile P = {
    .id = DM_GAME_ID, .name = DM_GAME_NAME, .romfs_marker = DM_GAME_ROMFS_MARKER, .args = DM_GAME_ARGS, .genv = DM_GAME_ENV,
    .program_id = DM_GAME_PROGRAM_ID,
    .mem_reserve = DM_GAME_MEM_RESERVE, .mem_alias = DM_GAME_MEM_ALIAS, .mem_stack = DM_GAME_MEM_STACK, .app_pool = DM_GAME_APP_POOL,
    .mem_used = DM_GAME_MEM_USED, .sys_resource = DM_GAME_SYS_RESOURCE, .stdalloc_cap = DM_GAME_STDALLOC_CAP,
    .mem_map_limit = DM_GAME_MEM_MAP_LIMIT,
    .present_display = DM_GAME_PRESENT_DISPLAY, .core_preempt = DM_GAME_CORE_PREEMPT, .alloc_big_align = DM_GAME_ALLOC_BIG_ALIGN,
    .fps_target = DM_GAME_FPS_TARGET, .fps_hold = DM_GAME_FPS_HOLD, .present_snap = DM_GAME_PRESENT_SNAP, .pso_mtl4 = DM_GAME_PSO_MTL4,
#ifdef DM_GAME_SHARED_FONT_TTF
    .shared_font_ttf = DM_GAME_SHARED_FONT_TTF,
#else
    .shared_font_ttf = -1,
#endif
    .pad_layout = DM_GAME_PAD_LAYOUT, .opmode = DM_GAME_OPMODE, .perf_boost = DM_GAME_PERF_BOOST,
    .tex_flush_upload = DM_GAME_TEX_FLUSH_UPLOAD, .render_cond = DM_GAME_RENDER_COND, .tex_dirty_upload = DM_GAME_TEX_DIRTY_UPLOAD,
    .tex_dyn_slots = DM_GAME_TEX_DYN_SLOTS, .gpu_ts_segment = DM_GAME_GPU_TS_SEGMENT, .vconv_range = DM_GAME_VCONV_RANGE,
#ifdef DM_GAME_NVN_API_MAJOR
    .nvn_api_major = DM_GAME_NVN_API_MAJOR, .nvn_api_minor = DM_GAME_NVN_API_MINOR,
#endif
};

const struct dm_profile *dm_profile(void) { return &P; }
