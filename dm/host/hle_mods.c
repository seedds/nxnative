// Mods folder (DM_MODS, default <DM_ROOT>/mods): emulator-style mods, one folder each, feeding the RomFS overlay, the cheat engine and load-time exefs patches (docs/development.md "Mods")
#include "dm.h"
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <TargetConditionals.h>

#define MOD_MAX 64
#define PFILE_MAX 256
#define PATCH_FILE_MAX (16u << 20)
#define NSO_HEADER 0x100

typedef struct { char name[256]; char dir[1024]; char root[1024]; } mod_t;
typedef struct { char path[1536]; char bid[65]; int mod, ips; } pfile_t;
typedef struct { uint64_t off; uint32_t len; uint8_t *data; } rec_t;
typedef struct { rec_t *r; int n, cap; } recs_t;
typedef struct { const module_t *m; uint64_t off, len; int mod; } applied_t;

static mod_t g_mods[MOD_MAX]; static int g_nmods;
static char g_cheat_dirs[MOD_MAX][1024]; static int g_cheat_mod[MOD_MAX], g_ncheat_dirs;
static pfile_t g_pfiles[PFILE_MAX]; static int g_npfiles;
static applied_t *g_applied; static int g_napplied, g_applied_cap;
static struct { char name[256]; char romfs[1300]; int on, at_start; } g_all[MOD_MAX * 4]; static int g_nall, g_edited;
static char g_overlay_first[4096];
static char g_base[1024];

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_hex(char c) { return isxdigit((unsigned char)c) != 0; }
static int has_suffix(const char *s, const char *suf) { size_t n = strlen(s), k = strlen(suf); return n > k && !strcasecmp(s + n - k, suf); }

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = malloc(PATCH_FILE_MAX + 1);
    size_t got = buf ? fread(buf, 1, PATCH_FILE_MAX + 1, f) : 0;
    fclose(f);
    if (!buf || got > PATCH_FILE_MAX) { free(buf); return NULL; }
    buf[got] = 0; *n = got;
    return buf;
}

// A patch names its module by NSO build ID, often with trailing zeros cut or all 32 bytes given; the loader keeps the first 16 bytes
static int bid_matches(const char *id, const char *build) {
    size_t n = strlen(id);
    if (n < 8 || strlen(build) != 32) return 0;
    for (size_t k = 0; k < 32; k++) {
        char a = k < n ? id[k] : '0';
        if (tolower((unsigned char)a) != tolower((unsigned char)build[k])) return 0;
    }
    return 1;
}

static void pchtxt_bid(const char *path, char out[65]) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (strncasecmp(s, "@nsobid-", 8)) continue;
        s += 8; size_t k = 0;
        while (is_hex(s[k]) && k < 64) { out[k] = s[k]; k++; }
        out[k] = 0;
        break;
    }
    fclose(f);
}

static void scan_patch_dir(const char *dir, int mod) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int ips = has_suffix(e->d_name, ".ips"), pch = has_suffix(e->d_name, ".pchtxt");
        if (!ips && !pch) {
            if (!strcmp(e->d_name, "main") || !strcmp(e->d_name, "sdk") || !strcmp(e->d_name, "rtld") || !strncmp(e->d_name, "subsdk", 6) || !strcmp(e->d_name, "main.npdm"))
                LOG("[RED] mods: %s: %s/%s replaces or adds a code module, which the rewriter has to process when the game is built: not loaded", g_mods[mod].name, dir, e->d_name);
            continue;
        }
        if (g_npfiles == PFILE_MAX) { LOG("[RED] mods: more than %d exefs patch files: %s/%s and later ones are not applied", PFILE_MAX, dir, e->d_name); break; }
        pfile_t *p = &g_pfiles[g_npfiles];
        snprintf(p->path, sizeof p->path, "%s/%s", dir, e->d_name);
        p->mod = mod; p->ips = ips;
        if (ips) {
            size_t k = 0;
            while (is_hex(e->d_name[k]) && k < 64) { p->bid[k] = e->d_name[k]; k++; }
            p->bid[k] = 0;
            if (e->d_name[k] != '.') p->bid[0] = 0;
        } else pchtxt_bid(p->path, p->bid);
        if (!p->bid[0]) { LOG("[RED] mods: %s: %s names no NSO build ID (an .ips is named after it, a .pchtxt starts with @nsobid-): not applied", g_mods[mod].name, p->path); continue; }
        g_npfiles++;
    }
    closedir(d);
}

