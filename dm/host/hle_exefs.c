// ExeFS patches of mods (docs/frontends.md, Mods; docs/reference/rewriter.md, ExeFS patches of mods): the IPS, IPS32 and
// pchtxt files that Atmosphère (exefs_patches/<name>/<build ID>.ips) and the yuzu family (<mod>/exefs/*.pchtxt, *.ips)
// apply to a game's NSOs, applied here once at load, before the game's first instruction, to the loaded modules.
//
//   DM_EXEFS_PATCHES=<dir>:<dir>:…   the exefs folders of the mods that are on, first mod first (a front end sets it,
//                                    or env.txt; docs/frontends.md, Mods). A file that would change a byte an
//                                    earlier folder's file wrote is refused as a whole: two mods that put code in the
//                                    same place (the padding between functions, a code cave) cannot both run
//
// The formats, as their reference implementations read them:
//   - IPS ("PATCH" … "EOF"): records of a 3-byte big-endian offset and a 2-byte length, then the bytes; a length of 0 is
//     a run (2-byte count, 1 byte). IPS32 ("IPS32" … "EEOF") has 4-byte offsets. Offsets count from the NSO header, so
//     the image offset is offset - 0x100 (Atmosphère's ro patcher). The file is named after the module's build ID in hex
//     (the full 32 bytes, or a prefix of it: at least the first 8 bytes are compared)
//   - pchtxt (IPSwitch's text form, as yuzu reads it): "@nsobid-<build ID>" names the module, "@flag offset_shift <n>"
//     is added to every offset (0x100 makes the offsets image offsets), "@little-endian" / "@big-endian" turn the bytes of
//     each later hex value around or not, "@enabled" / "@disabled" start a group that is or is not applied, "@stop" ends
//     the file; a patch line is "<hex offset> <hex bytes>" or "<hex offset> \"<string>\"", and "//" starts a comment
//
// What is written, and what is refused (each refusal is a [RED] line naming the patch file):
//   - a write goes through a writable alias of the module (mach_vm_remap, as the cheat engine's code writes): the game's
//     own mapping stays executable and is never made writable. No alias (the iPhone: its code pages are the app's signed
//     pages) means no patch
//   - a write into a module's text must not touch a word the rewriter changed or moved (<name>.changed.tsv, as for the
//     cheats): the patch would undo a site the host needs, or would never run. Such a file is refused as a whole, so a
//     patch never half applies
//   - a mod whose exefs folder also holds a module (main, subsdk0…9, sdk, rtld) or a main.npdm is refused as a whole:
//     its patches branch into code the module would add (exlaunch and skyline mods), which this host does not load
//   - a file of another build of the module matches nothing, and is logged as such
#include "dm.h"
#include <libkern/OSCacheControl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>
#endif
#include <dirent.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>

#define EXEFS_HEADER 0x100   // an NSO header: IPS offsets count from it

typedef struct { uint64_t off; uint32_t len; const uint8_t *bytes; uint8_t fill; int run; } exefs_rec_t;   // one record: bytes, or len copies of fill
typedef int (*exefs_cb)(void *ctx, const exefs_rec_t *r);   // a record found by a parser, at an image offset; nonzero stops

// ---------- the formats (parsers only: dm/host/tests/t_exefs.c runs them on hand-made files) ----------

static unsigned be(const uint8_t *p, int n) { unsigned v = 0; while (n--) v = v << 8 | *p++; return v; }

