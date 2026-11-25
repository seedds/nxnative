// The cheat engine's calls (dm/host/dm.h, hle_cheats.c) that the in-game Cheats screen (CheatsMenu.swift) uses; the declarations match dm.h's.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
enum { DM_CHEAT_LABEL = 1, DM_CHEAT_SECTION_START = 2, DM_CHEAT_SECTION_END = 4, DM_CHEAT_PAIR_ON = 8, DM_CHEAT_PAIR_OFF = 16,
       DM_CHEAT_ONCE = 32, DM_CHEAT_ASK = 64, DM_CHEAT_HOST = 128, DM_CHEAT_WEAK = 256 };
int dm_cheats_found(void);
int dm_cheats_ncheats(void);
const char *dm_cheats_build(void);
const char *dm_cheats_version(void);
const char *dm_cheats_dir_path(void);
int dm_cheats_nothers(void);
const char *dm_cheats_other(int i);
const char *dm_cheats_other_version(int i);
const char *dm_cheat_name_at(int i);
const char *dm_cheat_why_at(int i);
int dm_cheat_kind_at(int i);
int dm_cheat_on_at(int i);
void dm_cheat_toggle(int i);
void dm_cheats_reload(void);
int dm_cheat_flags_at(int i);
const char *dm_cheat_section_at(int i);
const char *dm_cheat_note_at(int i);
int dm_cheat_preview(int i, char *out, size_t cap);
int dm_cheat_value_get(int i, char *out, size_t cap);
int dm_cheat_value_set(int i, const char *text);
const char *dm_cheat_value_bounds(int i);
int dm_cheats_master(void);
void dm_cheats_master_set(int on);
#ifdef __cplusplus
}
#endif