static void scan_patch_sets(const char *parent, int mod) {
    DIR *d = opendir(parent);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1280]; snprintf(p, sizeof p, "%s/%s", parent, e->d_name);
        if (is_dir(p)) scan_patch_dir(p, mod);
    }
    closedir(d);
}

static void mod_root(const char *dir, const char *tid, char *root, size_t n) {
    const char *layouts[] = {"%s/atmosphere/contents/%s", "%s/contents/%s", "%s/%s"};
    snprintf(root, n, "%s", dir);
    for (size_t i = 0; i < sizeof layouts / sizeof *layouts; i++) {
        char p[1280]; snprintf(p, sizeof p, layouts[i], dir, tid);
        if (is_dir(p)) { snprintf(root, n, "%s", p); break; }
    }
}

// What the Mods menu needs of a mod, on or off: its romfs folder (changed while the game runs), and whether it has anything that is only read at the start
static void list_mod(const char *base, const char *name, const char *tid, int on) {
    char dir[1280], root[1280], p[1400];
    strcpy(g_all[g_nall].name, name); g_all[g_nall].on = on;
    snprintf(dir, sizeof dir, "%s/%s", base, name); mod_root(dir, tid, root, sizeof root);
    snprintf(p, sizeof p, "%s/romfs", root);
    if (is_dir(p) && !strchr(p, ':')) strlcpy(g_all[g_nall].romfs, p, sizeof g_all[0].romfs);
    const char *start[] = {"%s/exefs", "%s/cheats"};
    for (size_t i = 0; i < 2; i++) { snprintf(p, sizeof p, start[i], root); if (is_dir(p)) g_all[g_nall].at_start = 1; }
    snprintf(p, sizeof p, "%s/atmosphere/exefs_patches", dir); if (is_dir(p)) g_all[g_nall].at_start = 1;
    snprintf(p, sizeof p, "%s/exefs_patches", dir); if (is_dir(p)) g_all[g_nall].at_start = 1;
    g_nall++;
}

static void add_mod(const char *base, const char *name, const char *tid, char *overlay, size_t overlay_n) {
    if (g_nmods == MOD_MAX) { LOG("[RED] mods: more than %d mods: %s and later ones are not loaded", MOD_MAX, name); return; }
    int idx = g_nmods++;
    mod_t *m = &g_mods[idx];
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->dir, sizeof m->dir, "%s/%s", base, name);
    mod_root(m->dir, tid, m->root, sizeof m->root);
    char p[1280]; int romfs = 0, cheats = 0, before = g_npfiles;
    snprintf(p, sizeof p, "%s/romfs", m->root);
    if (is_dir(p)) {
        size_t used = strlen(overlay);
        if (strchr(p, ':')) LOG("[RED] mods: %s: its path holds a ':', the overlay list's separator: its romfs folder is not used", name);
        else if (used + strlen(p) + 2 > overlay_n) LOG("[RED] mods: %s: the RomFS overlay list is full (%zu bytes): its romfs folder is not used", name, overlay_n);
        else { if (used) strlcat(overlay, ":", overlay_n); strlcat(overlay, p, overlay_n); romfs = 1; }
    }
    snprintf(p, sizeof p, "%s/cheats", m->root);
    if (is_dir(p)) { g_cheat_mod[g_ncheat_dirs] = idx; snprintf(g_cheat_dirs[g_ncheat_dirs++], sizeof g_cheat_dirs[0], "%s", p); cheats = 1; }
    snprintf(p, sizeof p, "%s/exefs", m->root); scan_patch_dir(p, idx);
    snprintf(p, sizeof p, "%s/atmosphere/exefs_patches", m->dir); scan_patch_sets(p, idx);
    snprintf(p, sizeof p, "%s/exefs_patches", m->dir); scan_patch_sets(p, idx);
    LOG("mods: %d %s:%s%s %d exefs patch file(s)%s", idx + 1, name, romfs ? " romfs," : "", cheats ? " cheats," : "", g_npfiles - before,
        !romfs && !cheats && g_npfiles == before ? " (nothing this host loads: expected romfs/, cheats/ or exefs/ in it, or an atmosphere/contents/<title ID>/ tree)" : "");
}

