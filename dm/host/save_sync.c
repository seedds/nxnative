// Saves through a cloud folder (iCloud Drive): see save_sync.h. Plain C on system calls and CommonCrypto only, so the Mac launcher links it as the host does (dm/mac/pack_app.py).
// A packed save (<folder>/<title ID>/save.dmsync), little-endian:
//   header  "DMSYNC1\n", u32 version (1), u32 entries, 32 hex digits of the hash, i64 saved, char title[24], char device[64]
//   entry   u8 type (1 folder, 2 file), u8 0, u16 path length, u16 guest-name length, u16 0, u64 size, i64 mtime; the path (save/... or save_device/...), the guest name, the contents
//   end     "DMSYNCE\n", then the end of the file (a file cut off anywhere is refused)
// The hash runs over each entry's first 16 header bytes (not the date), its path, its guest name and its contents, in the order walk() visits them.
#include "save_sync.h"
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define SLOG(...) do { fprintf(stderr, "[dm] save sync: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define SRED(...) do { fprintf(stderr, "[dm] [RED] save sync: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define GUEST_NAME "org.d2rmobile.guest-name"   // hle_fs.c FS_GUEST_NAME: a file's guest name when its host name had to differ (a case collision); part of the save
#define QUIET_MS 3000    // a game often commits several times in a row: the sender waits until it has been quiet this long
#define FETCH_MS 10000   // the longest the game waits for the cloud's copy (a download); then it plays on with this device's save, which is not sent until the two have been compared
#define KEEP_BACKUPS 10  // backups kept of each side (local, cloud); older ones are removed when a new one is made
enum { HDR = 8 + 4 + 4 + SS_HASH + 8 + 24 + 64, ENT = 24, CHUNK = 1 << 16 };   // header bytes, entry header bytes, the copy buffer
static const char MAGIC[8] __attribute__((nonstring)) = "DMSYNC1\n", END[8] __attribute__((nonstring)) = "DMSYNCE\n";
static const char *const FOLDERS[2] = {"save", "save_device"};   // their names in a packed save; on disk DM_SAVE and DM_SAVE_device (hle_fs.c h_fs_MountDeviceSave)

// ---------- files ----------
static void folder_path(const char *save, int i, char *out, size_t n) { snprintf(out, n, i ? "%s_device" : "%s", save); }

static void free_names(char **v, size_t n) { for (size_t i = 0; i < n; i++) free(v[i]); free(v); }
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
// A folder's names in byte order, without . and ..; -1 (errno set) when it cannot be read, all of it: a readdir that fails partway is a failure, not the folder's end
static int names_of(const char *dir, char ***out, size_t *count) {
    DIR *d = opendir(dir); if (!d) return -1;
    char **v = NULL; size_t n = 0, cap = 0; struct dirent *e; int r = 0, err = 0;
    while ((errno = 0, e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { char **w = realloc(v, (cap = cap ? cap * 2 : 16) * sizeof *v); if (!w) { r = -1; err = ENOMEM; break; } v = w; }
        if (!(v[n] = strdup(e->d_name))) { r = -1; err = ENOMEM; break; }
        n++;
    }
    if (!r && errno) { r = -1; err = errno; }   // readdir's NULL with errno set
    closedir(d);
    if (r) { free_names(v, n); errno = err; return -1; }
    if (n) qsort(v, n, sizeof *v, by_name);
    *out = v; *count = n; return 0;
}

static int mkdirs(const char *path) {   // mkdir -p
    char p[PATH_MAX]; if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return -1;
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}

static int rm_tree(const char *p) {   // rm -rf: 0 when nothing is left
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(p);
    char **v; size_t n; if (names_of(p, &v, &n)) return -1;
    int r = 0;
    for (size_t i = 0; i < n; i++) { char c[PATH_MAX]; snprintf(c, sizeof c, "%s/%s", p, v[i]); if (rm_tree(c)) r = -1; }
    free_names(v, n);
    return rmdir(p) ? -1 : r;
}

// Anything in it but Finder's .DS_Store: an empty folder is no save (the host makes DM_SAVE at every mount, DM_SAVE_device at the device save's).
// 1: a save; 0: none (no folder, or an empty one); -1 (errno set): a folder that cannot be read, which is not "no save": a pack, a send or a pull that took it
// for none would leave this device's save out (a send would replace a cloud save that has it with one that does not)
static int has_entries(const char *dir) {
    char **v; size_t n, k = 0; if (names_of(dir, &v, &n)) return errno == ENOENT ? 0 : -1;
    for (size_t i = 0; i < n; i++) k += strcmp(v[i], ".DS_Store") != 0;
    free_names(v, n); return k > 0;
}

static int write_all(int fd, const void *b, size_t n) {
    for (const char *p = b; n; ) { ssize_t k = write(fd, p, n); if (k < 0 && errno == EINTR) continue; if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}

static int copy_fd(int in, int out) {
    char *b = malloc(CHUNK); if (!b) return -1;
    ssize_t k; int r = 0;
    while ((k = read(in, b, CHUNK)) != 0) { if (k < 0) { if (errno == EINTR) continue; r = -1; break; } if (write_all(out, b, (size_t)k)) { r = -1; break; } }
    free(b); return r;
}

static void hex(const uint8_t *d, char *out) { for (int i = 0; i < SS_HASH / 2; i++) snprintf(out + 2 * i, 3, "%02x", d[i]); }

static void when(int64_t t, char *out, size_t n) {   // a save's time in the log
    time_t tt = (time_t)t; struct tm tm;
    if (!t || !localtime_r(&tt, &tm) || !strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm)) snprintf(out, n, "unknown");
}

static void device_name(char *out, size_t n) {   // the model ("Mac16,1", "iPhone17,1"): which device a save came from, for the log
    char model[64] = ""; size_t len = sizeof model;
    if (sysctlbyname(TARGET_OS_OSX ? "hw.model" : "hw.machine", model, &len, NULL, 0)) model[0] = 0;
    snprintf(out, n, "%s", model[0] ? model : "unknown");
}

static void upper_title(const char *title, char *out, size_t n) {   // one spelling of the title ID in the cloud, whoever packed the app
    size_t i = 0; for (; title[i] && i + 1 < n; i++) out[i] = (char)toupper((unsigned char)title[i]);
    out[i] = 0;
}

// ---------- packing ----------
typedef struct { CC_SHA256_CTX sha; FILE *out; ss_meta *m; uint32_t entries; uint8_t buf[CHUNK]; } packer;

static int pack_entry(packer *k, const char *abs, const char *rel, const struct stat *st) {
    char guest[1024]; ssize_t gl = getxattr(abs, GUEST_NAME, guest, sizeof guest, 0, XATTR_NOFOLLOW);
    if (gl < 0) gl = 0;   // ENOATTR: the host name is the guest's
    size_t pl = strlen(rel); int file = S_ISREG(st->st_mode);
    if (pl > UINT16_MAX) return -1;
    uint8_t h[ENT] = {(uint8_t)(file ? 2 : 1)}; uint16_t pl16 = (uint16_t)pl, gl16 = (uint16_t)gl;
    uint64_t size = file ? (uint64_t)st->st_size : 0; int64_t mt = st->st_mtimespec.tv_sec;
    memcpy(h + 2, &pl16, 2); memcpy(h + 4, &gl16, 2); memcpy(h + 8, &size, 8); memcpy(h + 16, &mt, 8);
    CC_SHA256_Update(&k->sha, h, 16); CC_SHA256_Update(&k->sha, rel, (CC_LONG)pl); CC_SHA256_Update(&k->sha, guest, (CC_LONG)gl);
    if (k->out && (fwrite(h, 1, ENT, k->out) != ENT || fwrite(rel, 1, pl, k->out) != pl || fwrite(guest, 1, (size_t)gl, k->out) != (size_t)gl)) return -1;
    k->entries++;
    if (!file) return 0;
    int fd = open(abs, O_RDONLY); if (fd < 0) return -1;
    uint64_t left = size; ssize_t got = 0;
    while (left && (got = read(fd, k->buf, left < CHUNK ? (size_t)left : CHUNK)) != 0) {
        if (got < 0) { if (errno == EINTR) continue; break; }
        CC_SHA256_Update(&k->sha, k->buf, (CC_LONG)got);
        if (k->out && fwrite(k->buf, 1, (size_t)got, k->out) != (size_t)got) break;
        left -= (uint64_t)got;
    }
    close(fd);
    if (left) return -1;   // a failed read, or a file shorter than its size (the save is a clone no Commit touches, so not one being written)
    k->m->files++; k->m->bytes += (int64_t)size;
    if (mt > k->m->saved) k->m->saved = mt;
    return 0;
}

// Every entry under abs into k, depth first, a folder before what it holds, names in byte order: the order that gives the same save the same hash on every device.
// Finder's .DS_Store is left out (a save folder someone opened in Finder is still the same save), and so are links, which the host never makes in a save
static int walk(const char *abs, const char *rel, packer *k) {
    char **v; size_t n; if (names_of(abs, &v, &n)) return -1;
    int r = 0;
    for (size_t i = 0; i < n && !r; i++) {
        if (!strcmp(v[i], ".DS_Store")) continue;
        char a[PATH_MAX], p[PATH_MAX]; struct stat st;
        if (snprintf(a, sizeof a, "%s/%s", abs, v[i]) >= (int)sizeof a || snprintf(p, sizeof p, "%s/%s", rel, v[i]) >= (int)sizeof p || lstat(a, &st)) r = -1;
        else if (S_ISDIR(st.st_mode)) { if (!(r = pack_entry(k, a, p, &st))) r = walk(a, p, k); }
        else if (S_ISREG(st.st_mode)) r = pack_entry(k, a, p, &st);
    }
    free_names(v, n); return r;
}

static int pack_save(const char *save, packer *k) {
    CC_SHA256_Init(&k->sha);
    for (int i = 0; i < 2; i++) {
        char dir[PATH_MAX]; folder_path(save, i, dir, sizeof dir);
        int h = has_entries(dir);
        if (h < 0 || (h && walk(dir, FOLDERS[i], k))) return -1;
    }
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &k->sha); hex(d, k->m->hash);
    if (!k->m->files) k->m->hash[0] = 0;   // no save: no hash, so that "nothing here" compares as nothing
    return 0;
}

int ss_scan(const char *save, ss_meta *m) {
    memset(m, 0, sizeof *m);
    packer *k = calloc(1, sizeof *k); if (!k) return -1;
    k->m = m; int r = pack_save(save, k);
    free(k); return r;
}

int ss_pack(const char *save, const char *title, const char *out, ss_meta *m) {
    memset(m, 0, sizeof *m);
    upper_title(title, m->title, sizeof m->title); device_name(m->device, sizeof m->device);
    packer *k = calloc(1, sizeof *k); if (!k) return -1;
    k->m = m; int r = -1;
    uint8_t hdr[HDR] = {0}; uint32_t version = 1;
    if (!(k->out = fopen(out, "wb")) || fwrite(hdr, 1, HDR, k->out) != HDR || pack_save(save, k) || fwrite(END, 1, sizeof END, k->out) != sizeof END) goto done;
    memcpy(hdr, MAGIC, 8); memcpy(hdr + 8, &version, 4); memcpy(hdr + 12, &k->entries, 4); memcpy(hdr + 16, m->hash, strlen(m->hash));
    memcpy(hdr + 48, &m->saved, 8); memcpy(hdr + 56, m->title, strlen(m->title)); memcpy(hdr + 80, m->device, strlen(m->device));
    if (fseeko(k->out, 0, SEEK_SET) || fwrite(hdr, 1, HDR, k->out) != HDR || fflush(k->out) || fsync(fileno(k->out))) goto done;
    r = 0;
done:
    if (k->out && fclose(k->out)) r = -1;
    if (r) unlink(out);
    free(k); return r;
}

// A path in a packed save: under save/ or save_device/, with no empty, . or .. part (a damaged file must not write outside `into`)
static int path_ok(const char *p) {
    if (strncmp(p, "save/", 5) && strncmp(p, "save_device/", 12)) return 0;
    for (const char *s = p;; ) {
        const char *e = strchr(s, '/'); size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        if (!e) return 1;
        s = e + 1;
    }
}

int ss_read(const char *bundle, const char *into, ss_meta *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(bundle, "rb"); if (!f) return -1;
    uint8_t *buf = malloc(CHUNK), hdr[HDR], h[ENT], end[sizeof END], d[CC_SHA256_DIGEST_LENGTH];
    char want[SS_HASH + 1] = "", path[PATH_MAX], guest[1024], out[PATH_MAX];
    uint32_t version, n; int r = -1, fd = -1;
    CC_SHA256_CTX sha; CC_SHA256_Init(&sha);
    if (!buf || fread(hdr, 1, HDR, f) != HDR || memcmp(hdr, MAGIC, 8)) goto done;
    memcpy(&version, hdr + 8, 4); memcpy(&n, hdr + 12, 4);
    if (version != 1) goto done;
    memcpy(want, hdr + 16, SS_HASH); memcpy(&m->saved, hdr + 48, 8); memcpy(m->title, hdr + 56, 23); memcpy(m->device, hdr + 80, 63);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t pl, gl; uint64_t size; int64_t mt;
        if (fread(h, 1, ENT, f) != ENT) goto done;
        memcpy(&pl, h + 2, 2); memcpy(&gl, h + 4, 2); memcpy(&size, h + 8, 8); memcpy(&mt, h + 16, 8);
        if ((h[0] != 1 && h[0] != 2) || !pl || pl >= sizeof path || gl > sizeof guest || (h[0] == 1 && size)) goto done;
        if (fread(path, 1, pl, f) != pl || fread(guest, 1, gl, f) != gl) goto done;
        path[pl] = 0;
        if (strlen(path) != pl || !path_ok(path)) goto done;
        CC_SHA256_Update(&sha, h, 16); CC_SHA256_Update(&sha, path, pl); CC_SHA256_Update(&sha, guest, gl);
        if (into) {
            if (snprintf(out, sizeof out, "%s/%s", into, path) >= (int)sizeof out) goto done;
            if (h[0] == 1) { if (mkdirs(out)) goto done; }
            else {
                *strrchr(out, '/') = 0; int made = mkdirs(out); out[strlen(out)] = '/';
                if (made || (fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) goto done;
            }
            if (gl && setxattr(out, GUEST_NAME, guest, gl, 0, XATTR_NOFOLLOW)) goto done;
        }
        for (uint64_t left = size; left; ) {
            size_t k = left < CHUNK ? (size_t)left : CHUNK;
            if (fread(buf, 1, k, f) != k) goto done;
            CC_SHA256_Update(&sha, buf, (CC_LONG)k);
            if (fd >= 0 && write_all(fd, buf, k)) goto done;
            left -= k;
        }
        if (fd >= 0) {   // the file's own date back: the newest one is the save's age when two changed saves are compared (ss_decide), not the time of the copy
            struct timespec t[2] = {{(time_t)mt, 0}, {(time_t)mt, 0}}; futimens(fd, t);
            int bad = close(fd); fd = -1; if (bad) goto done;
        }
        if (h[0] == 2) m->files++, m->bytes += (int64_t)size;
    }
    if (fread(end, 1, sizeof end, f) != sizeof end || memcmp(end, END, sizeof end) || fgetc(f) != EOF) goto done;
    CC_SHA256_Final(d, &sha); hex(d, m->hash);
    if (!m->files) m->hash[0] = 0;
    if (strcmp(m->hash, want)) goto done;
    r = 0;
done:
    if (fd >= 0) close(fd);
    free(buf); fclose(f);
    if (r) m->files = 0, m->bytes = 0, m->hash[0] = 0;
    return r;
}

ss_way ss_decide(const char *local, const char *base, const char *remote, int64_t local_saved, int64_t remote_saved, int ask, int *both) {
    *both = 0;
    if (!*remote) return *local ? SS_PUSH : SS_KEEP;           // nothing in the cloud
    if (!strcmp(local, remote)) return SS_KEEP;                 // already the same
    if (!*local) return SS_PULL;                                // nothing here
    if (*base && !strcmp(local, base)) return SS_PULL;          // only the cloud's changed
    if (*base && !strcmp(remote, base)) return SS_PUSH;         // only this one changed
    *both = 1;                                                  // both changed, or this device's first sync with a save of its own
    if (ask) return SS_ASK;                                     // a front end asks the player (DM_SAVE_SYNC_CHOICE)
    return remote_saved > local_saved ? SS_PULL : SS_PUSH;      // no front end asks: the newer one; the other is kept in the backups
}

// DM_SAVE_SYNC_CHOICE: what the player chose before the start, after a front end compared the two saves (docs/frontends.md, The cloud saves folder).
// Any value says the front end asks the player when both changed (then neither replaces the other without a choice); unset, the newer one wins (ss_decide).
// "ask": no choice yet (the two were the same, or only one side changed, when the front end checked). "cloud:<hash>" brings in the cloud's save with that hash,
// "local:<hash>" lets this device's replace the cloud's save with that hash ("local:none": the cloud had none), "offline": the cloud could not be read before the start.
// A choice holds only for the save it was made on: when the cloud's has changed since, it is not used (and a save that changed on both sides is replaced on neither)
typedef enum { CH_NONE, CH_ASK, CH_CLOUD, CH_LOCAL, CH_OFFLINE } choice_kind;
static choice_kind read_choice(char *hash) {
    const char *v = getenv("DM_SAVE_SYNC_CHOICE"); hash[0] = 0;
    if (!v || !*v) return CH_NONE;
    if (!strcmp(v, "ask")) return CH_ASK;
    if (!strcmp(v, "offline")) return CH_OFFLINE;
    choice_kind k = !strncmp(v, "cloud:", 6) ? CH_CLOUD : !strncmp(v, "local:", 6) ? CH_LOCAL : CH_NONE;
    const char *h = v + 6;
    if (k == CH_LOCAL && !strcmp(h, "none")) return k;
    if (k != CH_NONE && strlen(h) == SS_HASH && strspn(h, "0123456789abcdef") == SS_HASH) { memcpy(hash, h, SS_HASH + 1); return k; }
    SRED("DM_SAVE_SYNC_CHOICE=%s is not a choice (ask, cloud:<hash>, local:<hash>, local:none, offline): ignored, the newer save wins", v);
    return CH_NONE;
}

// ---------- the sync state, backups, the cloud's file ----------
static void read_base(const char *save, char *out) {   // <save>.sync: the hash of the save last known to be the same here and in the cloud ("" for none)
    char p[PATH_MAX], ln[128]; out[0] = 0;
    snprintf(p, sizeof p, "%s.sync", save);
    FILE *f = fopen(p, "r"); if (!f) return;
    while (fgets(ln, sizeof ln, f)) if (!strncmp(ln, "base\t", 5) && strspn(ln + 5, "0123456789abcdef") == SS_HASH) { memcpy(out, ln + 5, SS_HASH); out[SS_HASH] = 0; }
    fclose(f);
}

static void write_base(const char *save, const char *hash) {   // written beside it and renamed over it: a cut-off write must not read as another base
    char p[PATH_MAX], t[PATH_MAX + 8];
    snprintf(p, sizeof p, "%s.sync", save); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w");
    int err = f ? 0 : errno;
    if (f) {
        if (fprintf(f, "# save sync (dm/host/save_sync.h): the save last known to be the same here and in the cloud\nbase\t%s\n", hash) < 0) err = errno;
        // Close even after a write error, and keep that error if close or cleanup fails too.
        if (fclose(f) && !err) err = errno;
        if (!err && rename(t, p)) err = errno;
        if (err) unlink(t);
    }
    if (err) SRED("cannot write %s (errno=%d): the next start compares with the sync before", p, err);
}

// A backup this file made for `side`: "YYYY-MM-DD HH.MM.SS <side>", or with " <k>" after it (backup_dir); anything else in <save>-backups is the player's
static int is_backup(const char *name, const char *side) {
    static const char form[] = "0000-00-00 00.00.00 ";
    for (int i = 0; form[i]; i++) if (form[i] == '0' ? !isdigit((unsigned char)name[i]) : name[i] != form[i]) return 0;
    const char *s = name + sizeof form - 1; size_t sl = strlen(side);
    if (strncmp(s, side, sl)) return 0;
    if (!s[sl]) return 1;
    if (s[sl] != ' ' || !s[sl + 1]) return 0;
    for (s += sl + 1; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

// Only the newest KEEP_BACKUPS of a side stay: each switch between devices keeps a whole save, and on a phone they add up. `made` (just made) is one of them whatever its date says
static void prune_backups(const char *save, const char *side, const char *made) {
    char dir[PATH_MAX], **v; size_t n, k = 0, gone = 0; snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return;
    const char *mine = strrchr(made, '/'); mine = mine ? mine + 1 : made;
    for (size_t i = 0; i < n; i++) if (is_backup(v[i], side) && strcmp(v[i], mine)) v[k++] = v[i]; else free(v[i]);   // in date order (names_of sorts them)
    for (size_t i = 0; i + (KEEP_BACKUPS - 1) < k; i++) {
        char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, v[i]);
        if (rm_tree(p)) SRED("cannot remove the old backup %s (errno=%d)", p, errno); else gone++;
    }
    if (gone) SLOG("removed %zu old %s backup%s (the newest %d stay)", gone, side, gone == 1 ? "" : "s", KEEP_BACKUPS);
    free_names(v, k);
}

// A folder for a save about to be replaced: <save>-backups/<date> <side> (local: this device's, cloud: the cloud's); with prune, the oldest of that side beyond
// KEEP_BACKUPS go now (a pull prunes once it is complete: a pull that fails keeps the backups it found)
static int backup_dir(const char *save, const char *side, char *out, size_t n, int prune) {
    char stamp[32]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &tm);
    snprintf(out, n, "%s-backups/%s %s", save, stamp, side);
    for (int k = 2; !access(out, F_OK); k++) snprintf(out, n, "%s-backups/%s %s %d", save, stamp, side, k);
    if (mkdirs(out)) return -1;
    if (prune) prune_backups(save, side, out);
    return 0;
}

// A DM_SAVE_SYNC value → <folder>/<title ID>/save.dmsync; 0 when sync is off
static int cloud_path(const char *v, const char *title, char *out, size_t n) {
    char dir[PATH_MAX], id[24];
    if (!v || !*v || !strcmp(v, "0")) return 0;
    if (!strcmp(v, "icloud")) {
#if TARGET_OS_OSX
        const char *home = getenv("HOME"); char drive[PATH_MAX]; struct stat st;
        snprintf(drive, sizeof drive, "%s/Library/Mobile Documents/com~apple~CloudDocs", home ? home : "");
        if (!home || stat(drive, &st)) {
            if (errno == EPERM || errno == EACCES) SLOG("off: this app may not open iCloud Drive (System Settings > Privacy & Security > Files and Folders)");
            else SLOG("off: iCloud Drive is not on for this Mac (no %s)", drive);
            return 0;
        }
        snprintf(dir, sizeof dir, "%s/nxnative Saves", drive);
#else
        SLOG("off: DM_SAVE_SYNC=icloud is the Mac's iCloud Drive folder; on this device, choose the folder in the launcher (Cloud saves)");
        return 0;
#endif
    } else snprintf(dir, sizeof dir, "%s", v);
    size_t L = strlen(dir); while (L > 1 && dir[L - 1] == '/') dir[--L] = 0;
    upper_title(title, id, sizeof id);
    if (!*id || strchr(id, '/')) return 0;
    // The game's own folder chosen instead of the one above it (a picker shows <folder>/<title ID> once another device has synced): taken as it is, not given a second <title ID>
    const char *last = strrchr(dir, '/');
    if (!strcasecmp(last ? last + 1 : dir, id)) return snprintf(out, n, "%s/save.dmsync", dir) < (int)n;
    return snprintf(out, n, "%s/%s/save.dmsync", dir, id) < (int)n;
}
static int cloud_file(const char *title, char *out, size_t n) { return cloud_path(getenv("DM_SAVE_SYNC"), title, out, n); }

// The platform's fetch, given at most `ms`: a download iCloud never finishes must not hold the game at its first save mount. One that comes too late is dropped.
// Each fetch writes a file of its own (part: <local>.<pid>-<n>.part), renamed to `local` only by the caller that still waits for it: a fetch that came too late
// removes its own file and nothing else, so it never writes over or removes what a later attempt fetched to the same `local` (the sender's out/cloud.dmsync)
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int done, abandoned, result; char cloud[PATH_MAX], part[PATH_MAX + 32]; } fetch_job;
static void *fetch_thread(void *p) {
    fetch_job *j = p; int r = front_save_sync_fetch(j->cloud, j->part);
    pthread_mutex_lock(&j->mu); j->done = 1; j->result = r; int late = j->abandoned; pthread_cond_signal(&j->cv); pthread_mutex_unlock(&j->mu);
    if (late) { unlink(j->part); pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); }
    return NULL;
}
static void deadline(int ms, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000; ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}
static int fetch_timed(const char *cloud, const char *local, int ms) {   // front_save_sync_fetch's answer, or -1 at the time limit (or with no thread to wait on)
    static _Atomic unsigned seq;
    fetch_job *j = calloc(1, sizeof *j); pthread_t t;
    if (!j) return -1;
    pthread_mutex_init(&j->mu, NULL); pthread_cond_init(&j->cv, NULL);
    snprintf(j->cloud, sizeof j->cloud, "%s", cloud); snprintf(j->part, sizeof j->part, "%s.%d-%u.part", local, (int)getpid(), atomic_fetch_add(&seq, 1));
    int e = pthread_create(&t, NULL, fetch_thread, j);
    if (e) { pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j); SRED("no thread to fetch the cloud's save (error %d)", e); return -1; }   // not fetched on this thread: a download that never finishes would hold the game
    pthread_detach(t);
    struct timespec ts; deadline(ms, &ts);
    pthread_mutex_lock(&j->mu);
    while (!j->done && pthread_cond_timedwait(&j->cv, &j->mu, &ts) != ETIMEDOUT) {}
    int r = j->done ? j->result : -1, mine = j->done;
    if (!mine) j->abandoned = 1;   // the thread frees it when it ends
    pthread_mutex_unlock(&j->mu);
    if (mine) {
        if (r == 1 && rename(j->part, local)) { SRED("cannot put the cloud's save at %s (errno=%d)", local, errno); r = -1; }
        if (r != 1) unlink(j->part);   // what a fetch that failed left, if anything
        pthread_mutex_destroy(&j->mu); pthread_cond_destroy(&j->cv); free(j);
    } else SLOG("the cloud's save did not come within %d s", ms / 1000);
    return r;
}