// IPS or IPS32: 0 and every record passed to cb (offsets before the header dropped), -1 when the file is not one or is cut short
static int exefs_parse_ips(const uint8_t *d, size_t n, exefs_cb cb, void *ctx) {
    int w; size_t i;
    if (n >= 5 && !memcmp(d, "IPS32", 5)) { w = 4; i = 5; }
    else if (n >= 5 && !memcmp(d, "PATCH", 5)) { w = 3; i = 5; }
    else return -1;
    for (;;) {
        if (i + w > n) return -1;
        if (w == 3 && !memcmp(d + i, "EOF", 3)) return 0;
        if (w == 4 && !memcmp(d + i, "EEOF", 4)) return 0;
        uint64_t off = be(d + i, w); i += w;
        if (i + 2 > n) return -1;
        unsigned len = be(d + i, 2); i += 2;
        exefs_rec_t r = { 0 };
        if (len) {
            if (i + len > n) return -1;
            r.len = len; r.bytes = d + i; i += len;
        } else {   // a run
            if (i + 3 > n) return -1;
            r.len = be(d + i, 2); r.fill = d[i + 2]; r.run = 1; i += 3;
        }
        if (off < EXEFS_HEADER || !r.len) continue;   // the header itself is not part of the image
        r.off = off - EXEFS_HEADER;
        if (cb(ctx, &r)) return 0;
    }
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// Whether a build ID written in a file (hex digits, any case, possibly a prefix or padded with zeros past the module's 16
// bytes) names the module's (build_id: the loader's 32 hex digits, the first 16 bytes). At least 16 digits must be given
static int exefs_bid_match(const char *file_hex, size_t n, const char *mod_hex) {
    size_t m = strlen(mod_hex);
    if (n < 16 || m < 16) return 0;
    for (size_t k = 0; k < n && k < m; k++) if (tolower((unsigned char)file_hex[k]) != tolower((unsigned char)mod_hex[k])) return 0;
    return 1;
}

// pchtxt: the build ID it names in *bid (empty when none), and with cb the records of its enabled groups. 0, or -1 when a
// patch line cannot be read (then nothing of the file may be applied: the caller has only checked so far)
static int exefs_parse_pchtxt(const char *text, char *bid, size_t bidn, exefs_cb cb, void *ctx) {
    long long shift = 0; int le = 0, on = 0; bid[0] = 0;
    uint8_t *buf = malloc(strlen(text) / 2 + 16); if (!buf) return -1;
    const char *s = text; int rc = 0;
    while (*s) {
        const char *e = strchr(s, '\n'); size_t len = e ? (size_t)(e - s) : strlen(s);
        char line[8192]; if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len); line[len] = 0; s = e ? e + 1 : s + len;
        char *l = line; while (*l == ' ' || *l == '\t') l++;
        char *t = l + strlen(l); while (t > l && (t[-1] == '\r' || t[-1] == ' ' || t[-1] == '\t')) *--t = 0;
        if (!*l || (l[0] == '/' && l[1] == '/') || l[0] == '#') continue;
        if (l[0] == '@') {
            if (!strncasecmp(l, "@stop", 5)) break;
            if (!strncasecmp(l, "@nsobid-", 8)) {
                size_t k = 0; for (const char *p = l + 8; isxdigit((unsigned char)*p) && k + 1 < bidn; p++) bid[k++] = *p;
                bid[k] = 0;
            } else if (!strncasecmp(l, "@flag", 5)) {
                char key[64]; long long v;
                if (sscanf(l + 5, " %63s %lli", key, &v) == 2 && !strcasecmp(key, "offset_shift")) shift = v;
            } else if (!strncasecmp(l, "@little-endian", 14)) le = 1;
            else if (!strncasecmp(l, "@big-endian", 11)) le = 0;
            else if (!strncasecmp(l, "@enabled", 8)) on = 1;
            else if (!strncasecmp(l, "@disabled", 9)) on = 0;
            continue;
        }
        if (!on || !cb) continue;
        // "<offset> <value>"
        char *p = l; uint64_t off = 0; int nd = 0;
        for (; hexval(*p) >= 0; p++, nd++) off = off << 4 | (uint64_t)hexval(*p);
        if (!nd || (*p != ' ' && *p != '\t')) { rc = -1; break; }
        while (*p == ' ' || *p == '\t') p++;
        uint32_t n = 0;
        if (*p == '"') {   // a string, C escapes
            for (p++; *p && *p != '"'; p++) {
                int c = (unsigned char)*p;
                if (c == '\\' && p[1]) { p++; c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == '0' ? 0 : (unsigned char)*p; }
                buf[n++] = (uint8_t)c;
            }
            if (*p != '"') { rc = -1; break; }
        } else {
            int nib = 0;
            for (; hexval(*p) >= 0; p++, nib++) { if (nib & 1) buf[n++] |= (uint8_t)hexval(*p); else buf[n] = (uint8_t)(hexval(*p) << 4); }
            if (!nib || (nib & 1)) { rc = -1; break; }
            while (*p == ' ' || *p == '\t') p++;
            if (*p && !(p[0] == '/' && p[1] == '/')) { rc = -1; break; }
            if (le) for (uint32_t a = 0, b = n - 1; a < b; a++, b--) { uint8_t x = buf[a]; buf[a] = buf[b]; buf[b] = x; }
        }
        long long at = (long long)off + shift - EXEFS_HEADER;
        if (at < 0 || !n) continue;   // the header
        exefs_rec_t r = { .off = (uint64_t)at, .len = n, .bytes = buf };
        if (cb(ctx, &r)) break;
    }
    free(buf);
    return rc;
}