static int by_name(const void *a, const void *b) { return strcasecmp(a, b); }

static int has_content(const char *dir, const char *tid) {
    const char *roots[] = {"%s", "%s/atmosphere/contents/%s", "%s/contents/%s", "%s/%s"}, *kinds[] = {"romfs", "exefs", "cheats"};
    char root[1280], p[1400];
    for (size_t r = 0; r < sizeof roots / sizeof *roots; r++) {
        snprintf(root, sizeof root, roots[r], dir, tid);
        for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) { snprintf(p, sizeof p, "%s/%s", root, kinds[k]); if (is_dir(p)) return 1; }
    }
    snprintf(p, sizeof p, "%s/atmosphere/exefs_patches", dir); if (is_dir(p)) return 1;
    snprintf(p, sizeof p, "%s/exefs_patches", dir);
    return is_dir(p);
}

static int list_dirs(const char *dir, char out[][256], int max) {
    int n = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && n < max) {
        if (ent->d_name[0] == '.' || strlen(ent->d_name) >= 120) continue;
        char p[1280]; snprintf(p, sizeof p, "%s/%s", dir, ent->d_name);
        if (is_dir(p)) strcpy(out[n++], ent->d_name);
    }
    closedir(d);
    qsort(out, (size_t)n, 256, by_name);
    return n;
}

// A mod folder with nothing of its own and folders that are mods is a pack of variants (a recolour in five colours): each is listed as
// "<pack>/<variant>". Two of a pack that replace the same RomFS file are alternatives, and only one of them is on; two that share no file
// (a pack of separate mods) can both be on
static size_t pack_len(const char *name) { const char *s = strchr(name, '/'); return s ? (size_t)(s - name) + 1 : 0; }
static int shares_file(const char *a, const char *b) {
    DIR *d = opendir(a);
    if (!d) return 0;
    struct dirent *e; int hit = 0;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char pa[1600], pb[1600]; struct stat sa, sb;
        snprintf(pa, sizeof pa, "%s/%s", a, e->d_name); snprintf(pb, sizeof pb, "%s/%s", b, e->d_name);
        if (stat(pa, &sa) || stat(pb, &sb)) continue;
        hit = S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode) ? shares_file(pa, pb) : !S_ISDIR(sa.st_mode) && !S_ISDIR(sb.st_mode);
    }
    closedir(d);
    return hit;
}
static int alternatives(int a, int b) {
    size_t n = pack_len(g_all[a].name);
    return a != b && n && !strncmp(g_all[a].name, g_all[b].name, n) && g_all[a].romfs[0] && g_all[b].romfs[0] && shares_file(g_all[a].romfs, g_all[b].romfs);
}
static int alternative_on(int a) { for (int i = 0; i < g_nall; i++) if (g_all[i].on && alternatives(a, i)) return 1; return 0; }
int dm_mods_exclusive(int i) { for (int k = 0; i >= 0 && i < g_nall && k < g_nall; k++) if (alternatives(i, k)) return 1; return 0; }