// ---------- the two directions ----------
// The pull's journal, <save>.pull: written and synced before the first of this device's folders moves, removed once the cloud's save is wholly in place (the commit
// point). It names the backup folder and, for each folder, what this device had there: nothing, an empty folder (the host makes DM_SAVE at every mount), or a save,
// with that folder's device and inode. A rename or a swap moves a folder whole and keeps its inode, so wherever a pull that failed, or an app killed in the middle of
// one, left this device's folder (in place, or in the backups), recover() finds it and puts it back. The journal lies beside the save: <save>.sync-tmp is cleared at
// every start and <save>-backups is pruned, and the mount reads it before either (mount_once). An app kill is covered; a power loss is not claimed (the folder renames
// are not synced to the disk)
enum { HAD_NONE, HAD_EMPTY, HAD_SAVE };   // what this device had in a folder before the pull
typedef struct { char kept[PATH_MAX]; int had[2]; uint64_t dev[2], ino[2]; } pull_journal;
static void journal_path(const char *save, char *out, size_t n) { snprintf(out, n, "%s.pull", save); }
static int journal_write(const char *save, const pull_journal *j) {
    char p[PATH_MAX], t[PATH_MAX + 8], b[PATH_MAX + 512], dir[PATH_MAX]; int e = 0;
    journal_path(save, p, sizeof p); snprintf(t, sizeof t, "%s.tmp", p);
    int L = snprintf(b, sizeof b, "# save sync (dm/host/save_sync.c): a pull of the cloud's save in progress; the next start puts this device's save back\nkept\t%s\n", j->kept);
    for (int i = 0; i < 2 && L > 0 && L < (int)sizeof b; i++)
        L += snprintf(b + L, sizeof b - (size_t)L, "folder\t%d\t%d\t%llu\t%llu\n", i, j->had[i], (unsigned long long)j->dev[i], (unsigned long long)j->ino[i]);
    if (L <= 0 || L >= (int)sizeof b) { errno = ENAMETOOLONG; return -1; }
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write_all(fd, b, (size_t)L) || fsync(fd)) e = errno;
    if (close(fd) && !e) e = errno;
    if (!e && rename(t, p)) e = errno;
    if (e) { unlink(t); errno = e; return -1; }
    snprintf(dir, sizeof dir, "%s", p); char *s = strrchr(dir, '/'); if (s) *s = 0;   // the rename on the disk too, as far as the folder's sync goes
    if (s && (fd = open(dir, O_RDONLY)) >= 0) { fsync(fd); close(fd); }
    return 0;
}
static int journal_read(const char *save, pull_journal *j) {   // 1: a pull did not finish; 0: none; -1: a journal that cannot be read
    char p[PATH_MAX], line[PATH_MAX + 64]; int got = 0;
    journal_path(save, p, sizeof p); memset(j, 0, sizeof *j);
    FILE *f = fopen(p, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        int i, had; unsigned long long dev, ino;
        if (!strncmp(line, "kept\t", 5)) { snprintf(j->kept, sizeof j->kept, "%s", line + 5); got |= 4; }
        else if (sscanf(line, "folder\t%d\t%d\t%llu\t%llu", &i, &had, &dev, &ino) == 4 && i >= 0 && i < 2 && had >= HAD_NONE && had <= HAD_SAVE) {
            j->had[i] = had; j->dev[i] = dev; j->ino[i] = ino; got |= 1 << i;
        }
    }
    fclose(f);
    return got == 7 ? 1 : -1;
}
static int is_folder(const char *p, uint64_t dev, uint64_t ino) { struct stat st; return !lstat(p, &st) && S_ISDIR(st.st_mode) && (uint64_t)st.st_dev == dev && (uint64_t)st.st_ino == ino; }
// This device's save back as it was before the pull the journal describes, from wherever that pull stopped; then the journal goes. Run again after it stopped itself,
// it goes on from there. What it removes is only ever the cloud's (still in the cloud): a folder that is not the one the journal names, or one where this device had none.
// 0: back (the journal removed); -1: not (the journal kept, and with it what this device's save is made of)
static int recover(const char *save, const pull_journal *j) {
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        char here[PATH_MAX], old[PATH_MAX];
        folder_path(save, i, here, sizeof here); snprintf(old, sizeof old, "%s/%s", j->kept, FOLDERS[i]);
        if (j->had[i] == HAD_SAVE) {
            if (is_folder(here, j->dev[i], j->ino[i])) {   // in place: a folder of this name in the backups is the cloud's, on its way in
                if (*j->kept && !access(old, F_OK) && rm_tree(old)) ok = 0;
            } else if (*j->kept && is_folder(old, j->dev[i], j->ino[i])) {   // in the backups: swapped back over the cloud's, which then goes
                if (access(here, F_OK) ? rename(old, here) : renamex_np(old, here, RENAME_SWAP) || rm_tree(old)) ok = 0;
            } else { SRED("%s: this device's folder is neither in place nor in %s", here, *j->kept ? j->kept : "the backups"); ok = 0; }
        } else {   // no save of this device's here: none again (an empty folder where there was one), the cloud's removed
            if (!access(here, F_OK) && rm_tree(here)) ok = 0;
            if (ok && j->had[i] == HAD_EMPTY && mkdirs(here)) ok = 0;
        }
    }
    if (!ok) return -1;
    if (*j->kept) rmdir(j->kept);   // empty now, unless the player put something there
    char p[PATH_MAX]; journal_path(save, p, sizeof p);
    return unlink(p) && errno != ENOENT ? -1 : 0;
}
// The cloud's save replaces this one. It is unpacked beside it first (<save>.sync-tmp/in), the journal written, then each of its folders moved into this device's
// backup folder and swapped from there with the live one (renamex_np, as a Commit swaps): one step puts the cloud's folder in place and leaves this device's in the
// backups, so the staging folder, which every start clears, never holds the only copy of this device's save. A folder the cloud's save does not have goes to the
// backups too, so both hold the same. A step that fails puts everything back (recover, as the next start would after a kill). The older backups of this device's
// saves are pruned only once this one is complete. kept: the backup of this device's save, "" when it had none.
// 0: the cloud's save is in place. -1: this device's save is as it was. -2: it could not be put back: the journal stays, and the next start tries again (until then
// the save is not mounted). errno: the first failure's
static int pull(const char *save, const char *bundle, ss_meta *r, char *kept, size_t kn) {
    char tmp[PATH_MAX], here[2][PATH_MAX], incoming[2][PATH_MAX], old[2][PATH_MAX], jp[PATH_MAX];
    pull_journal j; memset(&j, 0, sizeof j);
    int fresh[2], e = 0;
    snprintf(tmp, sizeof tmp, "%s.sync-tmp/in", save);
    kept[0] = 0;
    if (rm_tree(tmp) || mkdirs(tmp) || ss_read(bundle, tmp, r)) { e = errno; rm_tree(tmp); errno = e; return -1; }
    for (int i = 0; i < 2; i++) {
        struct stat st;
        folder_path(save, i, here[i], sizeof here[i]); snprintf(incoming[i], sizeof incoming[i], "%s/%s", tmp, FOLDERS[i]);
        fresh[i] = !access(incoming[i], F_OK);
        int none = lstat(here[i], &st) != 0, h = none ? 0 : has_entries(here[i]);
        if (h < 0) { e = errno; rm_tree(tmp); errno = e; return -1; }   // a folder that cannot be read: nothing has moved (taken as empty, it would be swapped into the staging folder and cleared)
        j.had[i] = none ? HAD_NONE : h ? HAD_SAVE : HAD_EMPTY;
        if (j.had[i] == HAD_SAVE) { j.dev[i] = (uint64_t)st.st_dev; j.ino[i] = (uint64_t)st.st_ino; }
    }
    if ((j.had[0] == HAD_SAVE || j.had[1] == HAD_SAVE) && backup_dir(save, "local", kept, kn, 0)) { e = errno; kept[0] = 0; rm_tree(tmp); errno = e; return -1; }
    snprintf(j.kept, sizeof j.kept, "%s", kept);
    if (journal_write(save, &j)) { e = errno; if (*kept) rmdir(kept); kept[0] = 0; rm_tree(tmp); errno = e; return -1; }   // nothing of this device's has moved
    for (int i = 0; i < 2; i++) {
        snprintf(old[i], sizeof old[i], "%s/%s", kept, FOLDERS[i]);
        if (j.had[i] == HAD_SAVE && fresh[i]) { if (rename(incoming[i], old[i]) || renamex_np(old[i], here[i], RENAME_SWAP)) { e = errno; goto undo; } }
        else if (j.had[i] == HAD_SAVE) { if (rename(here[i], old[i])) { e = errno; goto undo; } }
        else if (fresh[i]) {   // no save in this folder: an empty one is swapped into the staging folder, which is cleared
            if (access(here[i], F_OK) ? rename(incoming[i], here[i]) : renamex_np(incoming[i], here[i], RENAME_SWAP)) { e = errno; goto undo; }
        }
    }
    journal_path(save, jp, sizeof jp);
    if (unlink(jp)) { e = errno; goto undo; }   // the commit point
    rm_tree(tmp);
    if (*kept) prune_backups(save, "local", kept);
    return 0;
undo:
    {   int back = !recover(save, &j);
        if (back) kept[0] = 0;   // nothing of this device's save is in the backups now
        rm_tree(tmp);   // the cloud's save only (and an empty folder this device had): nothing of this device's save is ever here
        errno = e;
        return back ? -1 : -2; }
}

