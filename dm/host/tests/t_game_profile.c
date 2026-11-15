// The game profile of a run on the port (dm/host/game_rt.c; game_profile.h's runtime form, dm/games/game.py header --runtime): one host program for every profile.
//   1. Every profile of dm/games read back from the file game.py runtime writes for it (<build>/gen/profile/files/<id>.tsv, game.py runtime --all) equals its row of the
//      table compiled in (game.py table), field by field: the two outputs of game.py and the reader agree on every registered profile's data;
//   2. dm_game starts as the default's row (cmake -DGAME), before anything picks a profile;
//   3. the reader's refusals, each naming the key: an unknown key, a missing key, a key given twice, a row without a tab, malformed numbers, flags and enumerations, a list
//      beyond its bound, the cross-key rules game.py checks; and what it accepts: CR line ends, an empty build list, the optional constants unset;
//   4. dm_game_profile_select: no option (the default), --game in lower case, an unknown --game, --profile, DM_GAME_PROFILE, a file and --game together, an option without its
//      value or given twice, a bad file: the arguments after the options as main sees them.
// The files are found beside the test (argv[0]'s folder): the tree's gen/profile/files. The temporary files go to TMPDIR (CTest and the runners set it)
//   t_game_profile
#include "dm.h"
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>

int dm_game_profile_read(const char *path, struct dm_game_profile *p, char *err, size_t n);   // game_rt.c
int dm_game_profile_select(int *argc, char ***argv);
extern const struct dm_game_profile dm_game_profiles[];
extern const int dm_game_nprofiles, dm_game_default;

static int checks, fails;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...) {
    va_list a; va_start(a, fmt); checks++; fails += !ok;
    printf("%-5s ", ok ? "ok" : "FAIL"); vprintf(fmt, a); putchar('\n'); va_end(a);
}

// Field by field over the generated list of fields (DM_GAME_FIELDS): 1 when equal, else 0 with the first difference in diff
static int same(const struct dm_game_profile *a, const struct dm_game_profile *b, char *diff, size_t n) {
    *diff = 0;
#define CMP_STR(f) if (strcmp(a->f, b->f)) { snprintf(diff, n, #f ": '%s' against '%s'", a->f, b->f); return 0; }
#define CMP_U64(f) if (a->f != b->f) { snprintf(diff, n, #f ": %#llx against %#llx", (unsigned long long)a->f, (unsigned long long)b->f); return 0; }
#define CMP_INT(f) if (a->f != b->f) { snprintf(diff, n, #f ": %d against %d", a->f, b->f); return 0; }
#define CMP_FLAG CMP_INT
#define CMP_FPS CMP_INT
#define CMP_PAD CMP_INT
#define CMP_OPTFLAG CMP_INT
#define CMP_OPTINT CMP_INT
#define CMP_MODULES(f) CMP_INT(nmod) for (int i = 0; i < a->nmod; i++) CMP_STR(f[i])
#define CMP_VERSIONS(f) CMP_INT(nversions) for (int i = 0; i < a->nversions; i++) { CMP_STR(f[i].build) CMP_STR(f[i].ver) }
#define X(name, kind) CMP_##kind(name)
    DM_GAME_FIELDS(X)
#undef X
    return 1;
}

