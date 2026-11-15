// The game profile of a run, for the port's host (Linux and Windows; dm/host/CMakeLists.txt): one nxhost-run for every profile. game_profile.h's runtime form
// (dm/games/game.py header --runtime) makes every DM_GAME_* a read of dm_game's field; this file picks the profile at start. Apple's hosts are compiled for one profile
// (game.py header <id>) and never build this file.
// Where the profile comes from, in this order (dm_game_profile_select, called first thing by main_x86.c's main):
//   1. a profile file: nxhost-run --profile <file>, else the environment switch DM_GAME_PROFILE=<file>. Written by game.py runtime <id> <file>: the constants of any profile
//      game.py finds, a derived or provisional one through DM_GAMES_DIR too, so such a profile runs without compiling anything;
//   2. the table of every profile in dm/games compiled into the program (game.py table, gen/profile/game_profiles.c): nxhost-run --game <title ID>;
//   3. the profile the tree was configured with (cmake -DGAME=<id>): dm_game's initial value, so a program or test that never calls this runs as before.
// A file and --game together are refused: the run would otherwise depend on which one wins. The choice and its source are one log line. A file with an unknown key, a missing
// key, a key given twice or a malformed value, an unknown title ID and an option without its value each stop the start with a [RED] line naming it: no constant falls back to a
// default the profile did not choose
#include "dm.h"
#include <ctype.h>
#include <stddef.h>

#if !DM_GAME_RUNTIME
#error "game_rt.c reads the game profile at run time: game_profile.h must be the runtime form (dm/games/game.py header --runtime)"
#endif

extern const struct dm_game_profile dm_game_profiles[];   // game.py table: the registered profiles, then the default when it is not one of them
extern const int dm_game_nprofiles, dm_game_default;
extern const char dm_game_default_from[];                 // the default's folder when it is not in dm/games (cmake -DGAME of a DM_GAMES_DIR profile), else empty

enum { K_STR, K_U64, K_FLAG, K_FPS, K_PAD, K_OPTFLAG, K_OPTINT, K_MODULES, K_VERSIONS };
static const struct field { const char *key; int kind; size_t off; } k_fields[] = {
#define F(name, kind) {#name, K_##kind, offsetof(struct dm_game_profile, name)},
    DM_GAME_FIELDS(F)
#undef F
};
enum { NFIELDS = sizeof k_fields / sizeof *k_fields };
#define XSTR_(x) #x
#define XSTR(x) XSTR_(x)