// What this device committed goes to the cloud, unless the cloud already has it. It replaces only what it was compared against: no save, the save last synced
// (<save>.sync), or a damaged file (kept first). A cloud save that changed since this device's last sync (another device sent it, or this device never synced) is
// not replaced: the next start decides between the two (ss_decide: the newer one, or the player's choice when a front end asks).
// 1 = in step with the cloud, 0 = not: the cloud cannot be read or written now (the next Commit, a flush or the Mac launcher tries again), or it waits for the next start
static int send_save(const char *save, const char *title, const char *cloud) {
    char tmp[PATH_MAX], snap[PATH_MAX], packed[PATH_MAX], theirs[PATH_MAX], base[SS_HASH + 1], dir[PATH_MAX], kept[PATH_MAX], t[32];
    ss_meta mine, other; int ok = 0, got;
    snprintf(tmp, sizeof tmp, "%s.sync-tmp/out", save); snprintf(snap, sizeof snap, "%s/s", tmp);
    snprintf(packed, sizeof packed, "%s/save.dmsync", tmp); snprintf(theirs, sizeof theirs, "%s/cloud.dmsync", tmp);
    if (rm_tree(tmp) || mkdirs(tmp)) { SRED("cannot make %s (errno=%d)", tmp, errno); goto done; }
    // A clone of the committed folders, taken while no Commit can swap them: the game may commit again while this packs
    for (int i = 0; i < 2; i++) {
        char from[PATH_MAX], to[PATH_MAX]; folder_path(save, i, from, sizeof from); folder_path(snap, i, to, sizeof to);
        int h = has_entries(from);
        if (h < 0) { SRED("%s cannot be read (errno=%d): not sent", from, errno); goto done; }   // not packed as no save: the cloud's copy of it would go
        if (h && dm_fs_clone_committed(from, to)) { SRED("cloning %s failed (errno=%d)", from, errno); goto done; }
    }
    if (ss_pack(snap, title, packed, &mine)) { SRED("packing %s failed (errno=%d)", save, errno); goto done; }
    read_base(save, base);
    if (!mine.files || !strcmp(mine.hash, base)) { ok = 1; goto done; }   // no save yet, or the one already in step
    if ((got = fetch_timed(cloud, theirs, FETCH_MS)) < 0) { SLOG("the cloud's save %s cannot be read now; sending later", cloud); goto done; }
    if (got && ss_read(theirs, NULL, &other)) {   // no save can be read from it, so no progress is lost: the file itself is kept, then replaced
        char raw[PATH_MAX + 16];
        if (backup_dir(save, "cloud", kept, sizeof kept, 1) || snprintf(raw, sizeof raw, "%s/save.dmsync", kept) >= (int)sizeof raw || rename(theirs, raw)) { SRED("cannot keep the cloud's damaged save before replacing it (errno=%d); not sent", errno); goto done; }
        SRED("the cloud's save %s is damaged; this device's replaces it, and the damaged file is kept in %s", cloud, kept);
    }
    else if (got && !strcmp(other.hash, mine.hash)) { write_base(save, mine.hash); ok = 1; goto done; }
    else if (got && other.files && strcmp(other.hash, base)) {
        static _Atomic int told;   // once a session: the sender comes here after every Commit
        when(other.saved, t, sizeof t);
        if (!atomic_exchange(&told, 1)) SLOG("not sent: the cloud's save (from %s, saved %s) changed since this device's last sync, and so did this one; the next start decides between the two", other.device, t);
        goto done;
    }
    snprintf(dir, sizeof dir, "%s", cloud); *strrchr(dir, '/') = 0;
    if (mkdirs(dir) || front_save_sync_store(packed, cloud) != 1) { SRED("sending to %s failed (errno=%d)", cloud, errno); goto done; }
    write_base(save, mine.hash); ok = 1;
    when(mine.saved, t, sizeof t);
    SLOG("sent: %d files, saved %s, to %s", mine.files, t, cloud);
done:
    rm_tree(tmp); *strrchr(tmp, '/') = 0; rmdir(tmp);   // <save>.sync-tmp too, once nothing else is in it (a fetch that came too late may still be)
    return ok;
}

