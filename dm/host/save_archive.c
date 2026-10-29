// Save data as one file: pack the committed saves into an archive and restore one. See save_archive.h for the
// contract and docs/playing.md ("Save data") for the player's side. The format is here, not in a second tool:
//
//   header, 32 bytes    "NXSAVE\0\0" (8), uint32 version = 1, uint32 flags = 0,
//                       uint64 title id (0 = not recorded), uint32 entry count, uint32 reserved = 0
//   entry, repeated     uint8 kind (1 file, 2 directory), uint8 reserved, uint16 name length,
//                       uint32 data length (files; 0 for a directory), uint32 reserved,
//                       name (name length bytes), data (data length bytes), uint32 CRC-32 of the data (files)
//
// Every integer is little-endian. Names are guest paths: "save" and "save/…" for the account save,
// "save_device" and "save_device/…" for a device save, relative, '/'-separated, without "." or "..". Entries are
// sorted by name bytes and unique, and every file's folder is an entry of its own before it. A file's CRC-32 is
// IEEE 802.3 (the one zlib and PNG use). Nothing after the last entry is accepted, so a file cut short or joined to
// another is a format error.
//
// Why a format of its own: a save is already a tree of files, so a container only has to carry names, kinds, bytes
// and a checksum, and the reader here can be as strict as the writer. It is not a zip, so the launchers need
// neither a compression library nor a deflate implementation, and the archive is byte-for-byte reproducible from
// the same save (sorted, no timestamps).
//
// The names come from the guest, not the host: dm/host/hle_fs.c keeps the guest name in the
// org.d2rmobile.guest-name xattr when APFS's case-insensitivity forces another physical name (.dm-case-N). Import
// resolves components the same way, so a tree written on a case-sensitive volume is read back on an iPhone.
#include "save_archive.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define ARCHIVE_MAGIC "NXSAVE"   // then two 0 bytes: the first eight bytes of the file
#define ARCHIVE_VERSION 1u
#define ARCHIVE_MAX_PATH 1024
#define ARCHIVE_MAX_ENTRIES 200000u
#define ARCHIVE_CHUNK (256u << 10)
#define FS_GUEST_NAME "org.d2rmobile.guest-name"   // as hle_fs.c: the guest name of a case-collision alias
#define KIND_FILE 1
#define KIND_DIR 2

// A front end calls these before the game starts, so no mount holds the save; the host gives the real answer
// (hle_fs.c) and the Mac launcher, which links this file without the host, keeps the weak one. Packing or replacing
// a save underneath a mounted one would read or overwrite the tree the game is still writing into.
__attribute__((weak)) int dm_fs_save_in_use(void) { return 0; }

typedef struct { char *path; char *host; uint64_t size; uint8_t kind; } ent;
typedef struct { ent *v; size_t n, cap; } ents;

static int fail(int code, char *err, size_t n, const char *fmt, ...) {
    if (err && n) { va_list ap; va_start(ap, fmt); vsnprintf(err, n, fmt, ap); va_end(ap); }
    return code;
}
static void note(const char *fmt, ...) {   // the launchers' log (stderr), one line per step
    va_list ap; va_start(ap, fmt); fputs("save data: ", stderr); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}

static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const unsigned char *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const unsigned char *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

