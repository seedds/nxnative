#include <strings.h>

// Gothic II reads separate text and voice language settings from gothic.ini.
static void gothic_apply_language(const char *mount_name, const char *work) {
    if (strcmp(mount_name, "savegame") ||
        strcmp(dm_main_build(), "73c19a6aaead08e517b6553357a5260b")) return;

    // main+0x10d7d0 indexes the game's eight-entry text-language table at main+0x67fe30.
    const char *lang = getenv("DM_LANG");
    static const struct { const char *code, *scripts; int value; } languages[] = {
        {"en-US", "EN", 0}, {"en-GB", "EN", 0}, {"de", "DE", 1}, {"ru", "RU", 3},
        {"fr", "FR", 4}, {"fr-CA", "FR", 4}, {"es", "ES", 5}, {"es-419", "ES", 5}, {"it", "IT", 6},
    };
    int text = -1;
    for (size_t i = 0; lang && i < sizeof languages / sizeof languages[0]; i++)
        if (!strcmp(lang, languages[i].code)) {
            char menu[96];
            snprintf(menu, sizeof menu, "gothic2/_work/DATA/SCRIPTS_%s/_COMPILED/MENU.DAT", languages[i].scripts);
            if (gothic_has_file(menu)) text = languages[i].value;
            break;
        }
    if (text < 0) return;

    // The English, German, Polish and Russian archives have distinct on-disc spellings.
    static const char *const speech[][3] = {
        {"en/Speech1.NX.vdf", "en/Speech2.NX.vdf", "en/Speech_Addon.NX.vdf"},
        {"de/Speech1.NX.VDF", "de/Speech2.NX.VDF", "de/Speech_Addon.NX.VDF"},
        {"pl/speech1.NX.VDF", "pl/speech2.NX.VDF", "pl/speech_addon.NX.VDF"},
        {"ru/speech1.NX.VDF", "ru/speech2.NX.VDF", "ru/speech_addon.NX.VDF"},
    };
    int available[4] = {0}, voice = -1;
    for (int i = 0; i < 4; i++) {
        available[i] = 1;
        for (int j = 0; j < 3; j++) {
            char path[96];
            snprintf(path, sizeof path, "gothic2/Data/%s", speech[i][j]);
            available[i] &= gothic_has_file(path);
        }
        if (available[i] && voice < 0) voice = i;
    }
    int preferred_voice = text < 4 && available[text] ? text : 0;
    int has_preferred_voice = available[preferred_voice];
    if (has_preferred_voice) voice = preferred_voice;

    char path[1200], temp[1200];
    snprintf(path, sizeof path, "%s/gothic.ini", work);
    FILE *old = fopen(path, "r");
    if (!old && errno != ENOENT) {
        LOG("[RED] cannot read Gothic II's language settings: %s", strerror(errno));
        return;
    }

    // Apply the launcher choice in the writable transaction. The game commits it.
    snprintf(temp, sizeof temp, "%s/.gothic-ini.XXXXXX", work);
    int fd = mkstemp(temp);
    FILE *out = fd < 0 ? NULL : fdopen(fd, "w");
    if (!out) {
        if (fd >= 0) { close(fd); unlink(temp); }
        if (old) fclose(old);
        LOG("[RED] cannot prepare Gothic II's language settings");
        return;
    }
    int ok = 1;
    if (!old) {
        ok = fprintf(out, "[GAME]\r\nlanguage=%d\r\n", text) > 0;
        if (ok && voice >= 0) ok = fprintf(out, "voice=%d\r\n", voice) > 0;
    } else {
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        int in_game = 0, has_game = 0, has_language = 0, has_voice = 0, changed = 0;
        while ((len = getline(&line, &cap, old)) >= 0 && ok) {
            const char *key = line + strspn(line, " \t");
            if (*key == '[') {
                if (in_game && (!has_language || (!has_voice && voice >= 0))) {
                    if (!has_language) ok = fprintf(out, "language=%d\r\n", text) > 0;
                    if (ok && !has_voice && voice >= 0) ok = fprintf(out, "voice=%d\r\n", voice) > 0;
                    changed = 1;
                }
                in_game = !strncasecmp(key, "[GAME]", 6);
                has_game |= in_game;
                has_language = has_voice = 0;
                if (!ok) break;
            }
            int lang_key = in_game && !strncasecmp(key, "language", 8);
            int voice_key = in_game && !strncasecmp(key, "voice", 5);
            if (lang_key || voice_key) {
                const char *eq = key + (lang_key ? 8 : 5);
                eq += strspn(eq, " \t");
                if (*eq == '=') {
                    if (lang_key) has_language = 1;
                    else has_voice = 1;
                    int value = lang_key ? text : voice;
                    char *end;
                    long previous = strtol(eq + 1, &end, 10);
                    int valid_previous = end != eq + 1;
                    int keep_voice = voice_key && (voice < 0 ||
                        (!has_preferred_voice && valid_previous && previous >= 0 && previous < 4 && available[previous]));
                    if (keep_voice || (valid_previous && previous == value)) {
                        ok = fwrite(line, 1, (size_t)len, out) == (size_t)len;
                    } else {
                        ok = fprintf(out, "%s=%d%s", lang_key ? "language" : "voice", value,
                                     len >= 2 && line[len - 2] == '\r' && line[len - 1] == '\n' ? "\r\n" : "\n") > 0;
                        changed = 1;
                    }
                    continue;
                }
            }
            ok = fwrite(line, 1, (size_t)len, out) == (size_t)len;
        }
        ok = ok && feof(old) && !ferror(old);
        if (ok && (!has_game || (in_game && (!has_language || (!has_voice && voice >= 0))))) {
            ok = fprintf(out, "\r\n%s", has_game ? "" : "[GAME]\r\n") > 0;
            if (ok && (!has_game || !has_language)) ok = fprintf(out, "language=%d\r\n", text) > 0;
            if (ok && voice >= 0 && (!has_game || !has_voice)) ok = fprintf(out, "voice=%d\r\n", voice) > 0;
            changed = 1;
        }
        free(line);
        fclose(old);
        if (ok && !changed) {
            fclose(out);
            unlink(temp);
            return;
        }
    }
    if (fclose(out)) ok = 0;
    if (!ok || rename(temp, path)) LOG("[RED] cannot apply Gothic II's language settings");
    unlink(temp);
}