// order.txt lists mod folder names, highest priority first; a leading '#' turns a mod off. Folders it does not name follow in name order
void dm_mods_init(void) {
    char *base = g_base;
    const char *e = getenv("DM_MODS");
    if (e && *e) snprintf(base, sizeof g_base, "%s", e);
    else { e = getenv("DM_ROOT"); if (e && *e) snprintf(base, sizeof g_base, "%s/mods", e); }
    if (!base[0] || !is_dir(base)) return;
    static char found[MOD_MAX * 4][256], tops[MOD_MAX * 4][256], subs[MOD_MAX][256]; static char taken[MOD_MAX * 4];
    char tid[17]; snprintf(tid, sizeof tid, "%016llX", (unsigned long long)DM_GAME_PROGRAM_ID);
    int nfound = 0, ntops = list_dirs(base, tops, MOD_MAX * 4);
    for (int i = 0; i < ntops && nfound < MOD_MAX * 4; i++) {
        char p[1280], q[1500]; snprintf(p, sizeof p, "%s/%s", base, tops[i]);
        int nsubs = has_content(p, tid) ? 0 : list_dirs(p, subs, MOD_MAX), variants = 0;
        for (int k = 0; k < nsubs && nfound < MOD_MAX * 4; k++) {
            snprintf(q, sizeof q, "%s/%s", p, subs[k]);
            if (has_content(q, tid)) { snprintf(found[nfound++], sizeof found[0], "%s/%s", tops[i], subs[k]); variants++; }
        }
        if (!variants) strcpy(found[nfound++], tops[i]);
    }
    static char overlay[4096];
    e = getenv("DM_ROMFS_OVERLAY");
    if (e && *e) strlcpy(overlay, e, sizeof overlay);
    strlcpy(g_overlay_first, overlay, sizeof g_overlay_first);
    char op[1280], line[512]; snprintf(op, sizeof op, "%s/order.txt", base);
    FILE *f = fopen(op, "r");
    int off = 0;
    while (f && fgets(line, sizeof line, f)) {
        char *s = trim(line);
        int disabled = *s == '#';
        if (disabled) s = trim(s + 1);
        if (!*s) continue;
        for (int i = 0; i < nfound; i++) if (!taken[i] && !strcmp(found[i], s)) {
            taken[i] = 1;
            list_mod(base, s, tid, 0);
            if (!disabled && alternative_on(g_nall - 1)) { LOG("mods: %s is left off: a variant of its pack that replaces the same files is on", s); disabled = 1; }
            g_all[g_nall - 1].on = !disabled;
            if (disabled) off++; else add_mod(base, s, tid, overlay, sizeof overlay);
            break;
        }
    }
    if (f) fclose(f);
    for (int i = 0; i < nfound; i++) if (!taken[i]) {
        list_mod(base, found[i], tid, 0);
        int on = !alternative_on(g_nall - 1);   // of alternatives order.txt does not name, the first
        g_all[g_nall - 1].on = on;
        if (on) add_mod(base, found[i], tid, overlay, sizeof overlay); else off++;
    }
    if (overlay[0]) setenv("DM_ROMFS_OVERLAY", overlay, 1);
    LOG("mods: %s: %d on, %d off (order.txt)", base, g_nmods, off);
#if TARGET_OS_IPHONE
    if (g_npfiles) LOG("[RED] mods: %d exefs patch file(s) found, but an iOS app's code is signed and fixed: they are not applied", g_npfiles);
#endif
}

const char *dm_mods_dir_path(void) { return g_base; }
int dm_mods_count(void) { return g_nall; }
const char *dm_mods_name(int i) { return i >= 0 && i < g_nall ? g_all[i].name : ""; }
int dm_mods_on(int i) { return i >= 0 && i < g_nall && g_all[i].on; }
int dm_mods_edited(void) { return g_edited; }