// ---------- checksum ----------
static uint32_t g_crc_table[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_table[i] = c; }
}
static uint32_t crc_update(uint32_t c, const void *buf, size_t n) {
    pthread_once(&g_crc_once, crc_init);
    const unsigned char *p = buf;
    while (n--) c = g_crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

// ---------- host paths ----------
// DM_SAVE as the host uses it, with trailing slashes removed (.work / .new / .pre-import live beside it)
static int save_base(char *out, size_t n, char *err, size_t errlen) {
    const char *d = getenv("DM_SAVE");
    if (!d || !*d) return fail(DM_SAVE_ERR_USAGE, err, errlen, "The save folder is not set (DM_SAVE): start the game from its launcher.");
    size_t L = strlen(d);
    if (L >= n) return fail(DM_SAVE_ERR_USAGE, err, errlen, "The save folder path is too long.");
    memcpy(out, d, L + 1);
    while (L > 1 && out[L - 1] == '/') out[--L] = 0;
    return DM_SAVE_OK;
}
static int is_dir(const char *p) { struct stat st; return !lstat(p, &st) && S_ISDIR(st.st_mode); }
static int join(char *out, size_t n, const char *dir, const char *name) {
    int k = snprintf(out, n, "%s/%s", dir, name);
    if (k < 0 || (size_t)k >= n) { errno = ENAMETOOLONG; return 0; }
    return 1;
}
static int mkdir_p(const char *p) {   // every missing folder of a path (Documents on iOS, Application Support on the Mac)
    char buf[ARCHIVE_MAX_PATH];
    if (strlen(p) >= sizeof buf) { errno = ENAMETOOLONG; return 0; }
    strcpy(buf, p);
    for (char *s = buf + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(buf, 0755) && errno != EEXIST) return 0; *s = '/'; }
    if (mkdir(buf, 0755) && errno != EEXIST) return 0;
    return 1;
}
static int rm_tree(const char *p) {   // files unlink, folders recurse then rmdir; a missing path is fine
    struct stat st;
    if (lstat(p, &st)) return errno == ENOENT ? 1 : 0;
    if (!S_ISDIR(st.st_mode)) return unlink(p) == 0;
    DIR *d = opendir(p);
    if (d) {
        struct dirent *e; int ok = 1;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[ARCHIVE_MAX_PATH];
            if (!join(sub, sizeof sub, p, e->d_name) || !rm_tree(sub)) { ok = 0; break; }
        }
        closedir(d);
        if (!ok) return 0;
    }
    return rmdir(p) == 0 || errno == ENOENT;
}
// The guest name of a directory entry: the physical name unless hle_fs.c chose a .dm-case-N alias for a case collision
static int guest_name_of(const char *parent, const char *physical, char *out, size_t n) {
    if (strncmp(physical, ".dm-case-", 9)) {
        if (strlen(physical) >= n) { errno = ENAMETOOLONG; return 0; }
        strcpy(out, physical); return 1;
    }
    char p[ARCHIVE_MAX_PATH];
    if (!join(p, sizeof p, parent, physical)) return 0;
    ssize_t k = getxattr(p, FS_GUEST_NAME, out, n - 1, 0, XATTR_NOFOLLOW);
    if (k >= 0) { out[k] = 0; return 1; }
    if (errno != ENOATTR) return 0;
    if (strlen(physical) >= n) { errno = ENAMETOOLONG; return 0; }
    strcpy(out, physical); return 1;
}

// ---------- packing ----------
static void ents_free(ents *l) { for (size_t i = 0; i < l->n; i++) { free(l->v[i].path); free(l->v[i].host); } free(l->v); l->v = NULL; l->n = l->cap = 0; }
static int ents_add(ents *l, const char *path, const char *host, uint8_t kind, uint64_t size, char *err, size_t errlen) {
    if (l->n >= ARCHIVE_MAX_ENTRIES) return fail(DM_SAVE_ERR_IO, err, errlen, "The save has more than %u files and folders.", ARCHIVE_MAX_ENTRIES);
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 64;
        ent *v = realloc(l->v, cap * sizeof *v);
        if (!v) return fail(DM_SAVE_ERR_IO, err, errlen, "Out of memory.");
        l->v = v; l->cap = cap;
    }
    ent *e = &l->v[l->n];
    if (!(e->path = strdup(path)) || !(e->host = strdup(host))) { free(e->path); e->path = NULL; return fail(DM_SAVE_ERR_IO, err, errlen, "Out of memory."); }
    e->kind = kind; e->size = size; l->n++;
    return DM_SAVE_OK;
}
// One committed save tree, depth-first, names from the guest (guest_name_of). Symlinks and special files are
// skipped with a line: a save is files and folders, and following a link out of the save would pack something else.
static int walk_tree(ents *l, const char *dir, const char *prefix, int depth, char *err, size_t errlen) {
    if (depth > 32) return fail(DM_SAVE_ERR_IO, err, errlen, "The save folder nests deeper than 32 levels.");
    DIR *d = opendir(dir);
    if (!d) return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot read %s: %s.", dir, strerror(errno));
    struct dirent *e; int rc = DM_SAVE_OK;
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char name[ARCHIVE_MAX_PATH], host[ARCHIVE_MAX_PATH], path[ARCHIVE_MAX_PATH];
        struct stat st;
        if (!guest_name_of(dir, e->d_name, name, sizeof name) || !join(host, sizeof host, dir, e->d_name)) {
            rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot read a name in %s: %s.", dir, strerror(errno)); break;
        }
        if ((size_t)snprintf(path, sizeof path, "%s/%s", prefix, name) >= sizeof path) {
            rc = fail(DM_SAVE_ERR_IO, err, errlen, "The save has a path longer than %d bytes.", ARCHIVE_MAX_PATH); break;
        }
        if (lstat(host, &st)) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot read %s: %s.", host, strerror(errno)); break; }
        if (S_ISDIR(st.st_mode)) {
            if ((rc = ents_add(l, path, host, KIND_DIR, 0, err, errlen))) break;
            rc = walk_tree(l, host, path, depth + 1, err, errlen);
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_size < 0 || (uint64_t)st.st_size > 0xffffffffull) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "%s is too large for a save file (4 GiB).", path); break; }
            rc = ents_add(l, path, host, KIND_FILE, (uint64_t)st.st_size, err, errlen);
        } else note("skipping %s: not a file or folder", path);
    }
    closedir(d);
    return rc;
}
static int ent_cmp(const void *a, const void *b) { return strcmp(((const ent *)a)->path, ((const ent *)b)->path); }
static int write_all(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }

// One archive, written whole. A file's entry header carries its size; its CRC follows the data, so packing streams
// (no seeking back over a header, no second pass over the file).
static int write_archive(FILE *f, const ents *l, uint64_t title_id, char *err, size_t errlen) {
    unsigned char h[32];
    memcpy(h, ARCHIVE_MAGIC, 6); h[6] = h[7] = 0;
    put32(h + 8, ARCHIVE_VERSION); put32(h + 12, 0); put64(h + 16, title_id); put32(h + 24, (uint32_t)l->n); put32(h + 28, 0);
    if (!write_all(f, h, sizeof h)) return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write the archive: %s.", strerror(errno));
    unsigned char *buf = malloc(ARCHIVE_CHUNK);
    if (!buf) return fail(DM_SAVE_ERR_IO, err, errlen, "Out of memory.");
    int rc = DM_SAVE_OK;
    for (size_t i = 0; !rc && i < l->n; i++) {
        const ent *e = &l->v[i];
        size_t nl = strlen(e->path);
        unsigned char eh[12];
        eh[0] = e->kind; eh[1] = 0; put16(eh + 2, (uint16_t)nl); put32(eh + 4, (uint32_t)e->size); put32(eh + 8, 0);
        if (!write_all(f, eh, sizeof eh) || !write_all(f, e->path, nl)) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write the archive: %s.", strerror(errno)); break; }
        if (e->kind != KIND_FILE) continue;
        FILE *in = fopen(e->host, "rb");
        if (!in) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot read %s: %s.", e->host, strerror(errno)); break; }
        uint32_t crc = 0xffffffffu; uint64_t left = e->size; int io = 1;
        while (left) {
            size_t want = left < ARCHIVE_CHUNK ? (size_t)left : ARCHIVE_CHUNK;
            size_t got = fread(buf, 1, want, in);
            if (!got) { if (!ferror(in)) errno = EIO; break; }
            crc = crc_update(crc, buf, got);
            if (!write_all(f, buf, got)) break;
            left -= got;
        }
        io = !left && !ferror(in);
        fclose(in);
        if (!io) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot read %s: %s.", e->host, strerror(errno)); break; }
        unsigned char tail[4]; put32(tail, crc ^ 0xffffffffu);
        if (!write_all(f, tail, sizeof tail)) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write the archive: %s.", strerror(errno)); break; }
    }
    free(buf);
    return rc;
}

