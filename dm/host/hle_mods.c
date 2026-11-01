// Mods: folders of replacement game files and code patches, in the layouts emulators and Atmosphère read, found when the game mounts its RomFS.
//   DM_MODS=<folder>[:<folder>…]   folders of mods (default <DM_ROOT>/mods; the iOS app sets <App>/dm/mods, the mods it was built with, then Documents/mods)
//   DM_MODS_OFF=<name>[/<name>…]   mods switched off, by folder name ('/' separates them: no folder name can hold one); the launchers set it
// A mod is a folder holding romfs/ (files that replace the game's, or add to them) and/or exefs/ (code patches: .pchtxt, .ips). It may sit in a folder of mods
// directly (<folder>/<mod>/romfs), or inside the trees mod downloads come in: atmosphere/contents/<title ID>/, contents/<title ID>/<mod>/ (Ryujinx) or
// <title ID>/<mod>/ (yuzu's load folder), at the top of a mods folder or inside a mod's own folder. Mods come in DM_MODS order, each folder's in name order, and
// the first mod that has a file is the one the game reads (DM_ROMFS_OVERLAY's folders before all of them).
// Code patches are applied by the rewriter when the app is built (scripts/build_ipa_from_nsp.py --mods; patch_nso.py DM_EXEFS_PATCHES), since the code the game runs
// is rewritten and signed then: the modules they change are rewritten twice, without and with them, and the build carries both (the second in codemods/). A mod packed
// that way carries dm_code.tsv (the patch files applied); the mods with code patches switch on and off together, as one copy holds all their patches, and the loader
// takes that copy only while all of them are on (loader.c). Any other mod with code patches is skipped whole with a note, as its files without its code can break the
// game (a 60 fps mod's parameters without its frame-rate patch run the game at half speed).
// A mod may also carry env.txt, KEY=VALUE lines of host settings it needs, set while it is on (dm_mods_env; an env file or the launch environment wins): a 60 fps
// mod's DM_PRESENT_INTERVAL=display does what its SDK frame-rate patch does on a Switch, as the host paces presents itself.
#include "dm.h"
#include "mods.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((weak)) int dm_code_mods_loaded;   // loader.c's, when it is linked in (the unit tests have no loader)
#define MAX_MODS 128
typedef struct { char name[256]; char dir[1024]; int kind; } mod_t;
static mod_t g_mods[MAX_MODS];
static int g_n;

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int has_files(const char *dir) {   // any file below dir, dot files aside
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        found = is_dir(p) ? has_files(p) : 1;
    }
    closedir(d); return found;
}
static int is_mod(const char *dir) {
    char p[1100]; snprintf(p, sizeof p, "%s/romfs", dir); if (is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/exefs", dir); if (is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/dm_code.tsv", dir); return !access(p, F_OK);   // a built-in mod of code patches only
}
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
// The visible subfolder names of dir, sorted; *n set; NULL when there are none
static char **subdirs(const char *dir, int *n) {
    *n = 0; DIR *d = opendir(dir); if (!d) return NULL;
    char **v = NULL; int cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!is_dir(p)) continue;
        if (*n == cap) { cap = cap ? cap * 2 : 16; char **nv = realloc(v, cap * sizeof *v); if (!nv) die("mods: out of memory"); v = nv; }
        v[(*n)++] = strdup(e->d_name);
    }
    closedir(d);
    if (*n) qsort(v, *n, sizeof *v, by_name);
    return v;
}
static void add(const char *name, const char *dir) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_mods[i].name, name)) return;   // a name already found in an earlier folder: that one is used
    if (g_n == MAX_MODS) return;
    mod_t *m = &g_mods[g_n++];
    snprintf(m->name, sizeof m->name, "%s", name); snprintf(m->dir, sizeof m->dir, "%s", dir);
    char p[1100], q[1100];
    snprintf(p, sizeof p, "%s/romfs", dir); snprintf(q, sizeof q, "%s/exefs", dir);
    int romfs = has_files(p), code = has_files(q);
    snprintf(p, sizeof p, "%s/dm_code.tsv", dir);
    m->kind = !access(p, F_OK) ? DM_MOD_BUILT_IN : code ? DM_MOD_NEEDS_BUILD : romfs ? DM_MOD_FILES : DM_MOD_EMPTY;
}
// The mods of a folder whose subfolders are mods (contents/<title ID>, <title ID>): each subfolder that is one, by its own name
static void add_children(const char *dir) {
    int n; char **v = subdirs(dir, &n);
    for (int i = 0; i < n; i++) { char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, v[i]); if (is_mod(p)) add(v[i], p); free(v[i]); }
    free(v);
}
// dir/<name>, the name matched case-insensitively (title ID folders come in either case); 0 when there is none
static int child_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); found = is_dir(out); }
    closedir(d); return found;
}
// A tree under base: <base>/<title ID> (yuzu), <base>/contents/<title ID> (Ryujinx) or <base>/atmosphere/contents/<title ID>; the mod is named name when the
// tree's folder is one mod, and its subfolders by their own names when it holds several. 1 when base has one of the trees
static int add_tree(const char *name, const char *base, const char *tid) {
    char c[1100], a[1100], t[1100];
    int hit = (child_ci(base, "atmosphere", a, sizeof a) && child_ci(a, "contents", c, sizeof c) && child_ci(c, tid, t, sizeof t))
           || (child_ci(base, "contents", c, sizeof c) && child_ci(c, tid, t, sizeof t))
           || child_ci(base, tid, t, sizeof t);
    if (hit) { if (is_mod(t)) add(name, t); else add_children(t); }
    return hit;
}
static void scan_folder(const char *root, const char *tid) {
    add_tree("atmosphere", root, tid);   // the trees at the top of the folder (an SD card's copy, a Ryujinx mods folder); an Atmosphère tree is one mod
    int n; char **v = subdirs(root, &n);
    for (int i = 0; i < n; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", root, v[i]);
        if (strcasecmp(v[i], tid) && strcmp(v[i], "contents") && strcmp(v[i], "atmosphere")) { if (is_mod(p)) add(v[i], p); else add_tree(v[i], p, tid); }
        free(v[i]);
    }
    free(v);
}