static int parse_u64(const char *s, uint64_t *out) {   // 0x<hex> or decimal digits, nothing else (no sign, no blanks); overflow refused
    uint64_t v = 0; int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    if (!*s) return -1;
    for (; *s; s++) {
        int d = isdigit((unsigned char)*s) ? *s - '0' : base == 16 && isxdigit((unsigned char)*s) ? tolower((unsigned char)*s) - 'a' + 10 : -1;
        if (d < 0 || v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return -1;
        v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = v; return 0;
}
static int parse_small(const char *s, int *out) {   // a decimal number below 65536
    if (!*s || strlen(s) > 5) return -1;
    for (const char *p = s; *p; p++) if (!isdigit((unsigned char)*p)) return -1;
    long v = strtol(s, NULL, 10); if (v > 65535) return -1;
    *out = (int)v; return 0;
}
static char *next_word(char **s) {   // the next space-separated word of *s (the string is cut), or NULL
    char *p = *s; while (*p == ' ') p++;
    if (!*p) { *s = p; return NULL; }
    char *w = p; while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *s = p; return w;
}
static int is_id(const char *s) {   // a profile's name: the base game's title ID, 16 hex digits
    if (strlen(s) != 16) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}
static int same_id(const char *a, const char *b) { for (; *a && toupper((unsigned char)*a) == toupper((unsigned char)*b); a++, b++) {} return !*a && !*b; }

// One value into its field; 0, or -1 with what is wrong in why
static int set_field(struct dm_game_profile *p, const struct field *f, char *v, const char **why) {
    void *at = (char *)p + f->off;
    int n;
    switch (f->kind) {
    case K_STR: *(const char **)at = v; return 0;
    case K_U64: if (parse_u64(v, (uint64_t *)at)) { *why = "a number: 0x<hex> or decimal digits, below 2^64"; return -1; } return 0;
    case K_FLAG: case K_FPS: case K_PAD:
        if (parse_small(v, &n) || (f->kind == K_FLAG && n > 1) || (f->kind == K_FPS && n != 0 && n != 30 && n != 60) || (f->kind == K_PAD && n > 2)) {
            *why = f->kind == K_FLAG ? "0 or 1" : f->kind == K_FPS ? "0, 30 or 60" : "0, 1 or 2 (simple, full, touch)"; return -1;
        }
        *(int *)at = n; return 0;
    case K_OPTFLAG: case K_OPTINT:
        if (!strcmp(v, "-")) { *(int *)at = -1; return 0; }
        if (parse_small(v, &n) || (f->kind == K_OPTFLAG && n > 1)) { *why = f->kind == K_OPTFLAG ? "0, 1 or - (not set)" : "a decimal number below 65536, or - (not set)"; return -1; }
        *(int *)at = n; return 0;
    case K_MODULES: {
        const char **m = (const char **)at; n = 0;
        for (char *w; (w = next_word(&v)); n++) {
            if (n == DM_GAME_MAX_MODULES) { *why = "at most " XSTR(DM_GAME_MAX_MODULES) " module names (game.py MAX_MODULES)"; return -1; }
            m[n] = w;
        }
        if (!n) { *why = "one module name or more, separated by spaces"; return -1; }
        p->nmod = n; return 0;
    }
    case K_VERSIONS: {
        struct dm_game_version *ver = at; n = 0;
        for (char *w; (w = next_word(&v)); n++) {
            char *eq = strchr(w, '=');
            if (n == DM_GAME_MAX_VERSIONS) { *why = "at most " XSTR(DM_GAME_MAX_VERSIONS) " <main build ID>=<display version> pairs (game.py MAX_VERSIONS)"; return -1; }
            if (!eq || eq == w || !eq[1]) { *why = "<main build ID>=<display version> pairs, separated by spaces"; return -1; }
            *eq = 0; ver[n].build = w; ver[n].ver = eq + 1;
        }
        p->nversions = n; return 0;
    }
    }
    *why = "?"; return -1;
}

// Reads a profile file (game.py runtime) into *p: key<TAB>value rows, # lines and blank lines skipped, a CR before the line end allowed. 0, or -1 with the reason in err
int dm_game_profile_read(const char *path, struct dm_game_profile *p, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "cannot open it: %s", strerror(errno)); return -1; }
    size_t cap = 1 << 16, len = 0; char *text = malloc(cap + 1);
    for (size_t got; text && (got = fread(text + len, 1, cap - len, f)) > 0; ) {
        len += got;
        if (len == cap) { if (cap >= (1u << 24)) { free(text); fclose(f); snprintf(err, errn, "larger than 16 MiB: not a profile file"); return -1; } cap *= 2; text = realloc(text, cap + 1); }
    }
    fclose(f);
    if (!text) { snprintf(err, errn, "out of memory"); return -1; }
    text[len] = 0;   // kept: the strings of the profile point into it for the rest of the run
    memset(p, 0, sizeof *p);
    int line_of[NFIELDS] = {0}, line = 0;
    for (char *s = text, *next; s && *s; s = next) {
        line++;
        next = strchr(s, '\n'); if (next) *next++ = 0;
        size_t n = strlen(s); if (n && s[n - 1] == '\r') s[--n] = 0;
        if (!n || s[0] == '#') continue;
        char *tab = strchr(s, '\t');
        if (!tab) { snprintf(err, errn, "line %d has no tab: the rows are key<TAB>value", line); return -1; }
        *tab = 0;
        int k = 0; while (k < NFIELDS && strcmp(k_fields[k].key, s)) k++;
        if (k == NFIELDS) { snprintf(err, errn, "unknown key '%s' (line %d)", s, line); return -1; }
        if (line_of[k]) { snprintf(err, errn, "key '%s' given twice (lines %d and %d)", s, line_of[k], line); return -1; }
        line_of[k] = line;
        const char *why = NULL;
        if (set_field(p, &k_fields[k], tab + 1, &why)) { snprintf(err, errn, "key '%s' (line %d): malformed value: %s expected", s, line, why); return -1; }
    }
    char missing[1024] = ""; int nmiss = 0;
    for (int k = 0; k < NFIELDS; k++) if (!line_of[k]) { size_t m = strlen(missing); snprintf(missing + m, sizeof missing - m, "%s'%s'", nmiss++ ? ", " : "", k_fields[k].key); }
    if (nmiss) { snprintf(err, errn, "missing key%s %s (the file must give every constant: game.py runtime writes them all)", nmiss > 1 ? "s" : "", missing); return -1; }
    // What game.py checks across keys (constants())
    if (!is_id(p->id)) { snprintf(err, errn, "key 'id': malformed value '%s': the base game's title ID, 16 hex digits, expected", p->id); return -1; }
    if (p->tex_dirty_upload && !p->tex_flush_upload) { snprintf(err, errn, "key 'tex_dirty_upload': 1 needs tex_flush_upload 1"); return -1; }
    if (p->pad_layout == 2 && p->opmode) { snprintf(err, errn, "key 'pad_layout': 2 (touch) needs opmode 0 (handheld: the touch screen reaches the game only then)"); return -1; }
    if ((p->nvn_api_major < 0) != (p->nvn_api_minor < 0)) { snprintf(err, errn, "key 'nvn_api_minor': nvn_api_major and nvn_api_minor are both set or both -"); return -1; }
    return 0;
}