// The romfs folders follow at once: files the game opens from now on come from the new set (what it has loaded stays until it loads it again).
// Code patches and cheat folders are read at the start only. The choice is written to order.txt either way
void dm_mods_set(int i, int on) {
    if (i < 0 || i >= g_nall) return;
    for (int k = 0; on && k < g_nall; k++) if (g_all[k].on && alternatives(i, k)) { g_all[k].on = 0; if (g_all[k].at_start) g_edited = 1; }
    g_all[i].on = on; if (g_all[i].at_start) g_edited = 1;
    static char overlay[4096]; strlcpy(overlay, g_overlay_first, sizeof overlay);
    for (int k = 0; k < g_nall; k++) if (g_all[k].on && g_all[k].romfs[0] && strlen(overlay) + strlen(g_all[k].romfs) + 2 <= sizeof overlay) {
        if (overlay[0]) strlcat(overlay, ":", sizeof overlay);
        strlcat(overlay, g_all[k].romfs, sizeof overlay);
    }
    dm_fs_overlay_set(overlay);
    char p[1280], tmp[1290]; snprintf(p, sizeof p, "%s/order.txt", g_base); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG("[RED] mods: cannot write %s: %s", tmp, strerror(errno)); return; }
    for (int k = 0; k < g_nall; k++) fprintf(f, "%s%s\n", g_all[k].on ? "" : "# ", g_all[k].name);
    if (fclose(f) || rename(tmp, p)) LOG("[RED] mods: cannot write %s: %s", p, strerror(errno));
    else LOG("mods: %s turned %s%s", g_all[i].name, on ? "on" : "off", g_all[i].at_start ? " (its code patches and cheats follow at the next start)" : "");
}

const char *dm_mods_cheat_dir(int i, const char **mod) {
    if (i < 0 || i >= g_ncheat_dirs) return NULL;
    if (mod) *mod = g_mods[g_cheat_mod[i]].name;
    return g_cheat_dirs[i];
}

static void rec_add(recs_t *rs, uint64_t off, uint32_t len, const uint8_t *data, int fill) {
    if (!len) return;
    if (rs->n == rs->cap) {
        rs->cap = rs->cap ? rs->cap * 2 : 64;
        if (!(rs->r = realloc(rs->r, (size_t)rs->cap * sizeof *rs->r))) die("mods: patch record allocation failed");
    }
    uint8_t *copy = malloc(len);
    if (!copy) die("mods: patch record allocation failed");
    if (data) memcpy(copy, data, len); else memset(copy, fill, len);
    rs->r[rs->n++] = (rec_t){off, len, copy};
}
static void recs_free(recs_t *rs) { for (int i = 0; i < rs->n; i++) free(rs->r[i].data); free(rs->r); *rs = (recs_t){0}; }

// Record offsets are positions in the uncompressed NSO, whose 0x100-byte header precedes the image
static int parse_ips(const uint8_t *b, size_t n, recs_t *rs) {
    int w = n >= 5 && !memcmp(b, "IPS32", 5) ? 4 : n >= 5 && !memcmp(b, "PATCH", 5) ? 3 : 0;
    if (!w) return -1;
    size_t p = 5;
    for (;;) {
        if (w == 4 ? p + 4 <= n && !memcmp(b + p, "EEOF", 4) : p + 3 <= n && !memcmp(b + p, "EOF", 3)) return 0;
        if (p + (size_t)w + 2 > n) return -1;
        uint64_t off = 0;
        for (int k = 0; k < w; k++) off = off << 8 | b[p + k];
        p += (size_t)w;
        uint32_t len = (uint32_t)b[p] << 8 | b[p + 1]; p += 2;
        if (len) {
            if (p + len > n) return -1;
            rec_add(rs, off, len, b + p, 0); p += len;
        } else {
            if (p + 3 > n) return -1;
            rec_add(rs, off, (uint32_t)b[p] << 8 | b[p + 1], NULL, b[p + 2]); p += 3;
        }
    }
}