// ---------- the host: the first mount, the sender, flushes ----------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_mount_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_on;
static int g_mounted, g_want, g_busy, g_failed;   // the mount ran; a send asked for without a Commit (the mount found this save newer, a flush); a send running; the last one failed
static uint64_t g_commits, g_sent;                // Commits seen; the Commits the last send covered
static char g_save[PATH_MAX], g_title[24], g_cloud[PATH_MAX];

static void *sender(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (g_commits == g_sent && !g_want) pthread_cond_wait(&g_cv, &g_mu);
        for (uint64_t seen = g_commits; !g_want; ) {   // quiet for QUIET_MS since the last Commit (a flush does not wait)
            struct timespec ts; deadline(QUIET_MS, &ts);
            int r = 0;
            while (!g_want && g_commits == seen && r != ETIMEDOUT) r = pthread_cond_timedwait(&g_cv, &g_mu, &ts);
            if (g_commits == seen) break;
            seen = g_commits;
        }
        uint64_t upto = g_commits; g_want = 0; g_busy = 1;
        pthread_mutex_unlock(&g_mu);
        int ok = send_save(g_save, g_title, g_cloud);
        pthread_mutex_lock(&g_mu);
        g_busy = 0; g_sent = upto; g_failed = !ok;
        pthread_cond_broadcast(&g_cv);
    }
    return NULL;
}