int dm_save_export(const char *path, uint64_t title_id, char *err, size_t errlen) {
    if (!path || !*path) return fail(DM_SAVE_ERR_USAGE, err, errlen, "No file to write the save data to.");
    if (dm_fs_save_in_use()) return fail(DM_SAVE_ERR_USAGE, err, errlen, "The game is running: export its save data from the launcher instead.");
    char base[ARCHIVE_MAX_PATH];
    int rc = save_base(base, sizeof base, err, errlen);
    if (rc) return rc;
    ents l = {0}; int have_save = 0;
    if (is_dir(base)) {
        if ((rc = ents_add(&l, "save", base, KIND_DIR, 0, err, errlen))) goto out;
        if ((rc = walk_tree(&l, base, "save", 0, err, errlen))) goto out;
        have_save = 1;
    }
    char dev[ARCHIVE_MAX_PATH];
    if ((size_t)snprintf(dev, sizeof dev, "%s_device", base) < sizeof dev && is_dir(dev)) {
        if ((rc = ents_add(&l, "save_device", dev, KIND_DIR, 0, err, errlen))) goto out;
        if ((rc = walk_tree(&l, dev, "save_device", 0, err, errlen))) goto out;
    }
    if (!have_save) { rc = fail(DM_SAVE_ERR_EMPTY, err, errlen, "No save data to export yet: this game has not saved on this device."); goto out; }
    int files = 0;
    for (size_t i = 0; i < l.n; i++) if (l.v[i].kind == KIND_FILE) { files = 1; break; }
    if (!files) { rc = fail(DM_SAVE_ERR_EMPTY, err, errlen, "No save data to export yet: this game has not saved on this device."); goto out; }
    qsort(l.v, l.n, sizeof *l.v, ent_cmp);
    for (size_t i = 1; i < l.n; i++) if (!strcmp(l.v[i - 1].path, l.v[i].path)) {   // two host names with one guest name: refuse rather than lose one
        rc = fail(DM_SAVE_ERR_IO, err, errlen, "The save has two entries named %s.", l.v[i].path); goto out;
    }
    char tmp[ARCHIVE_MAX_PATH];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "The export path is too long."); goto out; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", tmp, strerror(errno)); goto out; }
    rc = write_archive(f, &l, title_id, err, errlen);
    if (!rc && fflush(f)) rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", tmp, strerror(errno));
    if (!rc && fsync(fileno(f))) rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", tmp, strerror(errno));
    if (fclose(f) && !rc) rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", tmp, strerror(errno));
    if (!rc && rename(tmp, path)) rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", path, strerror(errno));
    if (rc) unlink(tmp);
    else note("exported %zu entries to %s", l.n, path);
out:
    ents_free(&l);
    return rc;
}

