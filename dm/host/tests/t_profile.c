// dm_profile() returns the constants game_profile.h compiles in, field by field (docs/runtime-profile.md, step 1).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game_profile.h"
#include "hle_profile.h"

int main(void) {
    const struct dm_profile *p = dm_profile();
    assert(p && p == dm_profile());
    assert(!strcmp(p->id, DM_GAME_ID) && !strcmp(p->name, DM_GAME_NAME) && !strcmp(p->romfs_marker, DM_GAME_ROMFS_MARKER));
    assert(!strcmp(p->args, DM_GAME_ARGS) && !strcmp(p->genv, DM_GAME_ENV));
    assert(p->program_id == DM_GAME_PROGRAM_ID);
    assert(p->mem_reserve == DM_GAME_MEM_RESERVE && p->mem_alias == DM_GAME_MEM_ALIAS && p->mem_stack == DM_GAME_MEM_STACK);
    assert(p->app_pool == DM_GAME_APP_POOL && p->mem_used == DM_GAME_MEM_USED && p->sys_resource == DM_GAME_SYS_RESOURCE);
    assert(p->stdalloc_cap == DM_GAME_STDALLOC_CAP && p->mem_map_limit == DM_GAME_MEM_MAP_LIMIT);
    assert(p->present_display == DM_GAME_PRESENT_DISPLAY && p->core_preempt == DM_GAME_CORE_PREEMPT && p->alloc_big_align == DM_GAME_ALLOC_BIG_ALIGN);
    assert(p->fps_target == DM_GAME_FPS_TARGET && p->fps_hold == DM_GAME_FPS_HOLD && p->present_snap == DM_GAME_PRESENT_SNAP && p->pso_mtl4 == DM_GAME_PSO_MTL4);
#ifdef DM_GAME_SHARED_FONT_TTF
    assert(p->shared_font_ttf == DM_GAME_SHARED_FONT_TTF);
#else
    assert(p->shared_font_ttf == -1);
#endif
    assert(p->pad_layout == DM_GAME_PAD_LAYOUT && p->opmode == DM_GAME_OPMODE && p->perf_boost == DM_GAME_PERF_BOOST);
    assert(p->tex_flush_upload == DM_GAME_TEX_FLUSH_UPLOAD && p->render_cond == DM_GAME_RENDER_COND && p->tex_dirty_upload == DM_GAME_TEX_DIRTY_UPLOAD);
    assert(p->tex_dyn_slots == DM_GAME_TEX_DYN_SLOTS && p->gpu_ts_segment == DM_GAME_GPU_TS_SEGMENT && p->vconv_range == DM_GAME_VCONV_RANGE);
#ifdef DM_GAME_NVN_API_MAJOR
    assert(p->nvn_api_major == DM_GAME_NVN_API_MAJOR && p->nvn_api_minor == DM_GAME_NVN_API_MINOR);
#else
    assert(p->nvn_api_major == 0 && p->nvn_api_minor == 0);
#endif
    puts("t_profile: ok");
    return 0;
}