static void say(const char *how) { LOG("game profile %s (%s): %s", dm_game.id, dm_game.name, how); }

// Takes --profile <file> and --game <title ID> from the front of the arguments (*argc and *argv are moved past them: argv[1] is then the module folder as before), picks the
// profile into dm_game and logs it. 0, or -1 after a [RED] line: the caller stops with its usage status
int dm_game_profile_select(int *argc, char ***argv) {
    const char *file = NULL, *game = NULL, *from = NULL;
    int i = 1;
    while (i < *argc && (!strcmp((*argv)[i], "--profile") || !strcmp((*argv)[i], "--game"))) {
        const char *opt = (*argv)[i], **slot = !strcmp(opt, "--profile") ? &file : &game;
        if (i + 1 >= *argc || !*(*argv)[i + 1]) { LOG("[RED] %s needs a value: --profile <file> (game.py runtime writes it) or --game <title ID>", opt); return -1; }
        if (*slot) { LOG("[RED] %s given twice: %s and %s", opt, *slot, (*argv)[i + 1]); return -1; }
        *slot = (*argv)[i + 1]; i += 2;
    }
    if (i > 1) { (*argv)[i - 1] = (*argv)[0]; *argv += i - 1; *argc -= i - 1; }
    if (file) from = "--profile";
    else { const char *e = getenv("DM_GAME_PROFILE"); if (e && *e) { file = e; from = "DM_GAME_PROFILE"; } }
    if (file && game) { LOG("[RED] --game %s and the profile file %s (%s) both name the game profile: give one", game, file, from); return -1; }
    char how[1200];
    if (file) {
        struct dm_game_profile p; char err[1100];
        if (dm_game_profile_read(file, &p, err, sizeof err)) { LOG("[RED] game profile file %s (%s): %s", file, from, err); return -1; }
        dm_game = p;
        snprintf(how, sizeof how, "the file %s (%s)", file, from);
    } else if (game) {
        int k = 0; while (k < dm_game_nprofiles && !same_id(dm_game_profiles[k].id, game)) k++;
        if (k == dm_game_nprofiles && same_id(dm_game_profiles[dm_game_default].id, game)) k = dm_game_default;   // the default from outside dm/games is in the program too
        else if (k == dm_game_nprofiles) {
            LOG("[RED] --game %s: no such game profile among the %d built into this program (dm/games; game.py list names them); a profile outside dm/games runs from its "
                "file: game.py runtime <id> <file>, then --profile <file>", game, dm_game_nprofiles);
            return -1;
        }
        dm_game = dm_game_profiles[k];
        snprintf(how, sizeof how, "the table built into the program (--game)");
    } else {
        dm_game = dm_game_profiles[dm_game_default];
        if (*dm_game_default_from) snprintf(how, sizeof how, "the default the program was configured with (cmake -DGAME; the profile in %s)", dm_game_default_from);
        else snprintf(how, sizeof how, "the default the program was configured with (cmake -DGAME)");
    }
    say(how);
    return 0;
}
