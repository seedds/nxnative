// Real folder-backed OpenDirectory: allocation failure must return an error,
// leave the output untouched, and close the DIR already opened by the handler.
// A folder whose reading fails partway (readdir's NULL with EIO) is a failure everywhere, never a short listing taken for the whole.
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

static void *directory_calloc(size_t count, size_t size);
static DIR *directory_opendir(const char *path);
static int directory_closedir(DIR *d);
static struct dirent *directory_readdir(DIR *d);
#define calloc directory_calloc
#define opendir directory_opendir
#define closedir directory_closedir
#define readdir directory_readdir
#include "../hle_fs.c"
#undef readdir
#undef closedir
#undef opendir
#undef calloc

struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

static int fail_handle, failed_allocations, opened_fd = -1;
static void *directory_calloc(size_t count, size_t size) {
    if (fail_handle && count == 1 && size == sizeof(dirh_t)) {
        failed_allocations++;
        errno = ENOMEM;
        return NULL;
    }
    return calloc(count, size);
}
// One injected read failure: the next folder opened at fail_dir (trailing slashes aside) gives fail_after entries, then NULL with EIO; then injection is off
static char fail_dir[1100]; static DIR *fail_d; static int fail_after, read_failures;
static int same_dir(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    while (la > 1 && a[la - 1] == '/') la--;
    while (lb > 1 && b[lb - 1] == '/') lb--;
    return la == lb && !strncmp(a, b, la);
}
static void fail_reading(const char *dir, int after) { strlcpy(fail_dir, dir, sizeof fail_dir); fail_after = after; fail_d = NULL; }
static DIR *directory_opendir(const char *path) {
    DIR *d = opendir(path);
    if (d) opened_fd = dirfd(d);
    if (d && fail_dir[0] && same_dir(path, fail_dir)) fail_d = d;
    return d;
}
static struct dirent *directory_readdir(DIR *d) {
    if (d && d == fail_d) {
        struct dirent *e;
        while ((e = readdir(d)) && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))) {}
        if (e && fail_after-- > 0) return e;
        fail_dir[0] = 0; fail_d = NULL; read_failures++; errno = EIO; return NULL;
    }
    return readdir(d);
}

static int directory_closedir(DIR *d) {
    if (d == fail_d) fail_d = NULL;
    int r = closedir(d);
    // Successful cleanup may change errno; it must not hide the allocation error.
    if (fail_handle) errno = ENOENT;
    return r;
}

static int checks, fails;
static void drop_indexes(void) {   // the read-only folders' name indexes, built again at the next lookup
    for (dir_index **slot = g_dir_index; slot < g_dir_index + DIR_INDEX_SLOTS; slot++)
        while (*slot) { dir_index *x = *slot; *slot = x->next; free(x->e); free(x->pool); free(x->path); free(x); }
}
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "PASS" : "FAIL", text); } while (0)

