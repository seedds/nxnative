#include "save_backup_mac.h"
#include "save_sync.h"
#include <copyfile.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <removefile.h>

static void discard(const char *p) { removefile(p, NULL, REMOVEFILE_RECURSIVE); }
static int path(char *out, const char *root, const char *suffix) {
    if (snprintf(out, PATH_MAX, "%s%s", root, suffix) < PATH_MAX) return 0;
    errno = ENAMETOOLONG; return -1;
}
// Resolve the same effective path as the host, including a trailing slash, before deriving `_device`.
static int save_path(const char *save, char *out) {
    if (!save || !*save) { errno = EINVAL; return -1; }
    char p[PATH_MAX]; struct stat st;
    if (path(p, save, "")) return -1;
    size_t n = strlen(p); while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    // Resolving a symlink leaf would silently move the sibling `_device` save to another folder.
    if (lstat(p, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = EINVAL; return -1; }
    return realpath(save, out) ? 0 : -1;
}
static int snapshot(const char *save, const char *dir) {
    for (int i = 0; i < 2; i++) {
        char from[PATH_MAX], to[PATH_MAX]; struct stat st;
        if (path(from, save, i ? "_device" : "") || path(to, dir, i ? "/save_device" : "/save")) return -1;
        if (lstat(from, &st)) { if (errno == ENOENT) continue; return -1; }
        if (!S_ISDIR(st.st_mode)) { errno = EINVAL; return -1; }
        if (dm_fs_clone_committed(from, to)) return -1;
    }
    return 0;
}

int mac_save_export(const char *save, const char *title, const char *archive) {
    char root[PATH_MAX], tmp[PATH_MAX], snap[PATH_MAX], out[PATH_MAX];
    if (save_path(save, root) || path(tmp, root, ".export-XXXXXX") || path(out, archive, ".partial-XXXXXX")) return -1;
    if (!mkdtemp(tmp)) return -1;
    int fd = mkstemp(out), r = -1; ss_meta meta;
    if (fd < 0) goto done;
    close(fd);
    if (snapshot(root, tmp) || path(snap, tmp, "/save") || ss_pack(snap, title, out, &meta)) goto done;
    if (!meta.files) { errno = ENODATA; goto done; }
    // A failed pack never overwrites a previously exported archive.
    r = rename(out, archive);
done:;
    int e = errno; if (r) unlink(out); discard(tmp); errno = e; return r;
}

int mac_save_restore(const char *save, const char *title, const char *archive, char *kept, size_t size) {
    char root[PATH_MAX], tmp[PATH_MAX], recovery[PATH_MAX]; ss_meta meta;
    if (size) *kept = 0;
    // Check before making a recovery folder or changing the save. Never accept another game's backup.
    if (ss_read(archive, NULL, &meta) || strcasecmp(meta.title, title) || !meta.files) { errno = EINVAL; return -1; }
    if (save_path(save, root) || path(tmp, root, ".restore-XXXXXX") || path(recovery, root, ".before-restore-XXXXXX")) return -1;
    if (strlen(recovery) + 1 > size) { errno = ENAMETOOLONG; return -1; }
    if (!mkdtemp(tmp)) return -1;
    int r = -1, changed = 0, existed[2] = {0};
    char here[2][PATH_MAX], incoming[2][PATH_MAX];
    if (ss_read(archive, tmp, &meta) || strcasecmp(meta.title, title) || !meta.files) { errno = EINVAL; goto done; }
    // Prepare both directories before touching either. A backup without a device save removes the old device save too.
    for (int i = 0; i < 2; i++) {
        struct stat st;
        if (path(here[i], root, i ? "_device" : "") || path(incoming[i], tmp, i ? "/save_device" : "/save")) goto done;
        if (mkdir(incoming[i], 0700) && errno != EEXIST) goto done;
        if (!lstat(here[i], &st)) { if (!S_ISDIR(st.st_mode)) { errno = EINVAL; goto done; } existed[i] = 1; }
        else if (errno != ENOENT) goto done;
    }
    if (!mkdtemp(recovery)) goto done;
    if (snapshot(root, recovery)) { int e = errno; discard(recovery); errno = e; goto done; }
    strlcpy(kept, recovery, size);
    for (int i = 0; i < 2; i++) {
        if (existed[i] ? renamex_np(incoming[i], here[i], RENAME_SWAP) : rename(incoming[i], here[i])) {
            int e = errno;
            for (int j = changed - 1; j >= 0; j--) {
                if (existed[j] ? renamex_np(incoming[j], here[j], RENAME_SWAP) : rename(here[j], incoming[j])) {
                    // Leave both staging and recovery folders available if rollback itself fails.
                    fprintf(stderr, "[RED] save restore rollback failed; recovery %s, staging %s\n", recovery, tmp);
                    return -1;
                }
            }
            errno = e; goto done;
        }
        changed++;
    }
    r = 0;
done:;
    int e = errno; discard(tmp); errno = e; return r;
}