static int hex_val(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

static uint32_t parse_string(const char *s, uint8_t *out) {
    uint32_t n = 0;
    for (s++; *s && *s != '"'; s++) {
        if (*s != '\\' || !s[1]) { out[n++] = (uint8_t)*s; continue; }
        s++;
        switch (*s) {
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            case 'a': out[n++] = '\a'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'v': out[n++] = '\v'; break;
            case '0': out[n++] = 0; break;
            case 'x': if (is_hex(s[1]) && is_hex(s[2])) { out[n++] = (uint8_t)(hex_val(s[1]) << 4 | hex_val(s[2])); s += 2; } break;
            default: out[n++] = (uint8_t)*s;
        }
    }
    return n;
}

static void parse_pchtxt(char *text, recs_t *rs) {
    int enabled = 1, little = 0; uint64_t shift = 0;
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *s = trim(line);
        if (*s == '@') {
            if (!strncasecmp(s, "@stop", 5)) break;
            if (!strncasecmp(s, "@enabled", 8)) enabled = 1;
            else if (!strncasecmp(s, "@disabled", 9)) enabled = 0;
            else if (!strncasecmp(s, "@little-endian", 14)) little = 1;
            else if (!strncasecmp(s, "@big-endian", 11)) little = 0;
            else if (!strncasecmp(s, "@flag", 5)) {
                char *t = trim(s + 5);
                if (!strncasecmp(t, "offset_shift", 12)) shift = strtoull(t + 12, NULL, 0);
            }
            continue;
        }
        int hex8 = 1;
        for (int k = 0; k < 8 && hex8; k++) hex8 = is_hex(s[k]);
        if (!enabled || !hex8 || (s[8] != ' ' && s[8] != '\t')) continue;
        uint32_t off = 0;
        for (int k = 0; k < 8; k++) off = off << 4 | (uint32_t)hex_val(s[k]);
        if (little) off = __builtin_bswap32(off);
        char *v = trim(s + 8);
        uint8_t *bytes = malloc(strlen(v) + 1); uint32_t n = 0;
        if (!bytes) die("mods: patch record allocation failed");
        if (*v == '"') n = parse_string(v, bytes);
        else for (; is_hex(v[0]) && is_hex(v[1]); v += 2) bytes[n++] = (uint8_t)(hex_val(v[0]) << 4 | hex_val(v[1]));
        rec_add(rs, (uint64_t)off + shift, n, bytes, 0);
        free(bytes);
    }
}

// An instruction the rewriter replaces wherever the game has it (patch_nso.py): written into the mapped image as it is, it would run
// against Apple's kernel instead of the host. Field tests are by instruction class; within the FP / SIMD classes that move a value
// to or from a general register, v18 is taken for x18 (a refusal too many, never one too few)
static const char *needs_rewriter(uint32_t w) {
    unsigned rd = w & 31, rn = w >> 5 & 31, rt2 = w >> 10 & 31, rm = w >> 16 & 31;
    static const char *x18 = "uses x18, which Apple's kernel owns";
    if ((w & 0xFFE0001F) == 0xD4000001) return "is an svc";
    if ((w & 0xFFD80000) == 0xD5180000 && (w >> 16 & 7) == 3 && ((w >> 12 & 15) == 13 || (w >> 12 & 15) == 14)) return "reads or writes a thread or timer system register";
    if ((w & 0x3F800000) == 0x08000000) return "is an exclusive load or store";
    if ((w & 0xFFE00000) == 0xCB200000 && rd == 31 && rn == 31) return "moves sp by a register (sub sp, sp, xN)";
    if ((w & 0xBF800000) == 0x91000000 && rd == 31 && rn == 31 && !(w >> 22 & 1) && (w >> 10 & 0xFFF) % 16) return "moves sp by an immediate that is not a multiple of 16";
    switch (w >> 25 & 15) {
        case 8: case 9: {
            unsigned op = w >> 23 & 7;
            if (rd == 18) return x18;
            if (op >= 2 && op != 5 && rn == 18) return x18;
            if (op == 7 && rm == 18) return x18;
            return NULL;
        }
        case 10: case 11:
            if ((w & 0x7E000000) == 0x34000000 || (w & 0x7E000000) == 0x36000000) return rd == 18 ? x18 : NULL;
            if ((w & 0xFFC00000) == 0xD5000000) return rd == 18 ? x18 : NULL;
            if ((w & 0xFE000000) == 0xD6000000) return rn == 18 || rd == 18 ? x18 : NULL;
            return NULL;
        case 4: case 6: case 12: case 14: {
            int vec = w >> 26 & 1;
            int literal = (w & 0x3B000000) == 0x18000000, pair = (w & 0x3A000000) == 0x28000000, single = (w & 0x3A000000) == 0x38000000;
            int excl = (w & 0x3F000000) == 0x08000000, simd_struct = (w & 0xBE000000) == 0x0C000000;
            int prefetch = (w & 0xFFC00000) == 0xF9800000 || (w & 0xFFE00C00) == 0xF8A00800 || (w & 0xFFE00C00) == 0xF8800000 || (w & 0xFF000000) == 0xD8000000;
            if (!literal && rn == 18) return x18;
            if (!vec && !prefetch && rd == 18) return x18;
            if ((excl || (pair && !vec)) && rt2 == 18) return x18;
            if (excl && rm == 18) return x18;
            if (single && !(w >> 24 & 1) && (w >> 21 & 1) && !(w >> 10 & 1) && rm == 18) return x18;
            if (simd_struct && (w >> 23 & 1) && rm == 18) return x18;
            return NULL;
        }
        case 5: case 13: {
            int ccmp = (w & 0x1FE00000) == 0x1A400000, three = (w & 0x1F000000) == 0x1B000000;
            if (rn == 18) return x18;
            if (!ccmp && rd == 18) return x18;
            if (!(ccmp && (w >> 11 & 1)) && rm == 18) return x18;
            if (three && rt2 == 18) return x18;
            return NULL;
        }
        case 7: case 15: {
            int to_from_int = (w & 0x5F000000) == 0x1E000000 && ((w & 0x00200000) == 0 || (w & 0x0000FC00) == 0);
            int simd_copy = (w & 0x9FE08400) == 0x0E000400;
            return (to_from_int || simd_copy) && (rd == 18 || rn == 18) ? x18 : NULL;
        }
    }
    return NULL;
}

