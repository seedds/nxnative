// Prompt selection with real temporary pack directories: explicit mods win, absent packs never change the game.
#include "../button_prompts.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char root[] = "/tmp/dm-prompts-XXXXXX"; assert(mkdtemp(root));
    char mods[1200], pack[1200], rom[1200], save[1200], out[1200];
    snprintf(mods, sizeof mods, "%s/Mods", root); assert(!mkdir(mods, 0700));
    snprintf(pack, sizeof pack, "%s/Mods/ButtonPrompts-PlayStation", root); assert(!mkdir(pack, 0700));
    snprintf(rom, sizeof rom, "%s/Mods/ButtonPrompts-PlayStation/romfs", root); assert(!mkdir(rom, 0700));
    snprintf(save, sizeof save, "%s/save/", root);
    assert(dm_prompt_overlay(NULL, DM_PROMPTS_PLAYSTATION, NULL, save, out, sizeof out) == DM_PROMPTS_PLAYSTATION);
    assert(!strcmp(out, rom));
    dm_prompt_overlay("auto", DM_PROMPTS_PLAYSTATION, NULL, save, out, sizeof out); assert(!strcmp(out, rom));
    dm_prompt_overlay("playstation", DM_PROMPTS_XBOX, NULL, save, out, sizeof out); assert(!strcmp(out, rom));
    dm_prompt_overlay("switch", DM_PROMPTS_PLAYSTATION, NULL, save, out, sizeof out); assert(!*out);
    dm_prompt_overlay(NULL, DM_PROMPTS_SWITCH, NULL, save, out, sizeof out); assert(!*out);
    dm_prompt_overlay(NULL, DM_PROMPTS_XBOX, NULL, save, out, sizeof out); assert(!*out);   // missing Xbox pack
    dm_prompt_overlay("steamdeck", DM_PROMPTS_PLAYSTATION, NULL, save, out, sizeof out); assert(!*out);
    for (int i = 0; i < 2; i++) {   // an explicit overlay list or an explicitly empty value wins over automatic selection
        assert(dm_prompt_overlay("playstation", DM_PROMPTS_PLAYSTATION, i ? "" : "/mods/a:/mods/b", save, out, sizeof out) == DM_PROMPTS_SWITCH);
        assert(!*out);
    }
    assert(dm_prompt_overlay("../other", DM_PROMPTS_PLAYSTATION, NULL, save, out, sizeof out) == DM_PROMPTS_INVALID); assert(!*out);
    dm_prompt_overlay("playstation", DM_PROMPTS_PLAYSTATION, NULL, NULL, out, sizeof out); assert(!*out);
    dm_prompt_overlay("playstation", DM_PROMPTS_PLAYSTATION, NULL, save, out, 2); assert(!*out);
    assert(!rmdir(rom)); FILE *f = fopen(rom, "w"); assert(f); fclose(f);   // a file is not a pack directory
    dm_prompt_overlay("playstation", DM_PROMPTS_PLAYSTATION, NULL, save, out, sizeof out); assert(!*out);
    assert(!unlink(rom)); assert(!rmdir(pack)); assert(!rmdir(mods)); assert(!rmdir(root));
    puts("t_button_prompts OK");
    return 0;
}