// ---------- restoring ----------
static int read_all(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
// The root of an entry ("save" or "save_device"), created under the staging folder; NULL for a name outside both
static const char *name_root(const char *p, const char **rest) {
    if (!strncmp(p, "save/", 5)) { *rest = p + 5; return "save"; }
    if (!strcmp(p, "save")) { *rest = ""; return "save"; }
    if (!strncmp(p, "save_device/", 12)) { *rest = p + 12; return "save_device"; }
    if (!strcmp(p, "save_device")) { *rest = ""; return "save_device"; }
    return NULL;
}
static int valid_name(const char *p) {
    const char *rest, *root = name_root(p, &rest);
    if (!root) return 0;
    size_t rl = strlen(root);
    if (p[rl] == '/' && !p[rl + 1]) return 0;   // "save/" is not the root itself, and has nothing after it
    while (*rest) {
        const char *end = strchr(rest, '/');
        size_t k = end ? (size_t)(end - rest) : strlen(rest);
        if (!k || (k == 1 && rest[0] == '.') || (k == 2 && rest[0] == '.' && rest[1] == '.')) return 0;
        if (!end) break;
        rest = end + 1;
        if (!*rest) return 0;   // a trailing slash
    }
    return 1;
}
// The physical name of the entry whose guest name is `name` in `parent`; 1 with the name in physical, or 0
static int find_component(const char *parent, const char *name, char *physical, size_t n) {
    DIR *d = opendir(parent);
    if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char guest[ARCHIVE_MAX_PATH];
        if (!guest_name_of(parent, e->d_name, guest, sizeof guest)) continue;
        if (!strcmp(guest, name)) { snprintf(physical, n, "%s", e->d_name); found = 1; break; }
    }
    closedir(d);
    return found;
}
// A new physical name for `name` under `parent`: the guest name, or .dm-case-N when APFS matches that name to a
// different entry (the caller puts the guest name in the alias's xattr once it exists)
static int make_component(const char *parent, const char *name, char *physical, size_t n) {
    snprintf(physical, n, "%s", name);
    char probe[ARCHIVE_MAX_PATH]; struct stat st;
    if (!join(probe, sizeof probe, parent, physical)) return 0;
    if (!lstat(probe, &st)) {
        for (unsigned i = 1; i < 100000; i++) {
            snprintf(physical, n, ".dm-case-%u", i);
            if (!join(probe, sizeof probe, parent, physical)) return 0;
            if (lstat(probe, &st)) break;
        }
    }
    return 1;
}
// The staged path of one entry. `dir` says whether the entry is a folder (created) or a file (named, not created).
// Folders before the last component must exist already: the archive is sorted, and a listed folder's entry precedes
// its contents.
static int staged_path(const char *staging, const char *name, int dir, int *is_root, char *out, size_t n, char *err, size_t errlen) {
    const char *rest, *root = name_root(name, &rest);
    char root_path[ARCHIVE_MAX_PATH], path[ARCHIVE_MAX_PATH];
    if (!root || !join(root_path, sizeof root_path, staging, root)) return fail(DM_SAVE_ERR_IO, err, errlen, "The save has a path longer than %d bytes.", ARCHIVE_MAX_PATH);
    if (mkdir(root_path, 0755) && errno != EEXIST) return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot create %s: %s.", root_path, strerror(errno));
    *is_root = !*rest;
    snprintf(path, sizeof path, "%s", root_path);
    if (*is_root) { snprintf(out, n, "%s", path); return DM_SAVE_OK; }
    while (*rest) {
        const char *end = strchr(rest, '/');
        size_t k = end ? (size_t)(end - rest) : strlen(rest);
        char part[ARCHIVE_MAX_PATH], physical[ARCHIVE_MAX_PATH], full[ARCHIVE_MAX_PATH];
        memcpy(part, rest, k); part[k] = 0;
        int last = !end;
        if (!find_component(path, part, physical, sizeof physical)) {
            if (!last) return fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data lists %s before its folder.", name);
            if (!make_component(path, part, physical, sizeof physical) || !join(full, sizeof full, path, physical))
                return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot create a name in %s: %s.", path, strerror(errno));
        } else if (!join(full, sizeof full, path, physical)) return fail(DM_SAVE_ERR_IO, err, errlen, "The save has a path longer than %d bytes.", ARCHIVE_MAX_PATH);
        if (last && dir && mkdir(full, 0755) && errno != EEXIST) return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot create %s: %s.", full, strerror(errno));
        snprintf(path, sizeof path, "%s", full);
        if (!end) break;
        rest = end + 1;
    }
    snprintf(out, n, "%s", path);
    return DM_SAVE_OK;
}
// A fresh name in the staging folder is the guest name unless it had to become a .dm-case-N alias; give the alias
// the guest name back (hle_fs.c's mapping), so the game reads the name the archive carried.
static int fix_guest_name(const char *host, const char *name) {
    const char *leaf = strrchr(name, '/'); leaf = leaf ? leaf + 1 : name;
    const char *physical = strrchr(host, '/'); physical = physical ? physical + 1 : host;
    if (!strcmp(leaf, physical)) return 1;
    return setxattr(host, FS_GUEST_NAME, leaf, strlen(leaf), 0, XATTR_NOFOLLOW) == 0;
}
typedef struct { char staged[ARCHIVE_MAX_PATH], target[ARCHIVE_MAX_PATH], backup[ARCHIVE_MAX_PATH]; int moved; } swap_t;
static int swap_in(swap_t *s, int count, char *err, size_t errlen) {
    // Two renames per save: the committed tree aside as <save>.pre-import, the staged one into its place. A failure
    // puts back every tree already moved, so a refused import leaves the saves as they were.
    for (int i = 0; i < count; i++) {
        s[i].moved = 0;
        char work[ARCHIVE_MAX_PATH], neu[ARCHIVE_MAX_PATH];
        snprintf(work, sizeof work, "%s.work", s[i].target); snprintf(neu, sizeof neu, "%s.new", s[i].target);
        // the mount clears these at the next one anyway; clearing them here means it cannot fail on them
        if ((!access(work, F_OK) && !rm_tree(work)) || (!access(neu, F_OK) && !rm_tree(neu)))
            { fail(DM_SAVE_ERR_IO, err, errlen, "Cannot clear a leftover working save: %s.", strerror(errno)); goto rollback; }
        if (!access(s[i].target, F_OK)) {
            if (!access(s[i].backup, F_OK) && !rm_tree(s[i].backup)) { fail(DM_SAVE_ERR_IO, err, errlen, "Cannot replace %s: %s.", s[i].backup, strerror(errno)); goto rollback; }
            if (rename(s[i].target, s[i].backup)) { fail(DM_SAVE_ERR_IO, err, errlen, "Cannot move %s aside: %s.", s[i].target, strerror(errno)); goto rollback; }
            s[i].moved = 1;
        }
        if (rename(s[i].staged, s[i].target)) {
            fail(DM_SAVE_ERR_IO, err, errlen, "Cannot move the imported save to %s: %s.", s[i].target, strerror(errno));
            goto rollback;
        }
        s[i].moved = 2;   // the staged tree is in place, the old one (if any) in the backup
    }
    return DM_SAVE_OK;
rollback:
    for (int i = 0; i < count; i++) {
        if (s[i].moved == 2) { rename(s[i].target, s[i].staged); if (!access(s[i].backup, F_OK)) rename(s[i].backup, s[i].target); }
        else if (s[i].moved == 1) rename(s[i].backup, s[i].target);
        s[i].moved = 0;
    }
    return DM_SAVE_ERR_IO;
}