static char g_tmp[1024];
static const char *write_file(const char *name, const char *text) {
    static char path[1200];
    snprintf(path, sizeof path, "%s/%s", g_tmp, name);
    FILE *f = fopen(path, "wb"); if (!f) { printf("FAIL cannot write %s\n", path); exit(1); }
    fputs(text, f); fclose(f);
    return path;
}
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1); if (fread(s, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s); return NULL; }
    fclose(f); s[n] = 0; return s;
}
// base with the row of key replaced by row (NULL: the row removed); a key base lacks: row appended
static char *with_row(const char *base, const char *key, const char *row) {
    size_t kl = strlen(key), cap = strlen(base) + (row ? strlen(row) : 0) + 4;
    char *out = malloc(cap); out[0] = 0; int done = 0;
    for (const char *s = base; *s; ) {
        const char *e = strchr(s, '\n'); size_t len = e ? (size_t)(e - s) + 1 : strlen(s);
        if (!done && !strncmp(s, key, kl) && s[kl] == '\t') { if (row) { strcat(out, row); strcat(out, "\n"); } done = 1; }
        else strncat(out, s, len);
        s += len;
    }
    if (!done && row) { strcat(out, row); strcat(out, "\n"); }
    return out;
}
// The reader on text: expect_ok, or a refusal whose reason holds every one of want (separated by |)
static void expect(const char *what, const char *text, int expect_ok, const char *want) {
    struct dm_game_profile p; char err[1200] = "";
    int r = dm_game_profile_read(write_file("case.tsv", text), &p, err, sizeof err);
    int ok = expect_ok ? r == 0 : r != 0;
    if (ok && !expect_ok && want) {
        char w[300]; snprintf(w, sizeof w, "%s", want);
        for (char *t = strtok(w, "|"); t; t = strtok(NULL, "|")) if (!strstr(err, t)) ok = 0;
    }
    check(ok, "%s: %s%s", what, r ? "refused: " : expect_ok ? "read" : "read, not refused", r ? err : "");
}

int main(int argc, char **argv) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    char dir[1024], path[1300];
    snprintf(dir, sizeof dir, "%s", argv[0]);
    char *slash = strrchr(dir, '/');