int main(void) {
    char root[1024], file[1100];
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/t_fs_directory.XXXXXX", tmp ? tmp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(file, sizeof file, "%s/entry", root);
    FILE *f = fopen(file, "w");
    if (!f) { perror("fopen"); rmdir(root); return 2; }
    fputs("hello", f); fclose(f);
    CHECK(mount("test", root, 0) == RES_OK, "mount the synthetic folder");

    const uint64_t initial[] = {0, 0x1234};
    for (size_t i = 0; i < sizeof initial / sizeof initial[0]; i++) {
        uint64_t h = initial[i];
        opened_fd = -1; failed_allocations = 0; fail_handle = 1;
        Result r = h_fs_OpenDirectory(&h, "test:/", DIRMODE_DIR | DIRMODE_FILE);
        int error = errno;
        fail_handle = 0;
        CHECK(failed_allocations == 1, "inject failure into the directory-handle allocation");
        CHECK(r == RES_FAIL, "failed allocation returns the filesystem resource failure");
        CHECK(error == ENOMEM, "allocation error survives directory cleanup");
        CHECK(h == initial[i], "failed open leaves the caller's handle untouched");
        CHECK(opened_fd >= 0, "the handler opened a real directory before allocation failed");
        errno = 0;
        CHECK(fcntl(opened_fd, F_GETFD) == -1 && errno == EBADF, "failed open closes the real directory descriptor");
    }
    h_fs_CloseDirectory(0);

    uint64_t h = 0;
    Result r = h_fs_OpenDirectory(&h, "test:/", DIRMODE_DIR | DIRMODE_FILE);
    int fd = opened_fd;
    CHECK(r == RES_OK && h != 0, "a later successful open publishes a handle");
    if (r == RES_OK && h) {
        CHECK(fcntl(fd, F_GETFD) >= 0, "successful open keeps its directory descriptor alive");
        int64_t count = -1;
        CHECK(h_fs_DirCount(&count, h) == RES_OK && count == 1, "the successful handle counts the one synthetic file");
        DirectoryEntry entry[2] = {0};
        CHECK(h_fs_ReadDirectory(&count, entry, h, 2) == RES_OK && count == 1 &&
              !strcmp(entry[0].name, "entry") && entry[0].type == ENT_FILE && entry[0].size == 5,
              "the successful handle reads the file name, type and size");
        h_fs_CloseDirectory(h);
        errno = 0;
        CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "CloseDirectory releases the successful handle's descriptor");
    }

    // Reading fails partway: three names in a read-only folder (name index), a writable folder with a subfolder (path walk, listing, count, clean) and an overlay over a base
    char ro[1100], w[1100], sub[1200], ov[1100], q[1300];
    snprintf(ro, sizeof ro, "%s.ro", root); snprintf(w, sizeof w, "%s.w", root); snprintf(sub, sizeof sub, "%s/sub", w); snprintf(ov, sizeof ov, "%s.ov", root);
    mkdir(ro, 0755); mkdir(w, 0755); mkdir(sub, 0755); mkdir(ov, 0755);
    const char *three[] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        snprintf(q, sizeof q, "%s/%s", ro, three[i]); fclose(fopen(q, "w"));
        snprintf(q, sizeof q, "%s/%s", sub, three[i]); fclose(fopen(q, "w"));
        snprintf(q, sizeof q, "%s/o%s", ov, three[i]); fclose(fopen(q, "w"));
    }
    CHECK(mount("ro", ro, 1) == RES_OK && mount("w", w, 0) == RES_OK && mount("ov", root, 1) == RES_OK, "mount a read-only folder, a writable one and one with an overlay");
    strlcpy(g_mnt[g_nmnt - 1].overlay, ov, sizeof g_mnt[0].overlay);

    for (int after = 0; after <= 1; after++) {
        fail_reading(ro, after); int opened = 0, before = read_failures;
        for (int i = 0; i < 3; i++) {
            uint64_t fh = 0; char path[16]; snprintf(path, sizeof path, "ro:/%s", three[i]);
            if (h_fs_OpenFile(&fh, path, MODE_READ) == RES_OK) { opened++; h_fs_CloseFile(fh); }
        }
        CHECK(read_failures == before + 1 && opened == 3, after ? "a name index whose folder fails after one entry is not kept: every name still opens" : "a name index whose folder fails at once is not kept: every name still opens");
        drop_indexes();   // the next round builds it again
    }

    uint64_t fh = 0x1234; fail_reading(sub, 1);
    Result rr = h_fs_OpenFile(&fh, "w:/sub/zz", MODE_READ);
    CHECK(read_failures && rr != RES_OK && rr != RES_PATH_NOT_FOUND && fh == 0x1234, "a path walk whose folder cannot be read fails, not with not found");

    fail_reading(sub, 1); int64_t count = -1; DirectoryEntry ents[8];
    uint64_t dh = 0;
    CHECK(h_fs_OpenDirectory(&dh, "w:/sub", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&count, ents, dh, 8) != RES_OK, "a listing that fails after one entry fails");
    if (dh) h_fs_CloseDirectory(dh);
    dh = 0; CHECK(h_fs_OpenDirectory(&dh, "w:/sub", DIRMODE_FILE) == RES_OK, "open the folder again");
    fail_reading(sub, 1); CHECK(dh && h_fs_DirCount(&count, dh) != RES_OK, "a count that fails after one entry fails");
    CHECK(dh && h_fs_DirCount(&count, dh) == RES_OK && count == 3 && h_fs_ReadDirectory(&count, ents, dh, 8) == RES_OK && count == 3, "then the folder counts and lists its three files");
    if (dh) h_fs_CloseDirectory(dh);

    dh = 0x1234; fail_reading(ov, 1);
    CHECK(h_fs_OpenDirectory(&dh, "ov:/", DIRMODE_FILE) != RES_OK && dh == 0x1234, "an overlay folder that cannot be read fails the open and leaves the handle");
    dh = 0; count = 0;
    CHECK(h_fs_OpenDirectory(&dh, "ov:/", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&count, ents, dh, 8) == RES_OK && count == 4, "then the overlay lists its three files and the base's one");
    if (dh) h_fs_CloseDirectory(dh);

    // A name in several layers is listed once, as the first layer has it; the layers come in their order (the last overlay folder first), and names differing in case are two
    char ov2[1100]; snprintf(ov2, sizeof ov2, "%s.ov2", root); mkdir(ov2, 0755);
    snprintf(q, sizeof q, "%s/entry", ov); FILE *g = fopen(q, "w"); fputs("overlay!", g); fclose(g);
    snprintf(q, sizeof q, "%s/ENTRY", ov2); fclose(fopen(q, "w"));
    snprintf(q, sizeof q, "%s/oa", ov2); fclose(fopen(q, "w"));
    snprintf(g_mnt[g_nmnt - 1].overlay, sizeof g_mnt[0].overlay, "%s:%s", ov, ov2);
    drop_indexes();   // a read-only folder's index is kept for the run: the folder just changed, which a game's never does
    dh = 0; count = 0; int once = 0, upper = 0, big = 0;
    CHECK(h_fs_OpenDirectory(&dh, "ov:/", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&count, ents, dh, 8) == RES_OK && count == 5,
          "the layers list oa, ob, oc, entry and ENTRY, each once");
    for (int64_t i = 0; i < count; i++) { once += !strcmp(ents[i].name, "entry"); upper += !strcmp(ents[i].name, "ENTRY"); big += !strcmp(ents[i].name, "entry") && ents[i].size == 8; }
    CHECK(once == 1 && upper == 1 && big == 1, "entry is the overlay's (8 bytes, not the base's 5), and ENTRY is a name of its own");
    CHECK(count == 5 && (!strcmp(ents[0].name, "ENTRY") || !strcmp(ents[0].name, "oa")) && (!strcmp(ents[1].name, "ENTRY") || !strcmp(ents[1].name, "oa")), "the last overlay folder's names come first");
    if (dh) h_fs_CloseDirectory(dh);
    snprintf(q, sizeof q, "%s/entry", ov); unlink(q); snprintf(q, sizeof q, "%s/ENTRY", ov2); unlink(q); snprintf(q, sizeof q, "%s/oa", ov2); unlink(q); rmdir(ov2);

    fail_reading(sub, 1);
    CHECK(h_fs_CleanDirRec("w:/sub") != RES_OK, "a clean whose folder cannot be read fails");
    CHECK(h_fs_CleanDirRec("w:/sub") == RES_OK && rmdir(sub) == 0, "then it empties the folder");
    for (int i = 0; i < 3; i++) { snprintf(q, sizeof q, "%s/%s", ro, three[i]); unlink(q); snprintf(q, sizeof q, "%s/o%s", ov, three[i]); unlink(q); }
    CHECK(!rmdir(w) && !rmdir(ro) && !rmdir(ov), "remove the read-only, writable and overlay folders");
    CHECK(!unlink(file) && !rmdir(root), "remove the synthetic fixture");
    printf("== fs directory: %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
