// Exercise the real metadata writer with stdio failures, and the real pull (the cloud's save swapped in) with rename failures, without cloud or game files.
// pull: every step that can fail, failing: this device's save stays whole and nowhere but in place or in its backups, and the staging folder (cleared at every
// start) never holds this device's save, also while a pull that succeeds is between two steps (the app killed there).
// The journal (<save>.pull): the app killed after each rename or swap of a pull (a longjmp out of it, nothing undone), with this device's folders holding a save, empty or
// absent, then the next start's mount (sync off): this device's save is back as it was, the cloud's gone, the journal removed; killed again in the middle of that recovery,
// the next one finishes it. A recovery that fails refuses the mount and keeps the journal, and the start after it recovers; an unreadable journal refuses the mount; a
// second mount gets the first's answer. Older backups are pruned only when a pull completes, and nothing is sent after the host while a journal is pending
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fail_open, fail_write, fail_close, fail_rename, cleanup_errno, track_metadata;
static int fail_stage_write_after, stage_write_seen, stage_write_fd = -1, fail_stage_publish;
static int fail_start_clone, fail_start_publish;
static const char *fail_xattr_suffix;
static int fail_xattr_errno;
static int closes, metadata_fd;
static _Atomic int renames, unlinks;
static FILE *metadata;
static char diagnostic[2048];
static pthread_mutex_t race_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_cv = PTHREAD_COND_INITIALIZER;
static int race_need, race_arrived, race_hold_ms;
static void (*race_before_store)(void);
static _Atomic int rename_in_flight, rename_overlap;