#ifndef DM_EXEFS_PARSERS_ONLY

// ---------- the modules: their rewriter lists and writable aliases ----------

typedef struct {
    uint32_t (*changed)[2]; int nchanged, state;   // the rewriter's changed runs (sorted image offsets); state 1 read, -1 missing or of another build
    uint8_t *alias;                                // writable alias of the image, NULL when none could be made
    int alias_tried;
} exefs_mod_t;
static exefs_mod_t *g_em;

static void changed_read(module_t *m, exefs_mod_t *e) {
    if (e->state) return;
    e->state = -1;
    char p[1100]; snprintf(p, sizeof p, "%s/%s.changed.tsv", dm_module_dir(), m->name);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256], bid[40] = ""; unsigned long off, len; int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# BUILD_ID %39s", bid) == 1) continue;
        if (sscanf(line, "%lx %lx", &off, &len) != 2) continue;
        if (e->nchanged == cap) {
            cap = cap ? cap * 2 : 1024;
            uint32_t (*g)[2] = realloc(e->changed, (size_t)cap * sizeof *e->changed);
            if (!g) { fclose(f); e->nchanged = 0; return; }
            e->changed = g;
        }
        e->changed[e->nchanged][0] = (uint32_t)off; e->changed[e->nchanged][1] = (uint32_t)len; e->nchanged++;
    }
    fclose(f);
    if (strncasecmp(bid, m->build_id, 32)) { e->nchanged = 0; return; }
    e->state = 1;
}
static int changed_hit(const exefs_mod_t *e, uint64_t off, uint64_t len, uint32_t *at, uint32_t *end) {
    int lo = 0, hi = e->nchanged;
    while (lo < hi) { int mid = (lo + hi) / 2; if ((uint64_t)e->changed[mid][0] + e->changed[mid][1] <= off) lo = mid + 1; else hi = mid; }
    if (lo < e->nchanged && e->changed[lo][0] < off + len) { *at = e->changed[lo][0]; *end = e->changed[lo][0] + e->changed[lo][1]; return 1; }
    return 0;
}
#if TARGET_OS_IPHONE
static uint8_t *alias_of(module_t *m) { (void)m; return NULL; }
#else
static uint8_t *alias_of(module_t *m) {
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 0x4000;
    uint64_t size = ((uint64_t)m->size + (uint64_t)pg - 1) & ~((uint64_t)pg - 1);
    mach_vm_address_t dst = 0; vm_prot_t cur = 0, mx = 0;
    if (mach_vm_remap(mach_task_self(), &dst, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)(uintptr_t)m->base, FALSE, &cur, &mx, VM_INHERIT_SHARE)) return NULL;
    if (mach_vm_protect(mach_task_self(), dst, size, 0, VM_PROT_READ | VM_PROT_WRITE)) { mach_vm_deallocate(mach_task_self(), dst, size); return NULL; }
    return (uint8_t *)(uintptr_t)dst;
}
#endif

