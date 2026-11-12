// Exercise the real metadata writer with stdio failures, and the real pull (the cloud's save swapped in) with rename failures, without cloud or game files.
// pull: every step that can fail, failing: this device's save stays whole and nowhere but in place or in its backups, and the staging folder (cleared at every
// start) never holds this device's save, also while a pull that succeeds is between two steps (the app killed there).
// The journal (<save>.pull): the app killed after each rename or swap of a pull (a longjmp out of it, nothing undone), with this device's folders holding a save, empty or
// absent, then the next start's mount (sync off): this device's save is back as it was, the cloud's gone, the journal removed; killed again in the middle of that recovery,
// the next one finishes it. A recovery that fails refuses the mount and keeps the journal, and the start after it recovers; an unreadable journal refuses the mount; a
// second mount gets the first's answer. Older backups are pruned only when a pull completes, and nothing is sent after the host while a journal is pending
#include <errno.h>
#include <setjmp.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail_open, fail_write, fail_close, fail_rename, cleanup_errno;
static int closes, renames, unlinks, metadata_fd;
static FILE *metadata;
static char diagnostic[2048];

static FILE *test_fopen(const char *path, const char *mode) {
    if (!strcmp(mode, "w")) {
        if (fail_open) { errno = EACCES; return NULL; }
        metadata = fopen(path, mode);
        if (metadata) metadata_fd = fileno(metadata);
        return metadata;
    }
    return fopen(path, mode);
}
static int test_fprintf(FILE *f, const char *fmt, ...) {
    if (f == metadata && fail_write) { errno = ENOSPC; return -1; }
    va_list ap; va_start(ap, fmt);
    int r = f == stderr ? vsnprintf(diagnostic, sizeof diagnostic, fmt, ap) : vfprintf(f, fmt, ap);
    va_end(ap); return r;
}
static int test_fclose(FILE *f) {
    int tracked = f == metadata;
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
static int test_rename(const char *from, const char *to) {
    renames++;
    if (fail_rename) { errno = EROFS; return -1; }
    if (faulted(1, from, to)) return -1;
    int r = rename(from, to);
    if (*staging && holds(staging, "original")) staging_held = 1;
    if (!r) crash_point();
    return r;
}
static int test_renamex_np(const char *from, const char *to, unsigned flags) {
    if (faulted(2, from, to)) return -1;
    int r = renamex_np(from, to, flags);
    if (*staging && holds(staging, "original")) staging_held = 1;
    if (!r) crash_point();
    return r;
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
#define rename test_rename
#define unlink test_unlink
#define renamex_np test_renamex_np
#include "../save_sync.c"
#undef fopen
#undef fprintf
#undef fclose
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
    write_base(save, new_hash);
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
    char b[64] = {0}; FILE *f = fopen(path, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
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
// This device's save as device_save made it, and nothing of the cloud's anywhere under root but the bundles
static int as_before(const char *root, int dev_had) {
    char p[PATH_MAX], dev[PATH_MAX], **v; size_t n; int ok = 1;
    snprintf(p, sizeof p, "%s/save/a", root); ok &= is(p, "original 0");
    snprintf(p, sizeof p, "%s/save", root); if (!names_of(p, &v, &n)) { ok &= n == 1; free_names(v, n); } else ok = 0;
    snprintf(dev, sizeof dev, "%s/save_device", root);
    if (dev_had == HAD_SAVE) { snprintf(p, sizeof p, "%s/b", dev); ok &= is(p, "original 1"); }
    else if (dev_had == HAD_EMPTY) ok &= !access(dev, F_OK) && !has_entries(dev);
    else ok &= access(dev, F_OK) == -1;
    snprintf(p, sizeof p, "%s/save", root); ok &= !holds(p, "cloud") && !holds(dev, "cloud");
    snprintf(p, sizeof p, "%s/save-backups", root); ok &= !holds(p, "cloud") && !holds(p, "original");
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
    rmdir(folder);
    printf("save-sync metadata: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