#if defined(_WIN32)
    char *bs = strrchr(dir, '\\'); if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    const char *t = getenv("TMPDIR"); snprintf(g_tmp, sizeof g_tmp, "%s", t && *t ? t : ".");
    const struct dm_game_profile *def = &dm_game_profiles[dm_game_default];
    char diff[600];

    // 2. The default before anything picks a profile
    check(same(&dm_game, def, diff, sizeof diff), "dm_game starts as the default's row, %s (%s)%s%s", def->id, def->name, *diff ? ": " : "", diff);

    // 1. Every registered profile: its file read back equals its row
    int read_ok = 0, equal = 0;
    for (int i = 0; i < dm_game_nprofiles; i++) {
        const struct dm_game_profile *row = &dm_game_profiles[i];
        snprintf(path, sizeof path, "%s/gen/profile/files/%s.tsv", dir, row->id);
        struct dm_game_profile p; char err[1200];
        if (dm_game_profile_read(path, &p, err, sizeof err)) { printf("FAIL %s: %s\n", path, err); continue; }
        read_ok++;
        if (same(&p, row, diff, sizeof diff)) equal++; else printf("FAIL %s (%s): the file and the table differ: %s\n", row->id, row->name, diff);
    }
    check(dm_game_nprofiles > 1 && read_ok == dm_game_nprofiles && equal == dm_game_nprofiles,
          "every profile of dm/games: %d of %d files read, %d equal to their table row field by field (game.py runtime --all, game.py table)", read_ok, dm_game_nprofiles, equal);

    // 3. The reader's refusals and what it accepts, on the default's file changed one row at a time
    snprintf(path, sizeof path, "%s/gen/profile/files/%s.tsv", dir, def->id);
    char *base = read_text(path);
    if (!base) { printf("FAIL no %s (game.py runtime --all)\n", path); return 1; }
    expect("the default's file as written", base, 1, NULL);
    expect("an unknown key", with_row(base, "zzz", "bogus_key\t1"), 0, "unknown key 'bogus_key'");
    expect("a missing key", with_row(base, "mem_reserve", NULL), 0, "missing key 'mem_reserve'");
    expect("two missing keys", with_row(with_row(base, "mem_reserve", NULL), "opmode", NULL), 0, "missing keys|'mem_reserve'|'opmode'");
    expect("a key given twice", with_row(base, "zzz", "name\tAnother"), 0, "key 'name' given twice");
    expect("a row without a tab", with_row(base, "zzz", "name Another"), 0, "has no tab");
    expect("a malformed number", with_row(base, "mem_reserve", "mem_reserve\t0x14000000g"), 0, "key 'mem_reserve'|malformed");
    expect("a number with a sign", with_row(base, "app_pool", "app_pool\t-5"), 0, "key 'app_pool'|malformed");
    expect("a number beyond 64 bits", with_row(base, "mem_alias", "mem_alias\t0x10000000000000000"), 0, "key 'mem_alias'|malformed");
    expect("an empty number", with_row(base, "mem_stack", "mem_stack\t"), 0, "key 'mem_stack'|malformed");
    expect("a decimal number", with_row(base, "mem_used", "mem_used\t67108864"), 1, NULL);
    expect("a flag of 2", with_row(base, "mem_map_limit", "mem_map_limit\t2"), 0, "key 'mem_map_limit'|0 or 1");
    expect("fps_target 45", with_row(base, "fps_target", "fps_target\t45"), 0, "key 'fps_target'|0, 30 or 60");
    expect("pad_layout 3", with_row(base, "pad_layout", "pad_layout\t3"), 0, "key 'pad_layout'");
    expect("shared_font_ttf 2", with_row(base, "shared_font_ttf", "shared_font_ttf\t2"), 0, "key 'shared_font_ttf'");
    expect("shared_font_ttf 1", with_row(base, "shared_font_ttf", "shared_font_ttf\t1"), 1, NULL);
    expect("nvn_api_major set and nvn_api_minor not", with_row(with_row(base, "nvn_api_major", "nvn_api_major\t55"), "nvn_api_minor", "nvn_api_minor\t-"), 0, "nvn_api_minor");
    expect("nvn_api 53.313", with_row(with_row(base, "nvn_api_major", "nvn_api_major\t53"), "nvn_api_minor", "nvn_api_minor\t313"), 1, NULL);
    expect("no module", with_row(base, "modules", "modules\t"), 0, "key 'modules'");
    expect("17 modules", with_row(base, "modules", "modules\tm1 m2 m3 m4 m5 m6 m7 m8 m9 m10 m11 m12 m13 m14 m15 m16 m17"), 0, "key 'modules'|at most 16");
    expect("16 modules", with_row(base, "modules", "modules\tm1 m2 m3 m4 m5 m6 m7 m8 m9 m10 m11 m12 m13 m14 m15 m16"), 1, NULL);
    {
        char row[2000] = "display_versions\t";
        for (int i = 0; i < 33; i++) { size_t n = strlen(row); snprintf(row + n, sizeof row - n, "%s%032x=1.0.%d", i ? " " : "", i, i); }
        expect("33 build rows", with_row(base, "display_versions", row), 0, "key 'display_versions'|at most 32");
    }
    expect("a build row without its display version", with_row(base, "display_versions", "display_versions\tcd6e42aee7934f4d8393cd43893ae25e"), 0, "key 'display_versions'");
    expect("no build row", with_row(base, "display_versions", "display_versions\t"), 1, NULL);
    expect("an id that is no title ID", with_row(base, "id", "id\tmario"), 0, "key 'id'");
    expect("tex_dirty_upload without tex_flush_upload", with_row(with_row(base, "tex_dirty_upload", "tex_dirty_upload\t1"), "tex_flush_upload", "tex_flush_upload\t0"), 0, "tex_dirty_upload");
    expect("pad_layout touch in console mode", with_row(with_row(base, "pad_layout", "pad_layout\t2"), "opmode", "opmode\t1"), 0, "pad_layout");
    {   // CR line ends (a file edited on Windows)
        size_t n = strlen(base), k = 0; char *crlf = malloc(2 * n + 1);
        for (size_t i = 0; i < n; i++) { if (base[i] == '\n') crlf[k++] = '\r'; crlf[k++] = base[i]; }
        crlf[k] = 0;
        expect("CR line ends", crlf, 1, NULL);
    }
    {   // a value read as written: a changed name and memory size, an empty game_args kept empty
        struct dm_game_profile p; char err[1200];
        int r = dm_game_profile_read(write_file("changed.tsv", with_row(with_row(base, "name", "name\tA changed profile"), "mem_reserve", "mem_reserve\t0x150000000")), &p, err, sizeof err);
        check(!r && !strcmp(p.name, "A changed profile") && p.mem_reserve == 0x150000000ull && p.nmod == def->nmod, "a changed file's values as written: %s, %#llx", r ? err : p.name, (unsigned long long)p.mem_reserve);
    }
    check(same(&dm_game, def, diff, sizeof diff), "dm_game is still the default's after the reader's cases");

    // 4. The selection, as main calls it
    const char *good = write_file("good.tsv", with_row(base, "name", "name\tSelected from a file"));
    const struct dm_game_profile *row0 = &dm_game_profiles[0 == dm_game_default && dm_game_nprofiles > 1 ? 1 : 0];
    char lower[17]; for (int i = 0; i < 16; i++) lower[i] = (char)tolower((unsigned char)row0->id[i]); lower[16] = 0;
    unsetenv("DM_GAME_PROFILE");
    {
        char *a[] = {"nxhost-run", "dir", "main", NULL}; char **av = a; int ac = 3;
        int r = dm_game_profile_select(&ac, &av);
        check(!r && ac == 3 && av == a && same(&dm_game, def, diff, sizeof diff), "no option: the default, the arguments untouched");
    }
    {
        char *a[] = {"nxhost-run", "--game", lower, "dir", "main", NULL}; char **av = a; int ac = 5;
        int r = dm_game_profile_select(&ac, &av);
        check(!r && ac == 3 && !strcmp(av[0], "nxhost-run") && !strcmp(av[1], "dir") && !strcmp(av[2], "main") && same(&dm_game, row0, diff, sizeof diff),
              "--game %s (lower case): that row, the options taken off the arguments", lower);
    }
    {
        char *a[] = {"nxhost-run", "--game", "0000000000000000", "dir", NULL}; char **av = a; int ac = 4;
        check(dm_game_profile_select(&ac, &av) == -1, "--game of an unknown title ID: refused");
    }
    {
        char *a[] = {"nxhost-run", "--profile", (char *)good, "dir", NULL}; char **av = a; int ac = 4;
        int r = dm_game_profile_select(&ac, &av);
        check(!r && ac == 2 && !strcmp(av[1], "dir") && !strcmp(dm_game.name, "Selected from a file"), "--profile <file>: the file's profile (%s)", dm_game.name);
    }
    setenv("DM_GAME_PROFILE", good, 1);
    {
        dm_game = *def;
        char *a[] = {"nxhost-run", "dir", NULL}; char **av = a; int ac = 2;
        int r = dm_game_profile_select(&ac, &av);
        check(!r && ac == 2 && !strcmp(dm_game.name, "Selected from a file"), "DM_GAME_PROFILE=<file>: the file's profile");
    }
    {
        char *a[] = {"nxhost-run", "--game", lower, "dir", NULL}; char **av = a; int ac = 4;
        check(dm_game_profile_select(&ac, &av) == -1, "DM_GAME_PROFILE and --game together: refused");
    }
    unsetenv("DM_GAME_PROFILE");
    {
        char *a[] = {"nxhost-run", "--profile", NULL}; char **av = a; int ac = 2;
        check(dm_game_profile_select(&ac, &av) == -1, "--profile without its file: refused");
    }
    {
        char *a[] = {"nxhost-run", "--game", lower, "--game", lower, "dir", NULL}; char **av = a; int ac = 6;
        check(dm_game_profile_select(&ac, &av) == -1, "--game given twice: refused");
    }
    {
        char *a[] = {"nxhost-run", "--profile", (char *)write_file("bad.tsv", with_row(base, "zzz", "bogus_key\t1")), "dir", NULL}; char **av = a; int ac = 4;
        check(dm_game_profile_select(&ac, &av) == -1, "--profile of a file with an unknown key: refused");
    }
    printf("t_game_profile: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