// ---------- one file: check every record, then write them all ----------

typedef struct {
    module_t *m; exefs_mod_t *e; const char *file;
    int writing, bad, nrec; uint64_t bytes;
    uint8_t *claimed;   // the bytes an earlier (higher priority) file wrote: a later one keeps out of them
    char why[256];
} apply_t;

static uint64_t text_pages(const module_t *m) { long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 0x4000; return (m->text_end + (uint64_t)pg - 1) & ~(uint64_t)(pg - 1); }

static int rec_cb(void *ctx, const exefs_rec_t *r) {
    apply_t *a = ctx; module_t *m = a->m;
    if (!a->writing) {
        if (r->off + r->len > m->key_page_off || r->off + r->len < r->off) { snprintf(a->why, sizeof a->why, "writes +0x%llX, past the module's image", (unsigned long long)r->off); a->bad = 1; return 1; }
        if (r->off < text_pages(m)) {
            changed_read(m, a->e);
            uint32_t s, t;
            if (a->e->state != 1) { snprintf(a->why, sizeof a->why, "writes its code at +0x%llX, and %s.changed.tsv of this build is missing (rewrite the modules with dm/tools/patch_nso.py)", (unsigned long long)r->off, m->name); a->bad = 1; return 1; }
            if (changed_hit(a->e, r->off, r->len, &s, &t)) { snprintf(a->why, sizeof a->why, "writes +0x%llX, which the rewriter changed or moved (+0x%X..+0x%X)", (unsigned long long)r->off, s, t); a->bad = 1; return 1; }
        }
        for (uint32_t k = 0; k < r->len; k++) {   // a byte a mod earlier in the list wrote must stay as it wrote it, or this file would half replace that mod's code
            uint8_t v = r->run ? r->fill : r->bytes[k];
            if (a->claimed[r->off + k] && a->e->alias[r->off + k] != v) {
                snprintf(a->why, sizeof a->why, "writes +0x%llX, which a mod higher in the list already patched differently", (unsigned long long)(r->off + k)); a->bad = 1; return 1;
            }
        }
        a->nrec++; a->bytes += r->len;
        return 0;
    }
    uint8_t *dst = a->e->alias + r->off;
    dm_guest_write_prepare(m->base + r->off, r->len);
    for (uint32_t k = 0; k < r->len; k++) { dst[k] = r->run ? r->fill : r->bytes[k]; a->claimed[r->off + k] = 1; }   // the check pass made sure a claimed byte gets its own value again
    if (r->off < text_pages(m)) sys_icache_invalidate(m->base + r->off, r->len);
    return 0;
}

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 0 || len > (64 << 20)) { fclose(f); return NULL; }
    uint8_t *d = malloc((size_t)len + 1);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    if (d) { d[len] = 0; *n = (size_t)len; }
    return d;
}

static uint8_t **g_claimed;   // per module, allocated on its first patch

static int module_ready(int i) {
    module_t *m = &g_mod[i]; exefs_mod_t *e = &g_em[i];
    if (!e->alias_tried) { e->alias_tried = 1; e->alias = alias_of(m); }
    if (!e->alias) return 0;
    if (!g_claimed[i] && !(g_claimed[i] = calloc(1, m->key_page_off))) return 0;
    return 1;
}