static FILE *test_fopen(const char *path, const char *mode) {
    if (track_metadata && !strcmp(mode, "w")) {
        if (fail_open) { errno = EACCES; return NULL; }
        metadata = fopen(path, mode);
        if (metadata) metadata_fd = fileno(metadata);
        return metadata;
    }
    return fopen(path, mode);
}
static int test_fprintf(FILE *f, const char *fmt, ...) {
    if (track_metadata && f == metadata && fail_write) { errno = ENOSPC; return -1; }
    va_list ap; va_start(ap, fmt);
    int r = (track_metadata && f == stderr) ? vsnprintf(diagnostic, sizeof diagnostic, fmt, ap) : vfprintf(f, fmt, ap);
    va_end(ap); return r;
}
static int test_fclose(FILE *f) {
    int tracked = track_metadata && f == metadata;
    if (tracked) { closes++; metadata = NULL; }
    int r = fclose(f);
    if (tracked && fail_close) { errno = EIO; return EOF; }
    return r;
}
// A pull's fault: the call (1 rename, 2 renamex_np) whose from and to end so fails with EIO, after `skip` such calls succeeded; up to two at once
static struct fault { int op; const char *from, *to; int skip; } faults[2];
static char staging[PATH_MAX];   // <save>.sync-tmp: after every rename and swap of a pull, it must hold nothing of this device's save
static int staging_held;
static int ends(const char *s, const char *suffix) { size_t a = strlen(s), b = strlen(suffix); return a >= b && !strcmp(s + a - b, suffix); }
static int faulted(int op, const char *from, const char *to) {
    for (int i = 0; i < 2; i++) {
        struct fault *f = &faults[i];
        if (f->op != op || !ends(from, f->from) || !ends(to, f->to)) continue;
        if (f->skip-- > 0) continue;
        f->op = 0; errno = EIO; return 1;
    }
    return 0;
}
static int holds(const char *dir, const char *text);
// The app killed: after `crash_after` more renames and swaps succeed, the next one's caller never comes back (crash_jmp)
static int crash_after = -1; static jmp_buf crash_jmp;
static void crash_point(void) { if (crash_after > 0 && !--crash_after) { crash_after = -1; longjmp(crash_jmp, 1); } }
static int test_flock(int fd, int op) {
    if (op == LOCK_EX) {
        if (race_before_store) { void (*fn)(void) = race_before_store; race_before_store = NULL; fn(); }
        if (race_need > 0) {
            pthread_mutex_lock(&race_mu);
            race_arrived++;
            if (race_arrived >= race_need) pthread_cond_broadcast(&race_cv);
            while (race_arrived < race_need) pthread_cond_wait(&race_cv, &race_mu);
            pthread_mutex_unlock(&race_mu);
        }
    }
    return flock(fd, op);
}
static int test_rename(const char *from, const char *to) {
    renames++;
    if (fail_rename) { errno = EROFS; return -1; }
    if (faulted(1, from, to)) return -1;
    if (race_hold_ms > 0 && ends(to, "/save.dmsync")) {
        if (atomic_fetch_add(&rename_in_flight, 1) > 0) rename_overlap = 1;
        usleep((useconds_t)race_hold_ms * 1000);
        atomic_fetch_sub(&rename_in_flight, 1);
    }
    int r = rename(from, to);
    if (*staging && holds(staging, "original")) staging_held = 1;
    if (!r) crash_point();
    return r;
}
static int test_renamex_np(const char *from, const char *to, unsigned flags) {
    if (fail_stage_publish && strstr(from, "/.cloud-stage.")) {
        fail_stage_publish = 0; errno = EIO; return -1;
    }
    if (fail_start_publish && strstr(from, "/.start-stage.")) {
        fail_start_publish = 0; errno = EIO; return -1;
    }
    if (faulted(2, from, to)) return -1;
    if (race_hold_ms > 0 && ends(to, "/save.dmsync")) {
        if (atomic_fetch_add(&rename_in_flight, 1) > 0) rename_overlap = 1;
        usleep((useconds_t)race_hold_ms * 1000);
        atomic_fetch_sub(&rename_in_flight, 1);
    }
    int r = renamex_np(from, to, flags);
    if (*staging && holds(staging, "original")) staging_held = 1;
    if (!r) crash_point();
    return r;
}
static int test_open(const char *path, int oflag, ...) {
    mode_t mode = 0;
    if (oflag & O_CREAT) {
        va_list ap; va_start(ap, oflag);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    int fd = (oflag & O_CREAT) ? open(path, oflag, mode) : open(path, oflag);
    if (fd >= 0 && (oflag & O_CREAT) && fail_stage_write_after > 0 && strstr(path, "/.cloud-stage.")) {
        if (++stage_write_seen == fail_stage_write_after) stage_write_fd = fd;
    }
    return fd;
}
static ssize_t test_write(int fd, const void *buf, size_t n) {
    if (stage_write_fd >= 0 && fd == stage_write_fd) {
        stage_write_fd = -1; errno = ENOSPC; return -1;
    }
    return write(fd, buf, n);
}
static ssize_t test_getxattr(const char *path, const char *name, void *value, size_t size, u_int32_t position, int options) {
    if (fail_xattr_suffix && ends(path, fail_xattr_suffix)) { errno = fail_xattr_errno; return -1; }
    return getxattr(path, name, value, size, position, options);
}
static int test_clonefile(const char *src, const char *dst, uint32_t flags) {
    if (fail_start_clone && strstr(dst, "/.start-stage.")) {
        fail_start_clone = 0; errno = ENOSPC; return -1;
    }
    return clonefile(src, dst, flags);
}
static int test_unlink(const char *path) {
    unlinks++;
    int r = unlink(path);
    if (cleanup_errno) errno = cleanup_errno;
    return r;
}
#define fopen test_fopen
#define fprintf test_fprintf
#define fclose test_fclose
#define flock test_flock
#define open test_open
#define write test_write
#define getxattr test_getxattr
#define clonefile test_clonefile
#define rename test_rename
#define unlink test_unlink
#define renamex_np test_renamex_np
#include "../save_sync.c"
#undef fopen
#undef fprintf
#undef fclose
#undef flock
#undef open
#undef write
#undef getxattr
#undef clonefile
#undef rename
#undef unlink
#undef renamex_np

static int checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s (%s)\n", name, label); } } while (0)
static const char old_hash[] = "00000000000000000000000000000000";
static const char new_hash[] = "123456789abcdef0123456789abcdef0";

static void run_case(const char *save, const char *name, int open_error, int write_error,
                     int close_error, int rename_error, int expected_errno) {
    char path[PATH_MAX], temp[PATH_MAX + 8], hash[SS_HASH + 1];
    snprintf(path, sizeof path, "%s.sync", save);
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(path, "w");
    if (!f || fprintf(f, "base\t%s\n", old_hash) < 0 || fclose(f)) abort();
    fail_open = open_error; fail_write = write_error; fail_close = close_error; fail_rename = rename_error;
    cleanup_errno = EBUSY; // Cleanup must not replace the original diagnostic.
    closes = renames = unlinks = 0; metadata_fd = -1; metadata = NULL; diagnostic[0] = 0;
    track_metadata = 1;
    write_base(save, new_hash);
    track_metadata = 0;
    CHECK(closes == !open_error, "every opened stream is closed exactly once");
    if (!open_error) CHECK(fcntl(metadata_fd, F_GETFD) == -1 && errno == EBADF, "file descriptor is closed");
    CHECK(renames == !(open_error || write_error || close_error), "rename only follows successful write and close");
    CHECK(unlinks == (!!expected_errno && !open_error), "failed temporary file is removed");
    if (expected_errno) {
        char error[40]; snprintf(error, sizeof error, "(errno=%d)", expected_errno);
        CHECK(strstr(diagnostic, error) != NULL, "first error is reported after cleanup");
    } else CHECK(!diagnostic[0], "success has no error diagnostic");
    CHECK(access(temp, F_OK) == -1 && errno == ENOENT, "no temporary file remains");
    // Keep the failing implementation's leaked stream from affecting later cases.
    if (metadata) { fclose(metadata); metadata = NULL; }
    read_base(save, hash);
    CHECK(!strcmp(hash, expected_errno ? old_hash : new_hash), "failed updates preserve previous metadata; success replaces it");
}
// Any file under dir whose contents start with text
static int holds(const char *dir, const char *text) {
    char **v; size_t n; int k = 0; struct stat st;
    if (lstat(dir, &st)) return 0;
    if (!S_ISDIR(st.st_mode)) { char b[64] = {0}; FILE *f = fopen(dir, "r"); if (!f) return 0; fread(b, 1, sizeof b - 1, f); fclose(f); return !strncmp(b, text, strlen(text)); }
    if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) { char c[PATH_MAX]; snprintf(c, sizeof c, "%s/%s", dir, v[i]); k += holds(c, text); }
    free_names(v, n); return k;
}
static int is(const char *path, const char *text) {
    char b[512] = {0}; FILE *f = fopen(path, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(text) && !memcmp(b, text, k);
}
static void put(const char *path, const char *text) { FILE *f = fopen(path, "w"); if (!f || fputs(text, f) < 0 || fclose(f)) abort(); }
// The cloud's save, packed: save/a "cloud 0" and, with device, save_device/b "cloud 1"
static void cloud_bundle(const char *root, int device, char *out, size_t n) {
    char src[PATH_MAX], p[PATH_MAX]; ss_meta m;
    snprintf(src, sizeof src, "%s/cloud%d", root, device); mkdirs(src);
    snprintf(p, sizeof p, "%s/a", src); put(p, "cloud 0");
    if (device) { snprintf(p, sizeof p, "%s_device", src); mkdirs(p); snprintf(p, sizeof p, "%s_device/b", src); put(p, "cloud 1"); }
    snprintf(out, n, "%s/cloud%d.dmsync", root, device);
    if (ss_pack(src, "0100000000010000", out, &m)) abort();
}
// One pull onto this device's save/a "original 0" and save_device/b "original 1", with up to two faults; want: pull's answer.
// device_cloud: the cloud's save has save_device. After it: what is in place, where this device's save is, what is left behind
static void pull_case(const char *root, const char *name, const char *bundle, int device_cloud, struct fault f0, struct fault f1, int want) {
    char save[PATH_MAX], dev[PATH_MAX], p[PATH_MAX], kept[PATH_MAX], backups[PATH_MAX]; ss_meta m;
    snprintf(save, sizeof save, "%s/save", root); snprintf(dev, sizeof dev, "%s/save_device", root); snprintf(backups, sizeof backups, "%s-backups", save);
    rm_tree(save); rm_tree(dev); rm_tree(backups); mkdirs(save); mkdirs(dev);
    snprintf(p, sizeof p, "%s/a", save); put(p, "original 0"); snprintf(p, sizeof p, "%s/b", dev); put(p, "original 1");
    snprintf(staging, sizeof staging, "%s.sync-tmp", save); staging_held = 0;
    faults[0] = f0; faults[1] = f1;
    int r = pull(save, bundle, &m, kept, sizeof kept), e = errno;
    *staging = 0;
    CHECK(r == want, "pull's answer");
    CHECK(!faults[0].op && !faults[1].op, "every fault was reached");
    CHECK(!staging_held, "the staging folder never held this device's save");
    snprintf(p, sizeof p, "%s.sync-tmp/in", save);
    CHECK(access(p, F_OK) == -1 && errno == ENOENT, "the staging folder is cleared");
    CHECK(holds(root, "original 0") == 1 && holds(root, "original 1") == 1, "this device's save is there once, nothing lost or copied");
    CHECK(holds(save, "original 0") + holds(backups, "original 0") == 1 && holds(dev, "original 1") + holds(backups, "original 1") == 1,
          "this device's save is in place or in its backups");
    if (want) CHECK(r == -2 || e == EIO, "the first failure's errno");
    if (want == -1) {
        snprintf(p, sizeof p, "%s/a", save); CHECK(is(p, "original 0"), "a failed pull leaves this device's save in place");
        snprintf(p, sizeof p, "%s/b", dev); CHECK(is(p, "original 1"), "a failed pull leaves this device's device save in place");
        CHECK(!holds(backups, "cloud"), "no cloud file is left in the backups");
        CHECK(access(kept, F_OK) == -1, "the empty backup folder is removed");
    } else if (!want) {
        snprintf(p, sizeof p, "%s/a", save); CHECK(is(p, "cloud 0"), "the cloud's save is in place");
        snprintf(p, sizeof p, "%s/b", dev); CHECK(device_cloud ? is(p, "cloud 1") : access(p, F_OK) == -1, "the cloud's device save is in place, or none when it has none");
        snprintf(p, sizeof p, "%s/save/a", kept); CHECK(is(p, "original 0"), "this device's save is kept in the backup pull names");
        snprintf(p, sizeof p, "%s/save_device/b", kept); CHECK(is(p, "original 1"), "this device's device save is kept in the backup pull names");
    } else CHECK(*kept && holds(kept, "original"), "the backup pull names holds what is not in place");
}
// This device's save before a pull: save/a "original 0", and the device folder as `dev_had` says (HAD_SAVE: save_device/b "original 1", HAD_EMPTY: an empty folder, HAD_NONE: none)
static void device_save(const char *root, int dev_had) {
    char save[PATH_MAX], dev[PATH_MAX], p[PATH_MAX];
    snprintf(save, sizeof save, "%s/save", root); snprintf(dev, sizeof dev, "%s/save_device", root);
    rm_tree(save); rm_tree(dev); snprintf(p, sizeof p, "%s-backups", save); rm_tree(p); snprintf(p, sizeof p, "%s.pull", save); unlink(p);
    mkdirs(save); snprintf(p, sizeof p, "%s/a", save); put(p, "original 0");
    if (dev_had != HAD_NONE) mkdirs(dev);
    if (dev_had == HAD_SAVE) { snprintf(p, sizeof p, "%s/b", dev); put(p, "original 1"); }
}
// This device's save as device_save made it, no cloud files anywhere under root (except bundles), no local/cloud backups or staging,
// and exactly one start copy holding this device's recovered save (nothing of the cloud's)
static int as_before(const char *root, int dev_had) {
    char p[PATH_MAX], dev[PATH_MAX], bdir[PATH_MAX], **v; size_t n; int ok = 1;
    snprintf(p, sizeof p, "%s/save/a", root); ok &= is(p, "original 0");
    snprintf(p, sizeof p, "%s/save", root); if (!names_of(p, &v, &n)) { ok &= n == 1; free_names(v, n); } else ok = 0;
    snprintf(dev, sizeof dev, "%s/save_device", root);
    if (dev_had == HAD_SAVE) { snprintf(p, sizeof p, "%s/b", dev); ok &= is(p, "original 1"); }
    else if (dev_had == HAD_EMPTY) ok &= !access(dev, F_OK) && !has_entries(dev);
    else ok &= access(dev, F_OK) == -1;
    snprintf(p, sizeof p, "%s/save", root); ok &= !holds(p, "cloud") && !holds(dev, "cloud");
    snprintf(bdir, sizeof bdir, "%s/save-backups", root);
    ok &= !holds(bdir, "cloud");
    int starts = 0, others = 0;
    if (!names_of(bdir, &v, &n)) {
        for (size_t i = 0; i < n; i++) {
            if (is_backup(v[i], START_SIDE)) {
                starts++;
                snprintf(p, sizeof p, "%s/%s", bdir, v[i]);
                ok &= holds(p, "original") == (dev_had == HAD_SAVE ? 2 : 1) && !holds(p, "cloud");
            } else others++;
        }
        free_names(v, n);
    } else ok = 0;
    ok &= starts == 1 && others == 0;
    snprintf(p, sizeof p, "%s/save.pull", root); ok &= access(p, F_OK) == -1;
    return ok;
}
static int count_backups(const char *save) {
    char dir[PATH_MAX], **v; size_t n; int k = 0; snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) k += is_backup(v[i], "local");
    free_names(v, n); return k;
}
static void journal_cases(const char *root, const char *both, const char *one) {
    char save[PATH_MAX], kept[PATH_MAX], p[PATH_MAX]; ss_meta m; const char *name; struct fault none = {0};
    snprintf(save, sizeof save, "%s/save", root);
    unsetenv("DM_SAVE_SYNC");   // the next start's mount with sync off: the journal is put right whatever the setting
    static const char *had_names[] = {"no device folder", "an empty device folder", "a device save"};
    for (int dev_had = HAD_NONE; dev_had <= HAD_SAVE; dev_had++)
        for (int b = 0; b < 2; b++) {   // the cloud's save with and without a device save
            int steps = 0;
            for (int k = 1; k < 16; k++) {   // killed after k renames and swaps; the first k that is not reached is the pull completing
                device_save(root, dev_had);
                char label[200]; snprintf(label, sizeof label, "killed after %d steps, %s, the cloud's %s a device save", k, had_names[dev_had], b ? "with" : "without"); name = label;
                faults[0] = faults[1] = none; crash_after = k;
                if (!setjmp(crash_jmp)) { int r = pull(save, b ? both : one, &m, kept, sizeof kept); crash_after = -1; CHECK(!r, "an uninterrupted pull completes"); steps = k - 1; break; }
                snprintf(p, sizeof p, "%s.pull", save); CHECK(!access(p, F_OK), "the journal is there after the kill");
                if (k == 3) {   // and killed again in the middle of the recovery: the next start finishes it
                    crash_after = 1; pull_journal j;
                    if (!setjmp(crash_jmp)) { CHECK(journal_read(save, &j) == 1, "the journal reads back"); recover(save, &j); }
                    crash_after = -1;
                }
                CHECK(mount_once(save, "0100000000010000") == 0, "the next start's mount goes on");
                CHECK(as_before(root, dev_had), "this device's save is back as it was, the cloud's gone, the journal removed");
            }
            name = had_names[dev_had]; CHECK(steps >= 2, "a pull has steps to be killed after");
        }
    // A recovery that fails refuses the mount and keeps the journal; the start after it recovers
    name = "a recovery that fails";
    device_save(root, HAD_SAVE); faults[0] = faults[1] = none; crash_after = 3;   // the journal, the cloud's save into the backups, then its swap: the cloud's in place
    if (!setjmp(crash_jmp)) { pull(save, both, &m, kept, sizeof kept); crash_after = -1; CHECK(0, "the pull was killed"); }
    faults[0] = (struct fault){2, "local/save", "/save", 0};   // the swap back fails
    CHECK(mount_once(save, "0100000000010000") == -1, "the mount is refused");
    snprintf(p, sizeof p, "%s.pull", save); CHECK(!access(p, F_OK), "the journal is kept");
    CHECK(holds(root, "original 0") == 1 && holds(root, "original 1") == 1, "this device's save is whole, in place or in its backups");
    CHECK(!faults[0].op, "the fault was reached");
    CHECK(mount_once(save, "0100000000010000") == 0 && as_before(root, HAD_SAVE), "the next start puts it back");
    // An unreadable journal refuses the mount and is left for the player
    name = "an unreadable journal";
    device_save(root, HAD_SAVE); put(p, "not a journal\n");
    CHECK(mount_once(save, "0100000000010000") == -1 && !access(p, F_OK), "the mount is refused, the file left");
    unlink(p);
    // Pruning: a pull that fails keeps the older backups it found; one that completes leaves KEEP_BACKUPS
    name = "pruning";
    device_save(root, HAD_SAVE);
    for (int i = 0; i < KEEP_BACKUPS + 2; i++) { snprintf(p, sizeof p, "%s-backups/2020-01-%02d 00.00.00 local", save, i + 1); mkdirs(p); }
    faults[0] = (struct fault){1, "in/save", "local/save", 0}; faults[1] = none;
    CHECK(pull(save, both, &m, kept, sizeof kept) == -1 && count_backups(save) == KEEP_BACKUPS + 2, "a pull that fails prunes nothing");
    faults[0] = none;
    CHECK(pull(save, both, &m, kept, sizeof kept) == 0 && count_backups(save) == KEEP_BACKUPS, "one that completes leaves the newest KEEP_BACKUPS, its own among them");
    snprintf(p, sizeof p, "%s/save/a", kept); CHECK(is(p, "original 0"), "its own backup is kept");
    // Nothing is sent after the host while a journal is pending: what is in place may be half the cloud's
    name = "send after exit with a journal pending";
    device_save(root, HAD_SAVE); faults[0] = none; crash_after = 3;
    if (!setjmp(crash_jmp)) { pull(save, both, &m, kept, sizeof kept); crash_after = -1; }
    char cloud[PATH_MAX], cf[PATH_MAX]; snprintf(cloud, sizeof cloud, "%s/cloud-folder", root); setenv("DM_SAVE_SYNC", cloud, 1);
    CHECK(save_sync_after_exit(save, "0100000000010000") == 0, "not in step: nothing sent");
    snprintf(cf, sizeof cf, "%s/0100000000010000/save.dmsync", cloud); CHECK(access(cf, F_OK) == -1, "the cloud has no file");
    unsetenv("DM_SAVE_SYNC");
    // The first mount's answer is the second's (the device save mounted after the account save)
    name = "a second mount";
    CHECK(save_sync_mount(save, "0100000000010000") == 0 && as_before(root, HAD_SAVE), "the first mount recovers and goes on");
    device_save(root, HAD_SAVE); snprintf(p, sizeof p, "%s.pull", save); put(p, "not a journal\n");
    CHECK(save_sync_mount(save, "0100000000010000") == 0, "a second mount gets the first's answer, not a new one");
    unlink(p);
}
// dir holds `keep` and nothing else (keep NULL: nothing)
static int holds_only(const char *dir, const char *keep) {
    DIR *d = opendir(dir); struct dirent *e; int ok = d != NULL;
    while (d && (e = readdir(d))) if (e->d_name[0] != '.' && (!keep || strcmp(e->d_name, keep))) ok = 0;
    if (d) closedir(d);
    return ok;
}
static int file_is(const char *path, const char *text) {
    char b[64]; int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    return n >= 0 && (b[n] = 0, !strcmp(b, text));
}
// fetch_timed: a fetch that comes too late (blocked on a FIFO, the default fetch's open) neither writes over nor removes what a later attempt fetched to the same file
static void fetch_cases(const char *folder) {
    const char *name = "late fetch"; char dir[PATH_MAX], fifo[PATH_MAX], src[PATH_MAX], none[PATH_MAX], dst[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/fetch", folder); mkdir(dir, 0755);
    snprintf(fifo, sizeof fifo, "%s/cloud.fifo", folder); snprintf(src, sizeof src, "%s/cloud.dmsync", folder); snprintf(none, sizeof none, "%s/none.dmsync", folder);
    snprintf(dst, sizeof dst, "%s/cloud.dmsync", dir);
    put(src, "newer");
    CHECK(fetch_timed(none, dst, 2000, NULL) == 0 && holds_only(dir, NULL), "no cloud file: 0, nothing left");
    CHECK(fetch_timed(src, dst, 2000, NULL) == 1 && file_is(dst, "newer") && holds_only(dir, "cloud.dmsync"), "a fetch in time: the file at its place, nothing else");
    test_unlink(dst);
    if (mkfifo(fifo, 0644)) abort();
    CHECK(fetch_timed(fifo, dst, 100, NULL) == -1 && holds_only(dir, NULL), "a fetch that does not come in time: -1");
    CHECK(fetch_timed(src, dst, 2000, NULL) == 1 && file_is(dst, "newer"), "the next attempt's fetch");
    int w = open(fifo, O_WRONLY); if (w < 0 || write(w, "older", 5) != 5) abort();
    close(w);   // the first fetch goes on now: it copies "older", finds itself dropped and removes its file
    usleep(300 * 1000);
    CHECK(file_is(dst, "newer") && holds_only(dir, "cloud.dmsync"), "the late fetch leaves the later attempt's file as it was and removes only its own");
    test_unlink(dst); test_unlink(src); test_unlink(fifo); rmdir(dir);
}

typedef struct { const char *save, *title, *cloud; int ok; } send_args;
static void *send_worker(void *p) {
    send_args *a = p;
    a->ok = send_save(a->save, a->title, a->cloud);
    return NULL;
}
static int cloud_file_is(const char *cloud, const char *root, const char *text) {
    static int seq; char into[PATH_MAX], p[PATH_MAX]; ss_meta m;
    snprintf(into, sizeof into, "%s/peek-race-%d", root, seq++);
    rm_tree(into); mkdirs(into);
    int ok = !ss_read(cloud, into, &m) && (snprintf(p, sizeof p, "%s/save/a", into), is(p, text));
    rm_tree(into);
    return ok;
}
static char race_target[PATH_MAX], race_replacement[PATH_MAX], race_placeholder[PATH_MAX];
static void mutate_replace_cloud(void) { if (rename(race_replacement, race_target)) abort(); }
static void mutate_create_placeholder(void) { put(race_placeholder, "icloud placeholder"); }
static void mutate_unlink_cloud(void) { if (unlink(race_target)) abort(); }
static void mutate_inplace_cloud(void) {
    int fd = open(race_target, O_WRONLY | O_TRUNC);
    if (fd < 0) abort();
    int src = open(race_replacement, O_RDONLY);
    if (src < 0 || copy_fd(src, fd)) abort();
    close(src); close(fd);
    unlink(race_replacement);
}
static int count_cloud_backups(const char *save) {
    char dir[PATH_MAX], **v; size_t n; int k = 0; snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) k += is_backup(v[i], "cloud");
    free_names(v, n); return k;
}