typedef struct { uint32_t (*run)[2]; int n, ok; } changed_t;

static void changed_read(changed_t *c, const module_t *m, const char *dir) {
    *c = (changed_t){0};
    char p[1280]; snprintf(p, sizeof p, "%s/%s.changed.tsv", dir, m->name);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[256], bid[40] = ""; int cap = 0; unsigned long off, len;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# BUILD_ID %39s", bid) == 1) continue;
        if (sscanf(line, "%lx %lx", &off, &len) != 2) continue;
        if (c->n == cap) {
            cap = cap ? cap * 2 : 1024;
            if (!(c->run = realloc(c->run, (size_t)cap * sizeof *c->run))) die("mods: changed list allocation failed");
        }
        c->run[c->n][0] = (uint32_t)off; c->run[c->n][1] = (uint32_t)len; c->n++;
    }
    fclose(f);
    c->ok = !strcasecmp(bid, m->build_id);
}

static int changed_hit(const changed_t *c, uint64_t off, uint64_t len) {
    int lo = 0, hi = c->n;
    while (lo < hi) { int mid = (lo + hi) / 2; if ((uint64_t)c->run[mid][0] + c->run[mid][1] <= off) lo = mid + 1; else hi = mid; }
    return lo < c->n && c->run[lo][0] < off + len;
}

static const char *rec_refusal(const module_t *m, const changed_t *c, const rec_t *r, uint64_t *at) {
    *at = r->off;
    if (r->off < NSO_HEADER) return "lies in the NSO header";
    uint64_t off = r->off - NSO_HEADER, end = off + r->len;
    *at = off;
    if (end > m->key_page_off) return "lies outside the module's image";
    if (off >= m->text_end) return NULL;
    if (!c->ok) return "is code, and the module has no changed-words list of its build beside it (rewrite it with dm/tools/patch_nso.py)";
    uint64_t text_end = end < m->text_end ? end : m->text_end;
    if (changed_hit(c, off, text_end - off)) return "replaces an instruction the rewriter already moved out of line";
    for (uint64_t a = off & ~3ull; a < text_end; a += 4) {
        uint32_t was, now; uint8_t word[4];
        memcpy(word, m->base + a, 4); memcpy(&was, word, 4);
        for (int k = 0; k < 4; k++) if (a + k >= off && a + k < end) word[k] = r->data[a + k - off];
        memcpy(&now, word, 4);
        const char *why = now != was ? needs_rewriter(now) : NULL;
        if (why) { *at = a; return why; }
    }
    return NULL;
}