// One patch file against module i: 1 applied, 0 refused (logged)
static int apply_file(int i, const char *path, const uint8_t *d, size_t n, int text) {
    module_t *m = &g_mod[i];
    if (!module_ready(i)) { LOG("[RED] exefs: %s: %s", path, TARGET_OS_IPHONE ? "code patches are not possible on the iPhone (the app's signed code pages)" : "no writable alias of the module"); return 0; }
    apply_t a = { .m = m, .e = &g_em[i], .file = path, .claimed = g_claimed[i] };
    char bid[80];
    int rc = text ? exefs_parse_pchtxt((const char *)d, bid, sizeof bid, rec_cb, &a) : exefs_parse_ips(d, n, rec_cb, &a);
    if (rc) { LOG("[RED] exefs: %s: not a valid %s file, nothing applied", path, text ? "pchtxt" : "IPS"); return 0; }
    if (a.bad) { LOG("[RED] exefs: %s %s: nothing of the file applied", path, a.why); return 0; }
    a.writing = 1;
    if (text) exefs_parse_pchtxt((const char *)d, bid, sizeof bid, rec_cb, &a); else exefs_parse_ips(d, n, rec_cb, &a);
    LOG("exefs: %s → %s, %d writes, %llu bytes", path, m->name, a.nrec, (unsigned long long)a.bytes);
    return 1;
}

// A file in the folder that makes the mod one this host cannot run: a module or the program's metadata
static int is_module_file(const char *name) {
    static const char *const fixed[] = { "main", "main.npdm", "sdk", "rtld" };
    for (unsigned k = 0; k < sizeof fixed / sizeof *fixed; k++) if (!strcasecmp(name, fixed[k])) return 1;
    return !strncasecmp(name, "subsdk", 6) && isdigit((unsigned char)name[6]) && !name[7];
}

static int g_napplied;

static void apply_dir(const char *dir) {
    DIR *dp = opendir(dir);
    if (!dp) { LOG("[RED] exefs: no folder %s", dir); return; }
    struct dirent *de; char modfile[64] = "";
    char **names = NULL; int nn = 0, cap = 0;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        if (is_module_file(de->d_name)) { if (!modfile[0]) snprintf(modfile, sizeof modfile, "%s", de->d_name); continue; }
        if (nn == cap) { cap = cap ? cap * 2 : 16; char **g = realloc(names, (size_t)cap * sizeof *names); if (!g) break; names = g; }
        names[nn++] = strdup(de->d_name);
    }
    closedir(dp);
    for (int a = 1; a < nn; a++) for (int b = a; b > 0 && strcmp(names[b - 1], names[b]) > 0; b--) { char *x = names[b]; names[b] = names[b - 1]; names[b - 1] = x; }   // a fixed order
    if (modfile[0]) {
        LOG("[RED] exefs: %s holds a module (%s): the mod adds code this host does not load, so none of its patches are applied", dir, modfile);
        goto out;
    }
    for (int k = 0; k < nn; k++) {
        const char *nm = names[k]; size_t L = strlen(nm);
        int text = L > 7 && !strcasecmp(nm + L - 7, ".pchtxt"), ips = L > 4 && !strcasecmp(nm + L - 4, ".ips");
        if (!text && !ips) continue;
        char path[2048]; snprintf(path, sizeof path, "%s/%s", dir, nm);
        size_t n = 0; uint8_t *d = read_all(path, &n);
        if (!d) { LOG("[RED] exefs: cannot read %s", path); continue; }
        char bid[80] = "";
        if (text) exefs_parse_pchtxt((const char *)d, bid, sizeof bid, NULL, NULL);
        else { size_t k2 = 0; for (; k2 + 1 < sizeof bid && k2 < L - 4 && isxdigit((unsigned char)nm[k2]); k2++) bid[k2] = nm[k2]; bid[k2] = 0; }
        int hit = 0;
        for (int i = 0; i < g_nmod; i++) {
            if (g_mod[i].nro || !g_mod[i].build_id[0] || !exefs_bid_match(bid, strlen(bid), g_mod[i].build_id)) continue;
            hit = 1; g_napplied += apply_file(i, path, d, n, text);
            break;
        }
        if (!hit) LOG("exefs: %s is for build %s, which no module of this game has: not applied", path, bid[0] ? bid : "(none named)");
        free(d);
    }