static void concurrent_send_cases(const char *folder) {
    static const char *TID = "0100000000010000";
    char root[PATH_MAX], cloud_dir[PATH_MAX], title_dir[PATH_MAX], cf[PATH_MAX];
    char sa[PATH_MAX], sb[PATH_MAX], so[PATH_MAX], p[PATH_MAX], base_a[SS_HASH + 1], base_b[SS_HASH + 1], report[2048];
    ss_meta mo, ma, mb;
    snprintf(root, sizeof root, "%s/race", folder);
    snprintf(cloud_dir, sizeof cloud_dir, "%s/cloud", root);
    snprintf(title_dir, sizeof title_dir, "%s/%s", cloud_dir, TID);
    snprintf(cf, sizeof cf, "%s/save.dmsync", title_dir);
    snprintf(so, sizeof so, "%s/orig/save", root);
    snprintf(sa, sizeof sa, "%s/a/save", root);
    snprintf(sb, sizeof sb, "%s/b/save", root);
    rm_tree(root); mkdirs(title_dir); mkdirs(so); mkdirs(sa); mkdirs(sb);

    // 1. Two senders read the same original cloud save, pass hash checks, and race into replacement under per-title coordination.
    const char *name = "two senders racing over original cloud save";
    snprintf(p, sizeof p, "%s/a", so); put(p, "original");
    CHECK(!ss_pack(so, TID, cf, &mo), "pack original cloud save");
    snprintf(p, sizeof p, "%s/a", sa); put(p, "save-from-A"); write_base(sa, mo.hash); CHECK(!ss_scan(sa, &ma), "scan A");
    snprintf(p, sizeof p, "%s/a", sb); put(p, "save-from-B"); write_base(sb, mo.hash); CHECK(!ss_scan(sb, &mb), "scan B");

    race_need = 2; race_arrived = 0; race_hold_ms = 25; rename_in_flight = 0; rename_overlap = 0;
    send_args wa = {sa, TID, cf, -1}, wb = {sb, TID, cf, -1};
    pthread_t ta, tb;
    if (pthread_create(&ta, NULL, send_worker, &wa) || pthread_create(&tb, NULL, send_worker, &wb)) abort();
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    race_need = 0; race_hold_ms = 0;

    CHECK(wa.ok + wb.ok == 1, "exactly one competing sender succeeds");
    CHECK(!rename_overlap, "per-title write coordination serializes the replacement");
    CHECK(holds_only(title_dir, "save.dmsync"), "no temporary .nosync files remain in the cloud title directory");
    const char *win_save = wa.ok ? sa : sb, *lose_save = wa.ok ? sb : sa;
    const char *win_txt = wa.ok ? "save-from-A" : "save-from-B", *lose_txt = wa.ok ? "save-from-B" : "save-from-A";
    const char *win_hash = wa.ok ? ma.hash : mb.hash;
    CHECK(cloud_file_is(cf, root, win_txt), "cloud keeps the winning sender's save");
    snprintf(p, sizeof p, "%s/a", win_save); CHECK(is(p, win_txt), "winner's local save intact");
    snprintf(p, sizeof p, "%s/a", lose_save); CHECK(is(p, lose_txt), "stale sender retains its local save");
    read_base(win_save, base_a); read_base(lose_save, base_b);
    CHECK(!strcmp(base_a, win_hash), "winner advances its sync base");
    CHECK(!strcmp(base_b, mo.hash), "stale sender retains its previous sync base");
    CHECK(send_save(lose_save, TID, cf) == 0 && cloud_file_is(cf, root, win_txt), "a retry by the stale sender still refuses to overwrite the winner");
    CHECK(!save_sync_report(lose_save, TID, cloud_dir, 2000, report, sizeof report) && strstr(report, "conflict=1"), "next start sees a conflict for the stale sender");

    // 2. Two senders racing when no cloud save exists yet (both have empty base; kernel-atomic RENAME_EXCL).
    name = "two senders racing over absent cloud save";
    unlink(cf);
    snprintf(p, sizeof p, "%s.sync", sa); unlink(p);
    snprintf(p, sizeof p, "%s.sync", sb); unlink(p);
    race_need = 2; race_arrived = 0; race_hold_ms = 25; rename_in_flight = 0; rename_overlap = 0;
    wa = (send_args){sa, TID, cf, -1}; wb = (send_args){sb, TID, cf, -1};
    if (pthread_create(&ta, NULL, send_worker, &wa) || pthread_create(&tb, NULL, send_worker, &wb)) abort();
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    race_need = 0; race_hold_ms = 0;

    CHECK(wa.ok + wb.ok == 1, "exactly one sender creates the initial cloud save");
    CHECK(!rename_overlap, "per-title write coordination serializes initial creation");
    CHECK(holds_only(title_dir, "save.dmsync"), "no leftover staging files after absent-cloud race");
    win_save = wa.ok ? sa : sb; lose_save = wa.ok ? sb : sa;
    win_txt = wa.ok ? "save-from-A" : "save-from-B"; lose_txt = wa.ok ? "save-from-B" : "save-from-A";
    win_hash = wa.ok ? ma.hash : mb.hash;
    CHECK(cloud_file_is(cf, root, win_txt), "cloud holds the winner's initial save");
    snprintf(p, sizeof p, "%s/a", lose_save); CHECK(is(p, lose_txt), "stale sender retains its local save");
    read_base(win_save, base_a); read_base(lose_save, base_b);
    CHECK(!strcmp(base_a, win_hash) && !base_b[0], "winner advances base; stale sender keeps empty base");
    CHECK(!save_sync_report(lose_save, TID, cloud_dir, 2000, report, sizeof report) && strstr(report, "conflict=1"), "next start reports conflict for the stale sender");

    // 3. An iCloud placeholder appearing after an absent fetch blocks replacement.
    name = "placeholder appearing before absent-cloud store";
    unlink(cf);
    snprintf(p, sizeof p, "%s.sync", sb); unlink(p);
    snprintf(race_placeholder, sizeof race_placeholder, "%s/.save.dmsync.icloud", title_dir);
    race_before_store = mutate_create_placeholder;
    CHECK(send_save(sb, TID, cf) == 0, "sender refuses to create save.dmsync once .icloud placeholder appears");
    CHECK(access(cf, F_OK) == -1 && !access(race_placeholder, F_OK), "cloud file not created and placeholder untouched");
    read_base(sb, base_b); CHECK(!base_b[0], "stale sender keeps empty base");
    unlink(race_placeholder);

    // 4. A damaged cloud file replaced by a valid competing save before store: stale sender must not overwrite or prune backups.
    name = "damaged cloud file replaced by valid save before store";
    put(cf, "DMSYNC1\ndamaged");
    snprintf(race_target, sizeof race_target, "%s", cf);
    snprintf(race_replacement, sizeof race_replacement, "%s/valid-A.dmsync", root);
    CHECK(!ss_pack(sa, TID, race_replacement, &ma), "pack competing valid save A");
    for (int i = 0; i < KEEP_BACKUPS + 2; i++) { snprintf(p, sizeof p, "%s-backups/2020-01-%02d 00.00.00 cloud", sb, i + 1); mkdirs(p); }
    write_base(sb, mo.hash);
    race_before_store = mutate_replace_cloud;
    CHECK(send_save(sb, TID, cf) == 0, "stale sender does not overwrite valid save that replaced damaged file");
    CHECK(cloud_file_is(cf, root, "save-from-A"), "competing valid save A survives in cloud");
    CHECK(count_cloud_backups(sb) == KEEP_BACKUPS + 2, "stale sender did not prune or leave a bogus cloud backup");
    read_base(sb, base_b); CHECK(!strcmp(base_b, mo.hash), "stale sender retains previous base");

    // 5. Cloud file unlinked between fetch and store: stale sender does not recreate it.
    name = "cloud file unlinked before store";
    CHECK(!ss_pack(so, TID, cf, &mo), "re-pack original");
    write_base(sb, mo.hash);
    race_before_store = mutate_unlink_cloud;
    CHECK(send_save(sb, TID, cf) == 0 && access(cf, F_OK) == -1, "stale sender does not write over deleted cloud file");
    read_base(sb, base_b); CHECK(!strcmp(base_b, mo.hash), "stale sender retains previous base after unlink race");

    // 6. Cloud file overwritten in-place (same inode, new contents/size) between fetch and store.
    name = "in-place overwrite before store";
    CHECK(!ss_pack(so, TID, cf, &mo), "re-pack original for in-place test");
    CHECK(!ss_pack(sa, TID, race_replacement, &ma), "pack replacement A for in-place test");
    write_base(sb, mo.hash);
    race_before_store = mutate_inplace_cloud;
    CHECK(send_save(sb, TID, cf) == 0 && cloud_file_is(cf, root, "save-from-A"), "in-place overwrite detected as stale; competing save survives");
    read_base(sb, base_b); CHECK(!strcmp(base_b, mo.hash), "stale sender retains previous base after in-place race");

    rm_tree(root);
}
static int read_file_bytes(const char *path, uint8_t **out, size_t *len) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
    size_t n = (size_t)st.st_size;
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf) return -1;
    FILE *f = fopen(path, "rb");
    if (!f || (n && fread(buf, 1, n, f) != n)) { if (f) fclose(f); free(buf); return -1; }
    fclose(f);
    *out = buf; *len = n;
    return 0;
}
static int has_cloud_stage(const char *save) {
    char dir[PATH_MAX], **v; size_t n; int found = 0;
    snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) if (!strncmp(v[i], CLOUD_STAGE_PREFIX, sizeof CLOUD_STAGE_PREFIX - 1)) found = 1;
    free_names(v, n);
    return found;
}
// Run one full conflict mount+flush in a child process so each attempt has a clean sender thread and process state
static int conflict_mount_child(const char *save, const char *title, const char *cloud_dir, const char *choice,
                                int write_fail_after, int publish_fail) {
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) abort();
    if (!pid) {
        setenv("DM_SAVE_SYNC", cloud_dir, 1);
        setenv("DM_SAVE_SYNC_CHOICE", choice, 1);
        fail_stage_write_after = write_fail_after;
        stage_write_seen = 0;
        stage_write_fd = -1;
        fail_stage_publish = publish_fail;
        int mr = mount_once(save, title);
        int flushed = mr == 0 ? save_sync_flush(5000) : 0;
        _exit(mr != 0 ? 2 : flushed ? 0 : 1);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) return 99;
    return WEXITSTATUS(st);
}
static int verify_seeded_backups(const char *save, int first, int last) {
    char p[PATH_MAX], expect[64];
    for (int i = first; i <= last; i++) {
        snprintf(p, sizeof p, "%s-backups/2020-01-%02d 00.00.00 cloud/save/mark.txt", save, i);
        snprintf(expect, sizeof expect, "seeded-cloud-%02d", i);
        if (!is(p, expect)) return 0;
    }
    return 1;
}
static void conflict_cloud_backup_cases(const char *folder) {
    static const char *TID = "0100000000010000";
    const char *name = "conflict cloud backup";
    char root[PATH_MAX], cloud_dir[PATH_MAX], title_dir[PATH_MAX], cf[PATH_MAX];
    char orig[PATH_MAX], local[PATH_MAX], remote[PATH_MAX], backups[PATH_MAX], p[PATH_MAX], choice[64], base[SS_HASH + 1];
    ss_meta mo, ml, mr;
    snprintf(root, sizeof root, "%s/cloud-backup", folder);
    snprintf(cloud_dir, sizeof cloud_dir, "%s/cloud", root);
    snprintf(title_dir, sizeof title_dir, "%s/%s", cloud_dir, TID);
    snprintf(cf, sizeof cf, "%s/save.dmsync", title_dir);
    snprintf(orig, sizeof orig, "%s/orig/save", root);
    snprintf(local, sizeof local, "%s/local/save", root);
    snprintf(remote, sizeof remote, "%s/remote/save", root);
    snprintf(backups, sizeof backups, "%s-backups", local);
    rm_tree(root); mkdirs(title_dir); mkdirs(orig); mkdirs(local); mkdirs(remote);

    // Base save, then both sides diverge (conflict) with a multi-file cloud archive so restore fails mid-way
    snprintf(p, sizeof p, "%s/a", orig); put(p, "base-a");
    CHECK(!ss_scan(orig, &mo), "scan base save");
    snprintf(p, sizeof p, "%s/a", local); put(p, "local-a");
    write_base(local, mo.hash);
    CHECK(!ss_scan(local, &ml), "scan local save");
    snprintf(p, sizeof p, "%s/a", remote); put(p, "cloud-a");
    snprintf(p, sizeof p, "%s/b", remote); put(p, "cloud-b");
    CHECK(!ss_pack(remote, TID, cf, &mr), "pack competing cloud save");
    snprintf(choice, sizeof choice, "local:%s", mr.hash);

    uint8_t *cloud_before = NULL; size_t cloud_before_len = 0;
    CHECK(!read_file_bytes(cf, &cloud_before, &cloud_before_len), "snapshot initial cloud archive bytes");

    // Seed 10 recognizable completed cloud backups plus a user-created folder
    for (int i = 1; i <= KEEP_BACKUPS; i++) {
        char val[64];
        snprintf(p, sizeof p, "%s/2020-01-%02d 00.00.00 cloud/save", backups, i); mkdirs(p);
        snprintf(p, sizeof p, "%s/2020-01-%02d 00.00.00 cloud/save/mark.txt", backups, i);
        snprintf(val, sizeof val, "seeded-cloud-%02d", i);
        put(p, val);
    }
    snprintf(p, sizeof p, "%s/my-manual-backup", backups); mkdirs(p);
    snprintf(p, sizeof p, "%s/my-manual-backup/note.txt", backups); put(p, "keep-me");

    // Attempt 1 & 2: inject a mid-restore write failure (second file), then a publish rename failure; repeat to prove no rotation happens
    for (int attempt = 0; attempt < 2; attempt++) {
        int rc = conflict_mount_child(local, TID, cloud_dir, choice, attempt == 0 ? 2 : 0, attempt == 1 ? 1 : 0);
        CHECK(rc == 1, "failed conflict backup does not send local save");
        CHECK(count_cloud_backups(local) == KEEP_BACKUPS, "all ten previous cloud backups remain after failed restore");
        CHECK(verify_seeded_backups(local, 1, KEEP_BACKUPS), "every prior backup keeps its exact name and contents");
        CHECK(!has_cloud_stage(local), "failed conflict backup cleans up its staging folder");
        snprintf(p, sizeof p, "%s/my-manual-backup/note.txt", backups);
        CHECK(is(p, "keep-me"), "user-created folder survives failed restore");
        read_base(local, base);
        CHECK(!strcmp(base, mo.hash), "sync base does not advance when conflict backup fails");
        uint8_t *cloud_now = NULL; size_t cloud_now_len = 0;
        CHECK(!read_file_bytes(cf, &cloud_now, &cloud_now_len) && cloud_now_len == cloud_before_len && !memcmp(cloud_now, cloud_before, cloud_before_len),
              "cloud archive bytes remain identical after failed restore");
        free(cloud_now);
    }

    // Seed an interrupted leftover staging folder and verify the next mount sweeps it without touching backups or user folders
    snprintf(p, sizeof p, "%s/%s99999-0/save", backups, CLOUD_STAGE_PREFIX); mkdirs(p);
    snprintf(p, sizeof p, "%s/%s99999-0/save/partial", backups, CLOUD_STAGE_PREFIX); put(p, "interrupted");
    CHECK(has_cloud_stage(local), "seeded interrupted staging folder exists before next mount");
    CHECK(conflict_mount_child(local, TID, cloud_dir, choice, 1, 0) == 1, "third failed restore cleans up interrupted staging too");
    CHECK(!has_cloud_stage(local), "next mount removes leftover owned staging folder");
    CHECK(count_cloud_backups(local) == KEEP_BACKUPS && verify_seeded_backups(local, 1, KEEP_BACKUPS),
          "interrupted staging cleanup leaves all ten prior backups intact");
    snprintf(p, sizeof p, "%s/my-manual-backup/note.txt", backups);
    CHECK(is(p, "keep-me"), "interrupted staging cleanup leaves user-created folder intact");

    // Allow success: publish new cloud backup, prune oldest to KEEP_BACKUPS, advance base and upload local save
    CHECK(conflict_mount_child(local, TID, cloud_dir, choice, 0, 0) == 0, "conflict backup succeeds and uploads local save");
    CHECK(!has_cloud_stage(local), "no staging folder remains after successful backup");
    CHECK(count_cloud_backups(local) == KEEP_BACKUPS, "exactly ten completed cloud backups remain after rotation");
    snprintf(p, sizeof p, "%s/2020-01-01 00.00.00 cloud", backups);
    CHECK(access(p, F_OK) == -1 && errno == ENOENT, "oldest seeded cloud backup was rotated out");
    CHECK(verify_seeded_backups(local, 2, KEEP_BACKUPS), "newest nine seeded cloud backups remain intact");
    snprintf(p, sizeof p, "%s/my-manual-backup/note.txt", backups);
    CHECK(is(p, "keep-me"), "user-created folder survives rotation");
    // Locate the newly published cloud backup and verify both restored files
    char **v; size_t n; int found_new = 0;
    CHECK(!names_of(backups, &v, &n), "list backup directory after rotation");
    for (size_t i = 0; i < n; i++) {
        if (!is_backup(v[i], "cloud") || !strncmp(v[i], "2020-01-", 8)) continue;
        char pa[PATH_MAX], pb[PATH_MAX];
        snprintf(pa, sizeof pa, "%s/%s/save/a", backups, v[i]);
        snprintf(pb, sizeof pb, "%s/%s/save/b", backups, v[i]);
        if (is(pa, "cloud-a") && is(pb, "cloud-b")) found_new++;
    }
    free_names(v, n);
    CHECK(found_new == 1, "newly published cloud backup contains the complete restored cloud save");
    read_base(local, base);
    CHECK(!strcmp(base, ml.hash), "sync base advances to the uploaded local save");
    CHECK(cloud_file_is(cf, root, "local-a"), "cloud archive now holds the local save");

    free(cloud_before);
    rm_tree(root);
}
static void xattr_failure_cases(const char *folder) {
    static const char *TID = "0100000000010000";
    const char *name = "guest-name getxattr failures";
    char root[PATH_MAX], cloud_dir[PATH_MAX], title_dir[PATH_MAX], cf[PATH_MAX];
    char orig[PATH_MAX], local[PATH_MAX], out[PATH_MAX], unpacked[PATH_MAX], p[PATH_MAX], base[SS_HASH + 1];
    ss_meta mo, ml, mr;
    snprintf(root, sizeof root, "%s/xattr-fail", folder);
    snprintf(cloud_dir, sizeof cloud_dir, "%s/cloud", root);
    snprintf(title_dir, sizeof title_dir, "%s/%s", cloud_dir, TID);
    snprintf(cf, sizeof cf, "%s/save.dmsync", title_dir);
    snprintf(orig, sizeof orig, "%s/orig/save", root);
    snprintf(local, sizeof local, "%s/local/save", root);
    snprintf(out, sizeof out, "%s/packed.dmsync", root);
    snprintf(unpacked, sizeof unpacked, "%s/unpacked", root);
    rm_tree(root); mkdirs(title_dir); mkdirs(orig); mkdirs(local);

    snprintf(p, sizeof p, "%s/plain", orig); put(p, "previous-cloud");
    CHECK(!ss_pack(orig, TID, cf, &mo), "pack previous cloud save");
    uint8_t *cloud_before = NULL; size_t cloud_before_len = 0;
    CHECK(!read_file_bytes(cf, &cloud_before, &cloud_before_len), "snapshot previous cloud bytes");

    snprintf(p, sizeof p, "%s/plain", local); put(p, "ordinary-file");
    snprintf(p, sizeof p, "%s/.dm-case-1", local); put(p, "alias-content");
    CHECK(!setxattr(p, GUEST_NAME, "Plain", 5, 0, XATTR_NOFOLLOW), "attach guest-name xattr to .dm-case-1 alias");
    write_base(local, mo.hash);

    // Ordinary file lacks GUEST_NAME (ENOATTR) and succeeds alongside the attributed alias
    CHECK(!ss_scan(local, &ml) && ml.files == 2, "scan succeeds with ENOATTR on ordinary file and readable alias xattr");

    // Inject EIO reading the alias xattr (with cleanup_errno set to verify error preservation through ss_pack cleanup)
    fail_xattr_suffix = "/.dm-case-1";
    fail_xattr_errno = EIO;
    cleanup_errno = EBUSY;
    CHECK(ss_scan(local, &mr) == -1 && errno == EIO, "ss_scan propagates EIO from getxattr");
    CHECK(ss_pack(local, TID, out, &mr) == -1 && errno == EIO, "ss_pack propagates EIO from getxattr through cleanup");
    CHECK(access(out, F_OK) == -1 && errno == ENOENT, "failed ss_pack leaves no partial archive");
    CHECK(send_save(local, TID, cf) == 0, "send_save refuses to upload when getxattr fails");
    read_base(local, base);
    CHECK(!strcmp(base, mo.hash), "previous sync base is unchanged after getxattr failure");
    uint8_t *cloud_now = NULL; size_t cloud_now_len = 0;
    CHECK(!read_file_bytes(cf, &cloud_now, &cloud_now_len) && cloud_now_len == cloud_before_len && !memcmp(cloud_now, cloud_before, cloud_before_len),
          "previous cloud bytes are identical after getxattr failure");
    free(cloud_now);

    // Clear fault: send succeeds and the attributed alias round-trips its original guest name and contents
    fail_xattr_suffix = NULL;
    fail_xattr_errno = 0;
    cleanup_errno = 0;
    CHECK(send_save(local, TID, cf) == 1, "send_save succeeds once getxattr is readable");
    mkdirs(unpacked);
    CHECK(!ss_read(cf, unpacked, &mr) && !strcmp(mr.hash, ml.hash), "uploaded archive unpacks with matching hash");
    snprintf(p, sizeof p, "%s/save/plain", unpacked);
    CHECK(is(p, "ordinary-file"), "ordinary file round-trips");
    snprintf(p, sizeof p, "%s/save/.dm-case-1", unpacked);
    char g[16] = {0}; ssize_t gl = getxattr(p, GUEST_NAME, g, sizeof g - 1, 0, XATTR_NOFOLLOW);
    CHECK(is(p, "alias-content") && gl == 5 && !strcmp(g, "Plain"), "attributed .dm-case-1 alias round-trips guest name and contents");

    free(cloud_before);
    rm_tree(root);
}
static int count_start_backups(const char *save) {
    char dir[PATH_MAX], **v; size_t n; int k = 0; snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) k += is_backup(v[i], START_SIDE);
    free_names(v, n); return k;
}
static int has_start_stage(const char *save) {
    char dir[PATH_MAX], **v; size_t n; int found = 0; snprintf(dir, sizeof dir, "%s-backups", save);
    if (names_of(dir, &v, &n)) return 0;
    for (size_t i = 0; i < n; i++) if (!strncmp(v[i], START_STAGE_PREFIX, sizeof START_STAGE_PREFIX - 1)) found = 1;
    free_names(v, n); return found;
}
static void start_backup_cases(const char *folder) {
    const char *name = "start backup staging, hash cache, and budget";
    char root[PATH_MAX], save[PATH_MAX], backups[PATH_MAX], p[PATH_MAX], newest[PATH_MAX];
    snprintf(root, sizeof root, "%s/start-backup", folder);
    snprintf(save, sizeof save, "%s/save", root);
    snprintf(backups, sizeof backups, "%s-backups", save);
    rm_tree(root); mkdirs(save);
    unsetenv("DM_SAVE_SYNC"); unsetenv("DM_SAVE_BACKUPS");

    // 1. Seed 10 start backups + a player folder; inject clone failure and publish failure: neither prunes nor leaves staging.
    for (int i = 1; i <= KEEP_BACKUPS; i++) {
        snprintf(p, sizeof p, "%s/2020-01-%02d 00.00.00 start/save", backups, i); mkdirs(p);
        snprintf(p, sizeof p, "%s/2020-01-%02d 00.00.00 start/save/mark.txt", backups, i); put(p, "old");
    }
    snprintf(p, sizeof p, "%s/player-keep", backups); mkdirs(p);
    snprintf(p, sizeof p, "%s/player-keep/note.txt", backups); put(p, "keep");
    snprintf(p, sizeof p, "%s/a", save); put(p, "v1");

    fail_start_clone = 1;
    CHECK(mount_once(save, "0100000000010000") == 0, "clone failure does not block mount");
    CHECK(!fail_start_clone && !has_start_stage(save), "clone failure cleans up staging");
    CHECK(count_start_backups(save) == KEEP_BACKUPS, "clone failure prunes no existing start backups");
    snprintf(p, sizeof p, "%s/2020-01-01 00.00.00 start/save/mark.txt", backups);
    CHECK(is(p, "old"), "oldest start backup survives clone failure");

    fail_start_publish = 1;
    CHECK(mount_once(save, "0100000000010000") == 0, "publish failure does not block mount");
    CHECK(!fail_start_publish && !has_start_stage(save), "publish failure cleans up staging");
    CHECK(count_start_backups(save) == KEEP_BACKUPS, "publish failure prunes no existing start backups");
    CHECK(is(p, "old"), "oldest start backup survives publish failure");

    // 2. Leftover interrupted .start-stage.* is swept on mount, then valid start copy publishes and rotates oldest out.
    snprintf(p, sizeof p, "%s/%s99999-0/save", backups, START_STAGE_PREFIX); mkdirs(p);
    snprintf(p, sizeof p, "%s/%s99999-0/save/partial", backups, START_STAGE_PREFIX); put(p, "half");
    CHECK(has_start_stage(save), "seeded interrupted start staging exists");
    CHECK(mount_once(save, "0100000000010000") == 0, "mount sweeps staging and publishes start copy");
    CHECK(!has_start_stage(save), "no start staging remains after mount");
    CHECK(count_start_backups(save) == KEEP_BACKUPS, "start backups rotate to KEEP_BACKUPS after publish");
    snprintf(p, sizeof p, "%s/2020-01-01 00.00.00 start", backups);
    CHECK(access(p, F_OK) == -1 && errno == ENOENT, "oldest start backup rotated out");
    snprintf(p, sizeof p, "%s/player-keep/note.txt", backups);
    CHECK(is(p, "keep"), "player folder survives staging cleanup and rotation");

    // 3. Cached .hash skips re-scanning the newest backup's save/ directory; removing .hash falls back to ss_scan cleanly.
    newest_start_copy(save, newest, sizeof newest);
    CHECK(*newest != 0, "newest start copy found");
    snprintf(p, sizeof p, "%s/save", newest);
    chmod(p, 0);
    CHECK(mount_once(save, "0100000000010000") == 0 && count_start_backups(save) == KEEP_BACKUPS,
          "unchanged save deduplicates via .hash without reading backup save/");
    // Verify no second new copy was created (2020-01-02 still present)
    snprintf(p, sizeof p, "%s/2020-01-02 00.00.00 start", backups);
    CHECK(access(p, F_OK) == 0, "dedup via .hash created no duplicate backup");
    snprintf(p, sizeof p, "%s/save", newest); chmod(p, 0755);
    snprintf(p, sizeof p, "%s/%s", newest, START_HASH_FILE); unlink(p);
    CHECK(mount_once(save, "0100000000010000") == 0 && access(backups, F_OK) == 0, "mount without .hash succeeds");
    snprintf(p, sizeof p, "%s/2020-01-02 00.00.00 start", backups);
    CHECK(access(p, F_OK) == 0, "missing .hash falls back to ss_scan and still deduplicates");

    // 4. Same-second collision suffix ordering ("start" < "start 2" < ... < "start 10")
    rm_tree(backups);
    char ten_path[PATH_MAX];
    for (int k = 1; k <= 10; k++) {
        if (k == 1) snprintf(p, sizeof p, "%s/2025-05-05 12.00.00 start/save", backups);
        else snprintf(p, sizeof p, "%s/2025-05-05 12.00.00 start %d/save", backups, k);
        mkdirs(p);
        char fp[PATH_MAX], body[32];
        snprintf(fp, sizeof fp, "%s/a", p); snprintf(body, sizeof body, "seq-%d", k); put(fp, body);
        if (k == 10) snprintf(ten_path, sizeof ten_path, "%s/2025-05-05 12.00.00 start 10", backups);
    }
    newest_start_copy(save, newest, sizeof newest);
    CHECK(!strcmp(newest, ten_path), "same-second ordering picks start 10 over start 2");
    snprintf(p, sizeof p, "%s/a", save); put(p, "seq-10");
    CHECK(mount_once(save, "0100000000010000") == 0 && count_start_backups(save) == 10,
          "matching start 10 deduplicates without adding a copy");
    put(p, "seq-11");
    CHECK(mount_once(save, "0100000000010000") == 0 && count_start_backups(save) == 10,
          "adding an 11th same-second/later copy keeps 10");
    snprintf(p, sizeof p, "%s/2025-05-05 12.00.00 start", backups);
    CHECK(access(p, F_OK) == -1 && errno == ENOENT, "oldest same-second backup (no suffix) was pruned first");
    snprintf(p, sizeof p, "%s/2025-05-05 12.00.00 start 2", backups);
    CHECK(access(p, F_OK) == 0 && access(ten_path, F_OK) == 0, "start 2 and start 10 survive");

    // 5. Logical byte budget: cap total start backup bytes while always keeping at least the newest 1.
    rm_tree(backups);
    snprintf(p, sizeof p, "%s/2020-01-01 00.00.00 local/save", backups); mkdirs(p);
    snprintf(p, sizeof p, "%s/2020-01-01 00.00.00 local/save/keep", backups); put(p, "local-keep");
    int64_t saved_budget = g_start_budget;
    g_start_budget = 250;
    char blob[101]; memset(blob, 'A', 100); blob[100] = 0;
    for (int step = 0; step < 4; step++) {
        blob[0] = (char)('A' + step);
        snprintf(p, sizeof p, "%s/a", save); put(p, blob);
        CHECK(mount_once(save, "0100000000010000") == 0, "budgeted start mount succeeds");
    }
    CHECK(count_start_backups(save) == 2, "250-byte budget retains only the newest 2 of four 100-byte saves");
    newest_start_copy(save, newest, sizeof newest);
    snprintf(p, sizeof p, "%s/save/a", newest); blob[0] = 'D';
    CHECK(is(p, blob), "newest budgeted backup holds latest save");
    snprintf(p, sizeof p, "%s/2020-01-01 00.00.00 local/save/keep", backups);
    CHECK(is(p, "local-keep"), "budget pruning leaves local backups untouched");
    // Single save larger than budget still keeps itself (newest 1) and prunes older start backups
    char big[401]; memset(big, 'Z', 400); big[400] = 0;
    snprintf(p, sizeof p, "%s/a", save); put(p, big);
    CHECK(mount_once(save, "0100000000010000") == 0 && count_start_backups(save) == 1,
          "oversized single save always keeps the newest 1 start backup and trims older ones");
    newest_start_copy(save, newest, sizeof newest);
    snprintf(p, sizeof p, "%s/save/a", newest);
    CHECK(is(p, big), "oversized single start backup holds the complete save");
    g_start_budget = saved_budget;

    // 6. DM_SAVE_BACKUPS=0 disables start copies completely.
    rm_tree(backups);
    setenv("DM_SAVE_BACKUPS", "0", 1);
    CHECK(mount_once(save, "0100000000010000") == 0 && access(backups, F_OK) == -1,
          "DM_SAVE_BACKUPS=0 skips start backups completely");
    unsetenv("DM_SAVE_BACKUPS");

    rm_tree(root);
}
int main(void) {
    char folder[] = "/tmp/dm-save-sync-metadata-XXXXXX", save[PATH_MAX], path[PATH_MAX];
    if (!mkdtemp(folder)) return 1;
    snprintf(save, sizeof save, "%s/save", folder);
    run_case(save, "open failure", 1, 0, 0, 0, EACCES);
    run_case(save, "write failure", 0, 1, 0, 0, ENOSPC);
    run_case(save, "write and close failures", 0, 1, 1, 0, ENOSPC);
    run_case(save, "close failure", 0, 0, 1, 0, EIO);
    run_case(save, "rename failure", 0, 0, 0, 1, EROFS);
    run_case(save, "success", 0, 0, 0, 0, 0);
    snprintf(path, sizeof path, "%s.sync", save); unlink(path);
    {
        char root[PATH_MAX], both[PATH_MAX], one[PATH_MAX]; const char *name;
        struct fault none = {0};
        snprintf(root, sizeof root, "%s/pull", folder); mkdirs(root);
        cloud_bundle(root, 1, both, sizeof both); cloud_bundle(root, 0, one, sizeof one);
        name = "pull, nothing fails"; pull_case(root, name, both, 1, none, none, 0);
        name = "pull, the cloud's has no device save"; pull_case(root, name, one, 0, none, none, 0);
        // The move into the backups that the old pull did after its swap; here the first step, before anything is in place
        name = "pull, the cloud's save into the backups fails"; pull_case(root, name, both, 1, (struct fault){1, "in/save", "local/save", 0}, none, -1);
        name = "pull, the device save's swap fails: the save is swapped back"; pull_case(root, name, both, 1, (struct fault){2, "local/save_device", "/save_device", 0}, none, -1);
        name = "pull, the device save's move to the backups fails"; pull_case(root, name, one, 0, (struct fault){1, "/save_device", "local/save_device", 0}, none, -1);
        name = "pull, the device save's swap and the save's swap back fail";
        pull_case(root, name, both, 1, (struct fault){2, "local/save_device", "/save_device", 0}, (struct fault){2, "local/save", "/save", 1}, -2);
        {   char rs[PATH_MAX]; snprintf(rs, sizeof rs, "%s/save", root); snprintf(path, sizeof path, "%s.pull", rs);
            CHECK(!access(path, F_OK), "a pull that could not be put back keeps its journal");
            unsetenv("DM_SAVE_SYNC");
            CHECK(mount_once(rs, "0100000000010000") == 0 && as_before(root, HAD_SAVE), "the next start puts this device's save back"); }
        journal_cases(root, both, one);
        rm_tree(root);
    }
    fetch_cases(folder);
    concurrent_send_cases(folder);
    conflict_cloud_backup_cases(folder);
    xattr_failure_cases(folder);
    start_backup_cases(folder);
    rmdir(folder);
    printf("save-sync metadata: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