int dm_mods_scan(void) {
    g_n = 0;
    char tid[17]; snprintf(tid, sizeof tid, "%016llX", (unsigned long long)DM_GAME_PROGRAM_ID);
    char def[1100]; const char *e = getenv("DM_MODS");
    if (!e || !*e) { const char *r = getenv("DM_ROOT"); if (!r || !*r) return 0; snprintf(def, sizeof def, "%s/mods", r); e = def; }
    char *all = strdup(e), *save = NULL;
    if (!all) die("mods: out of memory");
    for (char *root = strtok_r(all, ":", &save); root; root = strtok_r(NULL, ":", &save)) {
        scan_folder(root, tid);
    }
    free(all);
    return g_n;
}
const char *dm_mod_name(int i) { return i >= 0 && i < g_n ? g_mods[i].name : ""; }
const char *dm_mod_dir(int i) { return i >= 0 && i < g_n ? g_mods[i].dir : ""; }
int dm_mod_kind(int i) { return i >= 0 && i < g_n ? g_mods[i].kind : DM_MOD_EMPTY; }
static int switched_off(const char *name) {
    const char *off = getenv("DM_MODS_OFF"); size_t k = strlen(name);
    for (const char *s = off; s && *s; ) {
        const char *t = strchr(s, '/'); size_t len = t ? (size_t)(t - s) : strlen(s);
        if (len == k && !strncmp(s, name, k)) return 1;
        s = t ? t + 1 : NULL;
    }
    return 0;
}
static int code_wanted(void) {   // over the current scan: at least one mod with built-in code patches, and none of them switched off
    int any = 0;
    for (int i = 0; i < g_n; i++) if (g_mods[i].kind == DM_MOD_BUILT_IN) { if (switched_off(g_mods[i].name)) return 0; any = 1; }
    return any;
}
int dm_mods_code_wanted(void) { dm_mods_scan(); return code_wanted(); }
int dm_mod_on(int i) {
    if (i < 0 || i >= g_n) return 0;
    return g_mods[i].kind == DM_MOD_BUILT_IN ? code_wanted() : g_mods[i].kind == DM_MOD_FILES && !switched_off(g_mods[i].name);
}
void dm_mods_env(void) {
    int n = dm_mods_scan();
    for (int i = 0; i < n; i++) {
        if (!dm_mod_on(i)) continue;
        char p[1100]; snprintf(p, sizeof p, "%s/env.txt", g_mods[i].dir);
        FILE *f = fopen(p, "r"); if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *s = line; while (*s == ' ' || *s == '\t') s++;
            s[strcspn(s, "\r\n")] = 0;
            char *eq = strchr(s, '=');
            if (!*s || *s == '#' || !eq || eq == s) continue;
            *eq = 0;
            if (getenv(s)) { LOG("mods: %s: %s stays %s (set before; the mod's env.txt has %s)", g_mods[i].name, s, getenv(s), eq + 1); continue; }
            setenv(s, eq + 1, 1); LOG("mods: %s: %s=%s (its env.txt)", g_mods[i].name, s, eq + 1);
        }
        fclose(f);
    }
}

const char *dm_mods_overlay(char *buf, unsigned n) {
    const char *ov = getenv("DM_ROMFS_OVERLAY");
    snprintf(buf, n, "%s", ov ? ov : "");
    int found = dm_mods_scan(), on = 0;
    for (int i = 0; i < found; i++) {
        const mod_t *m = &g_mods[i]; char romfs[1100]; snprintf(romfs, sizeof romfs, "%s/romfs", m->dir);
        const char *why = NULL;
        switch (m->kind) {
        case DM_MOD_EMPTY: why = "no files in romfs/ or exefs/"; break;
        case DM_MOD_NEEDS_BUILD: why = "it has code patches (exefs/), which are applied only when the app is built (build_ipa_from_nsp.py --mods): skipped whole"; break;
        case DM_MOD_FILES: if (switched_off(m->name)) why = "switched off (DM_MODS_OFF)"; break;
        case DM_MOD_BUILT_IN: if (!dm_code_mods_loaded) why = code_wanted() ? "its code patches are not in the modules the game loaded (no codemods copy)" : "switched off, with the other mods with code patches (DM_MODS_OFF)"; break;
        }
        if (!why && strchr(m->dir, ':')) why = "its path has a ':', which separates the overlay folders";
        if (why) { LOG("%smods: %s (%s): %s", m->kind == DM_MOD_FILES || (m->kind == DM_MOD_BUILT_IN && !code_wanted()) ? "" : "[RED] ", m->name, m->dir, why); continue; }
        if (!is_dir(romfs)) { LOG("mods: %s: on (code patches only)", m->name); on++; continue; }
        size_t len = strlen(buf);
        if (len + 1 + strlen(romfs) + 1 > n) { LOG("[RED] mods: %s: no room left in the overlay list (%u bytes): skipped", m->name, n); continue; }
        snprintf(buf + len, n - len, "%s%s", len ? ":" : "", romfs);
        LOG("mods: %s: on%s (%s)", m->name, m->kind == DM_MOD_BUILT_IN ? ", with its code patches" : "", romfs); on++;
    }
    if (found) LOG("mods: %d found, %d on", found, on);
    return buf;
}
