// Select a user-installed, per-game prompt overlay. No icon assets are supplied or changed here.
#pragma once
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef enum { DM_PROMPTS_INVALID = -1, DM_PROMPTS_SWITCH, DM_PROMPTS_PLAYSTATION, DM_PROMPTS_XBOX, DM_PROMPTS_STEAMDECK } dm_prompt_style;

static inline const char *dm_prompt_name(dm_prompt_style s) {
    switch (s) {
    case DM_PROMPTS_SWITCH: return "switch";
    case DM_PROMPTS_PLAYSTATION: return "playstation";
    case DM_PROMPTS_XBOX: return "xbox";
    case DM_PROMPTS_STEAMDECK: return "steamdeck";
    default: return "invalid";
    }
}
static inline dm_prompt_style dm_prompt_choice(const char *choice, dm_prompt_style detected) {
    if (!choice || !*choice || !strcmp(choice, "auto")) return detected;
    for (int i = DM_PROMPTS_SWITCH; i <= DM_PROMPTS_STEAMDECK; i++)
        if (!strcmp(choice, dm_prompt_name((dm_prompt_style)i))) return (dm_prompt_style)i;
    return DM_PROMPTS_INVALID;
}

// An explicit ROMFS_OVERLAY, even an empty one, wins. Missing packs leave out empty: the caller keeps the game's own files.
// Packs are under the save's parent, as pso_user.tsv is: <game data>/Mods/ButtonPrompts-<family>/romfs/.
static inline dm_prompt_style dm_prompt_overlay(const char *choice, dm_prompt_style detected, const char *explicit_overlay,
                                               const char *save, char *out, size_t size) {
    if (size) out[0] = 0;
    if (explicit_overlay) return DM_PROMPTS_SWITCH;
    dm_prompt_style s = dm_prompt_choice(choice, detected);
    if (s <= DM_PROMPTS_SWITCH || !save || !*save || !size) return s;
    const char *folder = s == DM_PROMPTS_PLAYSTATION ? "PlayStation" : s == DM_PROMPTS_XBOX ? "Xbox" : "SteamDeck";
    char parent[1024]; size_t n = strlen(save);
    if (n >= sizeof parent) return s;
    memcpy(parent, save, n + 1);
    while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    n = strlen(parent);
    int len = snprintf(out, size, "%s%sMods/ButtonPrompts-%s/romfs", parent, parent[n - 1] == '/' ? "" : "/", folder);
    struct stat st;
    if (len < 0 || (size_t)len >= size || stat(out, &st) || !S_ISDIR(st.st_mode)) out[0] = 0;
    return s;
}