static void note_applied(const module_t *m, uint64_t off, uint64_t len, int mod) {
    for (int i = 0; i < g_napplied; i++) {
        applied_t *a = &g_applied[i];
        if (a->m == m && a->mod != mod && a->off < off + len && off < a->off + a->len)
            LOG("mods: %s overrides %s at %s+%#llx", g_mods[mod].name, g_mods[a->mod].name, m->name, (unsigned long long)off);
    }
    if (g_napplied == g_applied_cap) {
        g_applied_cap = g_applied_cap ? g_applied_cap * 2 : 256;
        if (!(g_applied = realloc(g_applied, (size_t)g_applied_cap * sizeof *g_applied))) die("mods: applied list allocation failed");
    }
    g_applied[g_napplied++] = (applied_t){m, off, len, mod};
}

// Called while the image is still writable, before the loader seals its code and relocates it: the point at which Horizon's loader
// applies the same patches to the NSO. A file is applied whole or not at all, since half a hook is worse than none
void dm_mods_patch_module(module_t *m, const char *dir) {
    changed_t changed = {0}; int read_changed = 0;
    for (int i = g_npfiles - 1; i >= 0; i--) {
        pfile_t *p = &g_pfiles[i];
        if (!bid_matches(p->bid, m->build_id)) continue;
        size_t n; uint8_t *buf = read_file(p->path, &n);
        if (!buf) { LOG("[RED] mods: %s: cannot read %s (or it is larger than %u bytes)", g_mods[p->mod].name, p->path, PATCH_FILE_MAX); continue; }
        recs_t rs = {0};
        if (p->ips) { if (parse_ips(buf, n, &rs)) { LOG("[RED] mods: %s: %s is not a whole IPS or IPS32 file: not applied", g_mods[p->mod].name, p->path); recs_free(&rs); free(buf); continue; } }
        else parse_pchtxt((char *)buf, &rs);
        free(buf);
        if (!read_changed) { changed_read(&changed, m, dir); read_changed = 1; }
        const char *why = NULL; uint64_t at = 0; int refused = 0;
        for (int k = 0; k < rs.n; k++) {
            uint64_t a; const char *w = rec_refusal(m, &changed, &rs.r[k], &a);
            if (w && !refused++) { why = w; at = a; }
        }
        if (refused) LOG("[RED] mods: %s: %s is not applied: %d of its %d patches cannot be made in the running image, the first at %s+%#llx, which %s. It needs the module rewritten with the patch (not supported yet)",
                         g_mods[p->mod].name, p->path, refused, rs.n, m->name, (unsigned long long)at, why);
        else {
            for (int k = 0; k < rs.n; k++) {
                uint64_t off = rs.r[k].off - NSO_HEADER;
                memcpy(m->base + off, rs.r[k].data, rs.r[k].len);
                note_applied(m, off, rs.r[k].len, p->mod);
            }
            LOG("mods: %s: %d patch(es) from %s applied to %s (build %s)", g_mods[p->mod].name, rs.n, strrchr(p->path, '/') + 1, m->name, m->build_id);
        }
        recs_free(&rs);
    }
    free(changed.run);
}

void dm_mods_report_unmatched(void) {
    for (int i = 0; i < g_npfiles; i++) {
        pfile_t *p = &g_pfiles[i]; int hit = 0;
        for (int k = 0; k < g_nmod && !hit; k++) hit = bid_matches(p->bid, g_mod[k].build_id);
        if (!hit) LOG("[RED] mods: %s: %s is for build %s, which no loaded module has (main is %s): made for another version of the game, not applied", g_mods[p->mod].name, p->path, p->bid, dm_main_build());
    }
}