// The first save mount: an interrupted pull put right first, whatever the sync setting (a player may have turned sync off since), before the staging folder is cleared
// and before a sender can pack what is in place; then, with sync on, the cloud's save and this one compared. 0: this device's save may be mounted; -1: it may not
// (an interrupted or failed pull could not be put back: what this device's save is made of stays where the journal says, and the next start tries again)
static int mount_once(const char *save, const char *title) {
    char cloud[PATH_MAX], tmp[PATH_MAX], theirs[PATH_MAX], base[SS_HASH + 1], kept[PATH_MAX], t[32], t2[32], chosen[SS_HASH + 1];
    ss_meta mine, other; memset(&other, 0, sizeof other); kept[0] = 0;
    pull_journal j; int jr = journal_read(save, &j);
    if (jr < 0) { SRED("%s.pull, the journal of a pull of the cloud's save, cannot be read (errno=%d): the save is not mounted; this device's save is in place or in %s-backups", save, errno, save); return -1; }
    if (jr) {
        SLOG("a pull of the cloud's save did not finish: putting this device's save back");
        if (recover(save, &j)) { SRED("this device's save could not be put back (errno=%d): it is not mounted; what is not in %s is kept in %s, and the next start tries again", errno, save, *j.kept ? j.kept : "its backups"); return -1; }
        SLOG("this device's save is back as it was before that pull");
    }
    if (!cloud_file(title, cloud, sizeof cloud)) return 0;
    SLOG("on: %s", cloud);
    snprintf(tmp, sizeof tmp, "%s.sync-tmp", save); snprintf(theirs, sizeof theirs, "%s/in.dmsync", tmp);
    if (rm_tree(tmp) || mkdirs(tmp)) { SRED("cannot make %s (errno=%d): off", tmp, errno); return 0; }   // rm_tree: what an earlier session left
    if (ss_scan(save, &mine)) { SRED("this device's save %s cannot be read (errno=%d): off", save, errno); return 0; }
    read_base(save, base);
    choice_kind choice = read_choice(chosen);
    int got = choice == CH_OFFLINE ? -1 : fetch_timed(cloud, theirs, FETCH_MS), both = 0, failed = 0, pr;
    if (got > 0 && ss_read(theirs, NULL, &other)) { SRED("the cloud's save %s is damaged: playing on with this device's, which replaces it (the damaged file is kept)", cloud); got = 0; }
    else if (got > 0 && strcasecmp(other.title, title)) { SRED("the cloud's save %s is %s's, not %s's: not used", cloud, other.title, title); got = -1; }
    if (got < 0) { SLOG("playing on with this device's save; it is not sent until it has been compared with the cloud's"); failed = 1; }
    ss_way way = got < 0 ? SS_KEEP : ss_decide(mine.hash, base, got ? other.hash : "", mine.saved, other.saved, choice != CH_NONE, &both);
    // The player's choice, for the save it was made on (the cloud's may have changed between the check and this mount)
    const char *now = got > 0 && other.files ? other.hash : "none";
    if ((choice == CH_CLOUD || choice == CH_LOCAL) && got >= 0 && way != SS_KEEP) {
        if (strcmp(choice == CH_CLOUD ? chosen : (*chosen ? chosen : "none"), now))   // the way stays ss_decide's: a save that changed on both sides is replaced on neither
            SRED("the cloud's save changed after the player chose (now %s): the choice is not used", now);
        else way = choice == CH_CLOUD ? SS_PULL : SS_PUSH;
    } else if (choice == CH_OFFLINE) SLOG("the player chose to play with this device's save without the cloud's");
    switch (way) {
    case SS_PULL:
        when(other.saved, t, sizeof t);
        if ((pr = pull(save, theirs, &other, kept, sizeof kept))) {
            if (pr == -2) { SRED("bringing the cloud's save in failed (errno=%d), and this device's could not all be put back: it is not mounted; what is not in %s is kept in %s, and the next start tries again", errno, save, kept); rm_tree(tmp); return -1; }
            SRED("bringing the cloud's save in failed (errno=%d): playing on with this device's", errno);
            failed = 1; break;
        }
        write_base(save, other.hash);
        SLOG("brought in the cloud's save (from %s, saved %s%s)%s%s", other.device, t, choice == CH_CLOUD ? ", the player's choice" : both ? ", the newer of two that both changed" : "", *kept ? "; this device's is kept in " : "", kept);
        break;
    case SS_PUSH:
        // Over a cloud save that changed since the last sync (the newer of two that both changed, or the player's choice): the start chose against it, so it is kept
        // first and counts as the one last synced; the sender replaces only that one (send_save), not a save another device sends meanwhile
        if (got > 0 && other.files && strcmp(other.hash, base)) {
            if (backup_dir(save, "cloud", kept, sizeof kept, 1) || ss_read(theirs, kept, &other)) { SRED("cannot keep the cloud's save before replacing it (errno=%d): not sent", errno); failed = 1; break; }
            write_base(save, other.hash);
        }
        when(mine.saved, t, sizeof t); when(other.saved, t2, sizeof t2);
        SLOG("this device's save (saved %s) %s: it goes to the cloud", t, !got || !other.files ? "is not in the cloud yet" : choice == CH_LOCAL ? "is the player's choice" : both ? "is the newer of two that both changed" : "changed since the last sync");
        if (*kept) SLOG("the cloud's save it replaces (from %s, saved %s) is kept in %s", other.device, t2, kept);
        g_want = 1;
        break;
    case SS_ASK:
        when(other.saved, t, sizeof t); when(mine.saved, t2, sizeof t2);
        SLOG("both changed: the cloud's save (from %s, saved %s) and this device's (saved %s); playing on with this device's, and neither replaces the other until the player chooses at the next start", other.device, t, t2);
        failed = 1;
        break;
    case SS_KEEP:
        if (got > 0 && mine.files) { write_base(save, mine.hash); SLOG("in step with the cloud"); }
        else if (!got) SLOG("no save here or in the cloud yet");
        break;
    }
    rm_tree(tmp);
    snprintf(g_save, sizeof g_save, "%s", save); upper_title(title, g_title, sizeof g_title); snprintf(g_cloud, sizeof g_cloud, "%s", cloud);
    g_failed = failed;
    pthread_t th;
    if (pthread_create(&th, NULL, sender, NULL)) { SRED("cannot start the sender: saves are not sent this session"); return 0; }
    pthread_detach(th);
    atomic_store(&g_on, 1);
    return 0;
}