int dm_save_import(const char *path, uint64_t title_id, char *err, size_t errlen) {
    if (!path || !*path) return fail(DM_SAVE_ERR_USAGE, err, errlen, "No save data file was chosen.");
    if (dm_fs_save_in_use()) return fail(DM_SAVE_ERR_USAGE, err, errlen, "The game is running: import save data from the launcher instead.");
    char base[ARCHIVE_MAX_PATH];
    int rc = save_base(base, sizeof base, err, errlen);
    if (rc) return rc;
    FILE *f = fopen(path, "rb");
    if (!f) return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot read %s: %s.", path, strerror(errno));
    unsigned char h[32];
    if (!read_all(f, h, sizeof h) || memcmp(h, ARCHIVE_MAGIC, 6) || h[6] || h[7]) { fclose(f); return fail(DM_SAVE_ERR_FORMAT, err, errlen, "This file is not save data (it does not start with an nxnative save archive)."); }
    uint32_t version = get32(h + 8), flags = get32(h + 12), count = get32(h + 24);
    uint64_t title = get64(h + 16);
    if (version != ARCHIVE_VERSION || flags) { fclose(f); return fail(DM_SAVE_ERR_FORMAT, err, errlen, "This save data was written by another version of nxnative (format %u).", version); }
    if (title && title_id && title != title_id) { fclose(f); return fail(DM_SAVE_ERR_GAME, err, errlen, "This save data belongs to another game (%016llX; this app is %016llX).", (unsigned long long)title, (unsigned long long)title_id); }
    if (!count || count > ARCHIVE_MAX_ENTRIES) { fclose(f); return fail(DM_SAVE_ERR_FORMAT, err, errlen, "This save data lists no files, or too many."); }
    char parent[ARCHIVE_MAX_PATH];
    snprintf(parent, sizeof parent, "%s", base);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0; else snprintf(parent, sizeof parent, ".");
    if (!mkdir_p(parent)) { fclose(f); return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot create %s: %s.", parent, strerror(errno)); }
    char staging[ARCHIVE_MAX_PATH];
    if ((size_t)snprintf(staging, sizeof staging, "%s.import", base) >= sizeof staging) { fclose(f); return fail(DM_SAVE_ERR_IO, err, errlen, "The save path is too long."); }
    if (!rm_tree(staging) || mkdir(staging, 0755)) { fclose(f); return fail(DM_SAVE_ERR_IO, err, errlen, "Cannot prepare %s: %s.", staging, strerror(errno)); }
    int have_save = 0, have_device = 0;
    char prev[ARCHIVE_MAX_PATH] = "";
    unsigned char *buf = malloc(ARCHIVE_CHUNK);
    if (!buf) { fclose(f); rm_tree(staging); return fail(DM_SAVE_ERR_IO, err, errlen, "Out of memory."); }
    for (uint32_t i = 0; !rc && i < count; i++) {
        unsigned char eh[12], crc_bytes[4];
        if (!read_all(f, eh, sizeof eh)) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data is cut short (entry %u of %u).", i + 1, count); break; }
        uint8_t kind = eh[0]; size_t nl = get16(eh + 2); uint32_t dl = get32(eh + 4);
        if ((kind != KIND_FILE && kind != KIND_DIR) || (kind == KIND_DIR && dl)) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data has an entry of an unknown kind."); break; }
        char name[ARCHIVE_MAX_PATH + 1];
        if (!nl || nl >= sizeof name || !read_all(f, name, nl)) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data has an entry with no name, or is cut short."); break; }
        name[nl] = 0;
        if (!valid_name(name)) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data has an entry outside save/ and save_device/: %s", name); break; }
        if (prev[0] && strcmp(prev, name) >= 0) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data's entries are not sorted and unique (%s).", name); break; }
        snprintf(prev, sizeof prev, "%s", name);
        int is_root = 0; char subject[ARCHIVE_MAX_PATH];
        if ((rc = staged_path(staging, name, kind == KIND_DIR, &is_root, subject, sizeof subject, err, errlen))) break;
        if (is_root) {   // "save" / "save_device" themselves
            if (kind != KIND_DIR) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data has a file where a folder belongs: %s", name); break; }
            if (!strcmp(name, "save")) have_save = 1; else have_device = 1;
            continue;
        }
        if (!strncmp(name, "save/", 5)) have_save = 1; else have_device = 1;
        if (kind == KIND_DIR) {
            if (!fix_guest_name(subject, name)) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot name %s: %s.", subject, strerror(errno)); break; }
            continue;
        }
        int fd = open(subject, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", subject, strerror(errno)); break; }
        uint32_t crc = 0xffffffffu, left = dl; int io = 1, cut = 0;
        while (left) {
            size_t want = left < ARCHIVE_CHUNK ? left : ARCHIVE_CHUNK;
            if (!read_all(f, buf, want)) { cut = 1; break; }
            crc = crc_update(crc, buf, want);
            for (size_t off = 0; off < want; ) { ssize_t w = write(fd, buf + off, want - off); if (w < 0) { if (errno == EINTR) continue; io = 0; break; } off += (size_t)w; }
            if (!io) break;
            left -= (uint32_t)want;
        }
        if (cut) { close(fd); rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data is cut short (%s).", name); break; }
        if (!io) { close(fd); rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", subject, strerror(errno)); break; }
        if (!read_all(f, crc_bytes, sizeof crc_bytes)) { close(fd); rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data is cut short (%s).", name); break; }
        int s1 = fsync(fd), s2 = close(fd);
        if (s1 || s2) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot write %s: %s.", subject, strerror(errno)); break; }
        if (!fix_guest_name(subject, name)) { rc = fail(DM_SAVE_ERR_IO, err, errlen, "Cannot name %s: %s.", subject, strerror(errno)); break; }
        if ((crc ^ 0xffffffffu) != get32(crc_bytes)) { rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data is damaged: the checksum of %s does not match.", name); break; }
    }
    int extra = !rc && (fgetc(f) != EOF || ferror(f));
    fclose(f); free(buf);
    if (!rc && extra) rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data has bytes after its last entry.");
    if (!rc && !have_save) rc = fail(DM_SAVE_ERR_FORMAT, err, errlen, "The save data has no save folder.");
    if (rc) { rm_tree(staging); return rc; }
    swap_t sw[2]; int n = 0;
    snprintf(sw[n].staged, sizeof sw[n].staged, "%s/save", staging); snprintf(sw[n].target, sizeof sw[n].target, "%s", base);
    n++;
    if (have_device) {
        snprintf(sw[n].staged, sizeof sw[n].staged, "%s/save_device", staging);
        snprintf(sw[n].target, sizeof sw[n].target, "%s_device", base);
        n++;
    }
    for (int i = 0; i < n; i++) snprintf(sw[i].backup, sizeof sw[i].backup, "%s.pre-import", sw[i].target);
    rc = swap_in(sw, n, err, errlen);
    if (!rc) note("imported %u entries from %s into %s%s", count, path, base, have_device ? " and its device save" : "");
    rm_tree(staging);
    return rc;
}