out:
    for (int k = 0; k < nn; k++) free(names[k]);
    free(names);
}

// A mod's code can reach an SDK function without importing it: Ocarina of Time's Fast Travel mod finds nn::fs::MountSdCard in the sdk module at run time and calls
// its address, which main never imports. The host takes such functions over by binding the imports that name them (loader.c, HLE_FORCE), so that call ran the SDK's
// own definition, which opened fsp-srv and aborted (Result 0xE15). When patches were applied, each take-over no import bound whose SDK definition goes through
// fsp-srv (its reason starts "fsp-srv"; the host never serves that service, so the definition could only abort) gets its definition's first 16 bytes replaced with a
// jump to the host's function (ldr x16, #8; br x16; the address), written through the alias as a patch. A definition the rewriter changed there, or that a mod
// patched itself, is left as it is
typedef struct { int n; } redirect_t;
static void redirect_cb(void *ctx, const char *name, void *fn, const char *why) {
    if (strncmp(why, "fsp-srv", 7)) return;
    redirect_t *r = ctx;
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (m->nro) continue;
        uint8_t *def = module_sym(m, name);
        if (!def || def < m->base || (uint64_t)(def - m->base) + 16 > m->text_end) continue;
        uint64_t off = (uint64_t)(def - m->base); uint32_t s, t;
        if (!module_ready(i)) return;
        changed_read(m, &g_em[i]);
        if (g_em[i].state != 1 || changed_hit(&g_em[i], off, 16, &s, &t)) { LOG("[RED] exefs: %s+0x%llX %s: the rewriter changed its start, so a mod's call to it runs the SDK's own definition", m->name, (unsigned long long)off, name); continue; }
        int mine = 0; for (int k = 0; k < 16; k++) mine |= g_claimed[i][off + k];
        if (mine) continue;
        uintptr_t to = (uintptr_t)fn;
        uint32_t w[4] = { 0x58000050u, 0xD61F0200u, (uint32_t)to, (uint32_t)(to >> 32) };   // ldr x16, #8; br x16; .quad fn
        dm_guest_write_prepare(m->base + off, 16);
        memcpy(g_em[i].alias + off, w, 16);
        sys_icache_invalidate(m->base + off, 16);
        r->n++;
    }
}

// After load_modules (the images are in place and relocated), before the game's first instruction (main.c)
void dm_exefs_init(void) {
    const char *list = getenv("DM_EXEFS_PATCHES");
    if (!list || !*list) return;
    g_em = calloc((size_t)g_nmod, sizeof *g_em); g_claimed = calloc((size_t)g_nmod, sizeof *g_claimed);
    if (!g_em || !g_claimed) die("exefs: allocation failed");
    char *dirs = strdup(list), *save = NULL;
    for (char *d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) if (*d) apply_dir(d);
    free(dirs);
    LOG("exefs: %d patch file%s applied", g_napplied, g_napplied == 1 ? "" : "s");
    if (g_napplied) {
        void dm_force_unbound(void (*cb)(void *, const char *, void *, const char *), void *ctx);
        redirect_t r = {0}; dm_force_unbound(redirect_cb, &r);
        if (r.n) LOG("exefs: %d SDK file system function%s the game does not import now run%s in the host, for the mods' own calls", r.n, r.n == 1 ? "" : "s", r.n == 1 ? "s" : "");
    }
    for (int i = 0; i < g_nmod; i++) { free(g_em[i].changed); free(g_claimed[i]); }   // the aliases stay mapped: nothing reads them again, and unmapping is not worth a code path
    free(g_em); free(g_claimed); g_em = NULL; g_claimed = NULL;
}

#endif