// ---------- what a front end asks the player with ----------
// The two saves and the one last synced, as the mount will see them (dm/host/save_sync.h). The cloud's file is found as the mount finds it (cloud_path) and fetched
// through the same front-end call, so on the iPhone the report is what downloads a file iCloud has only listed. It goes to a file of its own under <save>.sync-tmp:
// a report the front end stopped waiting for may still be fetching when the next one starts, or when the game's mount clears the folder.
int save_sync_report(const char *save, const char *title, const char *folder, int wait_ms, char *out, size_t n) {
    char dd[PATH_MAX], cloud[PATH_MAX], tmp[PATH_MAX], theirs[PATH_MAX], base[SS_HASH + 1], dev[64];
    static _Atomic int seq;
    ss_meta mine, other; int got = -1, both = 0, same = 1, on;
    memset(&other, 0, sizeof other); base[0] = 0;
    if (!save || !*save || !title || !*title || !out || !n) return -1;
    snprintf(dd, sizeof dd, "%s", save); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;
    if (ss_scan(dd, &mine)) return -1;
    read_base(dd, base);
    if ((on = cloud_path(folder ? folder : getenv("DM_SAVE_SYNC"), title, cloud, sizeof cloud))) {
        snprintf(tmp, sizeof tmp, "%s.sync-tmp", dd);
        snprintf(theirs, sizeof theirs, "%s/report-%d-%d.dmsync", tmp, (int)getpid(), atomic_fetch_add(&seq, 1));
        if (!mkdirs(tmp)) {
            got = fetch_timed(cloud, theirs, wait_ms > 0 ? wait_ms : FETCH_MS);
            if (got > 0 && ss_read(theirs, NULL, &other)) { SRED("the cloud's save %s is damaged", cloud); got = 0; memset(&other, 0, sizeof other); }   // the mount replaces it
            unlink(theirs); rmdir(tmp);
        }
        if (got > 0 && strcasecmp(other.title, title)) same = 0;
    }
    pull_journal j; int pending = journal_read(dd, &j) != 0;   // a pull that did not finish: what is in place may be half the cloud's, so no comparison is offered;
    if (got >= 0 && same && !pending) ss_decide(mine.hash, base, got > 0 ? other.hash : "", mine.saved, other.saved, 1, &both);   // the mount puts this device's save back, then compares
    snprintf(dev, sizeof dev, "%s", other.device); for (char *s = dev; *s; s++) if (*s == ' ' || *s == '=') *s = '_';
    int w = snprintf(out, n, "sync=%d cloud=%d local=%s saved=%lld files=%d size=%lld chash=%s csaved=%lld cfiles=%d csize=%lld cdevice=%s sametitle=%d conflict=%d pull=%d",
                     on, on ? got : 0, *mine.hash ? mine.hash : "-", (long long)mine.saved, mine.files, (long long)mine.bytes,
                     got > 0 && *other.hash ? other.hash : "-", (long long)other.saved, other.files, (long long)other.bytes, *dev ? dev : "-", same, both, pending);
    return w < 0 || w >= (int)n ? -1 : 0;
}

int save_sync_mount(const char *save, const char *title) {   // the first mount does it; a second one (the device save) waits for it and gets its answer
    static int unsafe;
    pthread_mutex_lock(&g_mount_mu);
    if (!g_mounted) { g_mounted = 1; unsafe = mount_once(save, title) != 0; }
    int r = unsafe ? -1 : 0;
    pthread_mutex_unlock(&g_mount_mu);
    return r;
}

void save_sync_committed(void) {
    if (!atomic_load(&g_on)) return;
    pthread_mutex_lock(&g_mu); g_commits++; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}

int save_sync_flush(int timeout_ms) {
    if (!atomic_load(&g_on)) return 1;
    struct timespec ts; deadline(timeout_ms, &ts);
    pthread_mutex_lock(&g_mu);
    if (g_commits != g_sent || g_failed || g_busy) { g_want = 1; pthread_cond_broadcast(&g_cv); }
    while ((g_want || g_busy || g_commits != g_sent) && pthread_cond_timedwait(&g_cv, &g_mu, &ts) != ETIMEDOUT) {}
    int ok = !g_want && !g_busy && g_commits == g_sent && !g_failed;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int save_sync_after_exit(const char *save, const char *title) {   // save: DM_SAVE, its trailing slashes stripped as h_fs_MountSave strips them (the same <save>.sync and <save>-backups)
    char cloud[PATH_MAX], dd[PATH_MAX];
    if (!save || !*save) return 1;
    snprintf(dd, sizeof dd, "%s", save); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;
    pull_journal j;
    if (journal_read(dd, &j)) { SRED("a pull of the cloud's save did not finish: nothing is sent until the next start puts this device's save back"); return 0; }   // what is in place may be half the cloud's
    return !cloud_file(title, cloud, sizeof cloud) || send_save(dd, title, cloud);
}

// ---------- the platform's parts: these defaults are plain file copies, which the Mac's iCloud Drive takes (it downloads a file a read asks for) ----------
__attribute__((weak)) int front_save_sync_fetch(const char *cloud, const char *local) {
    int in = open(cloud, O_RDONLY); if (in < 0) return errno == ENOENT ? 0 : -1;
    int out = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return -1; }
    int r = copy_fd(in, out); close(in);
    if (close(out)) r = -1;
    if (r) unlink(local);
    return r ? -1 : 1;
}

// Written beside the cloud file under a .nosync name (iCloud Drive does not upload those) and renamed over it: iCloud sees one whole new file
__attribute__((weak)) int front_save_sync_store(const char *local, const char *cloud) {
    char part[PATH_MAX]; snprintf(part, sizeof part, "%s.%d.nosync", cloud, (int)getpid());
    int in = open(local, O_RDONLY); if (in < 0) return 0;
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return 0; }
    int r = copy_fd(in, out);
    if (!r && fsync(out)) r = -1;
    close(in);
    if (close(out)) r = -1;
    if (!r && rename(part, cloud)) r = -1;
    if (r) unlink(part);
    return !r;
}

__attribute__((weak)) int dm_fs_clone_committed(const char *dir, const char *to) { return clonefile(dir, to, 0) ? -1 : 0; }
