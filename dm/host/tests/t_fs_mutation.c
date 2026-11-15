// Unexpected host failures while changing a save must reject the whole transaction,
// including failures before a physical path exists. Real APFS clones and commits;
// only the failing POSIX operation is replaced. No game data is needed.
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
static DIR *fault_opendir(const char *p);
static int fault_open(const char *p, int flags, ...);
static int fault_unlink(const char *p);
static int fault_mkdir(const char *p, mode_t mode);
static int fault_rmdir(const char *p);
static int fault_rename(const char *a, const char *b);
static int fault_ftruncate(int fd, off_t size);
static int fault_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opts);
static int fault_removexattr(const char *p, const char *n, int opts);
static int fault_access(const char *p, int mode);
#define opendir fault_opendir
#define open fault_open
#define unlink fault_unlink
#define mkdir fault_mkdir
#define rmdir fault_rmdir
#define rename fault_rename
#define ftruncate fault_ftruncate
#define setxattr fault_setxattr
#define removexattr fault_removexattr
#define access fault_access
#include "../hle_fs.c"
#undef opendir
#undef open
#undef unlink
#undef mkdir
#undef rmdir
#undef rename
#undef ftruncate
#undef setxattr
#undef removexattr
#undef access

struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;
static const char *fault;
static int fault_errno, injected, skip;
static int fail(const char *op) {
    if (!fault || strcmp(fault, op)) return 0;
    if (skip) { skip--; return 0; }
    fault = NULL; injected++; errno = fault_errno; return 1;
}
static DIR *fault_opendir(const char *p) { return fail("lookup") ? NULL : opendir(p); }
static int fault_open(const char *p, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    return fail("open") ? -1 : open(p, flags, mode);
}
static int fault_unlink(const char *p) { return fail("unlink") ? -1 : unlink(p); }
static int fault_mkdir(const char *p, mode_t mode) { return fail("mkdir") ? -1 : mkdir(p, mode); }
static int fault_rmdir(const char *p) { return fail("rmdir") ? -1 : rmdir(p); }
static int fault_rename(const char *a, const char *b) { return fail("rename") ? -1 : rename(a, b); }
static int fault_ftruncate(int fd, off_t size) { return fail("truncate") ? -1 : ftruncate(fd, size); }
static int fault_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opts) {
    return fail("metadata") ? -1 : setxattr(p, n, v, s, pos, opts);
}
static int fault_removexattr(const char *p, const char *n, int opts) { return fail("metadata") ? -1 : removexattr(p, n, opts); }
static int fault_access(const char *p, int mode) { return fail("probe") ? -1 : access(p, mode); }
static int checks, failures;
#define CHECK(c, name) do { checks++; if (!(c)) { failures++; printf("FAIL %s: %s\n", name, #c); } } while (0)
static void put(const char *p, const char *s) {
    FILE *f = fopen(p, "w"); if (!f || fputs(s, f) < 0 || fclose(f)) abort();
}
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f); fclose(f); return n == strlen(s) && !memcmp(b, s, n);
}
enum action { OPEN_WRITE, OPEN_APPEND, CREATE, DELETE, MKDIR, RMDIR, CLEAN, TREE, RENAME, RENAME_DIR, READ, STAT };
static Result act(enum action a, const char *mount_name) {
    char p[80], to[80]; uint64_t h = 0; int32_t t;
    const char *leaf = a == CREATE || a == MKDIR ? "new" : a == RMDIR || a == CLEAN || a == TREE || a == RENAME_DIR ? "dir" : "file";
    snprintf(p, sizeof p, "%s:/%s", mount_name, leaf); snprintf(to, sizeof to, "%s:/renamed", mount_name);
    switch (a) {
    case OPEN_WRITE: case OPEN_APPEND: case READ: {
        Result r = h_fs_OpenFile(&h, p, a == READ ? MODE_READ : a == OPEN_APPEND ? MODE_APPEND : MODE_WRITE);
        if (!r) h_fs_CloseFile(h); return r;
    }
    case CREATE: return h_fs_CreateFile(p, 10);
    case DELETE: return h_fs_DeleteFile(p);
    case MKDIR: return h_fs_CreateDirectory(p);
    case RMDIR: return h_fs_DeleteDir(p);
    case CLEAN: return h_fs_CleanDirRec(p);
    case TREE: return h_fs_DeleteDirRec(p);
    case RENAME: return h_fs_RenameFile(p, to);
    case RENAME_DIR: return h_fs_RenameDir(p, to);
    case STAT: return h_fs_GetEntryType(&t, p);
    }
    abort();
}
int main(void) {
    static const struct { const char *name; enum action action; const char *fault; int error, poisoned; const char *mount, *failat; } cases[] = {
        {"write lookup", OPEN_WRITE, "lookup", EIO, 1, "alias"},
        {"append lookup", OPEN_APPEND, "lookup", EACCES, 1, "alias"},
        {"create lookup", CREATE, "lookup", ENOMEM, 1, "alias"},
        {"delete lookup", DELETE, "lookup", EIO, 1, "alias"},
        {"mkdir lookup", MKDIR, "lookup", EIO, 1, "alias"},
        {"rmdir lookup", RMDIR, "lookup", EIO, 1, "alias"},
        {"clean lookup", CLEAN, "lookup", EIO, 1, "alias"},
        {"tree lookup", TREE, "lookup", EIO, 1, "alias"},
        {"rename lookup", RENAME, "lookup", EIO, 1, "alias"},
        {"directory rename lookup", RENAME_DIR, "lookup", EIO, 1, "alias"},
        {"rename destination lookup", RENAME, "lookup2", EIO, 1, "alias"},
        {"rename destination probe", RENAME, "probe", EIO, 1, "alias"},
        {"write open", OPEN_WRITE, "open", EMFILE, 1, "alias"},
        {"create open", CREATE, "open", ENOSPC, 1, "alias"},
        {"create size", CREATE, "truncate", ENOSPC, 1, "alias"},
        {"create name", CREATE, "metadata", EIO, 1, "alias"},
        {"delete", DELETE, "unlink", EACCES, 1, "alias"},
        {"mkdir", MKDIR, "mkdir", ENOSPC, 1, "alias"},
        {"directory name", MKDIR, "metadata", EIO, 1, "alias"},
        {"rmdir", RMDIR, "rmdir", EIO, 1, "alias"},
        {"clean child", CLEAN, "unlink", EIO, 1, "alias"},
        {"tree child", TREE, "unlink", EIO, 1, "alias"},
        {"rename", RENAME, "rename", EIO, 1, "alias"},
        {"directory rename", RENAME_DIR, "rename", EIO, 1, "alias"},
        {"rename name", RENAME, "metadata", EIO, 1, "alias"},
        {"directory rename name", RENAME_DIR, "metadata", EIO, 1, "alias"},
        {"rename name and rollback", RENAME, "metadata", ENAMETOOLONG, 1, "alias", "renameback:1"},   // the file stays at the new path: refused whatever the first error
        {"read lookup", READ, "lookup", EIO, 0, "alias"},
        {"read open", READ, "open", EIO, 0, "alias"},
        {"stat lookup", STAT, "lookup", EIO, 0, "alias"},
        {"readonly lookup", OPEN_WRITE, "lookup", EIO, 0, "readonly"},
        {"nontransactional lookup", CREATE, "lookup", EIO, 0, "scratch"},
        {"missing lookup", OPEN_WRITE, "lookup", ENOENT, 0, "alias"},
        {"not directory lookup", CREATE, "lookup", ENOTDIR, 0, "alias"},
        {"long name", CREATE, "lookup", ENAMETOOLONG, 0, "alias"},
        {"invalid name", CREATE, "open", EILSEQ, 0, "alias"},
        {"rename name refused", RENAME, "metadata", ENAMETOOLONG, 0, "alias"},   // rolled back: the working copy is unchanged
        {"exists", CREATE, "open", EEXIST, 0, "alias"},
        {"invalid size", CREATE, "truncate", EINVAL, 0, "alias"},
        {"directory as file", OPEN_WRITE, "open", EISDIR, 0, "alias"},
        {"nonempty directory", RMDIR, "rmdir", ENOTEMPTY, 0, "alias"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char root[] = "/private/tmp/t_fs_mutation.XXXXXX", save[1024], other[1024], p[1100], marker[1100], other_marker[1100];
        if (!mkdtemp(root)) abort();
        snprintf(save, sizeof save, "%s/save", root); snprintf(other, sizeof other, "%s/save-other", root);
        if (mkdir(save, 0755) || mkdir(other, 0755)) abort();
        snprintf(marker, sizeof marker, "%s/marker", save); put(marker, "old");
        snprintf(other_marker, sizeof other_marker, "%s/marker", other); put(other_marker, "old");
        snprintf(p, sizeof p, "%s/file", save); put(p, "keep");
        snprintf(p, sizeof p, "%s/dir", save); if (mkdir(p, 0755)) abort();
        snprintf(p, sizeof p, "%s/dir/child", save); put(p, "child");
        g_nmnt = 0; memset(g_mnt, 0, sizeof g_mnt); g_nfail = g_lasterr = 0;
        CHECK(mount_ex("save", save, 0, 1) == RES_OK, cases[i].name);
        CHECK(mount_ex("alias", save, 0, 1) == RES_OK, cases[i].name);
        CHECK(mount_ex("save-other", other, 0, 1) == RES_OK, cases[i].name);
        CHECK(mount("readonly", save, 1) == RES_OK && mount("scratch", other, 0) == RES_OK, cases[i].name);
        snprintf(p, sizeof p, "%s/marker", g_mnt[0].dir); put(p, "next");
        snprintf(p, sizeof p, "%s/marker", g_mnt[2].dir); put(p, "other-next");
        fault = cases[i].fault; skip = !strcmp(fault, "lookup2"); if (skip) fault = "lookup";
        fault_errno = cases[i].error; injected = 0;
        if (cases[i].failat) setenv("DM_TEST_FS_FAILAT", cases[i].failat, 1);
        CHECK(act(cases[i].action, cases[i].mount) != RES_OK, cases[i].name);
        unsetenv("DM_TEST_FS_FAILAT");
        CHECK(injected == 1, cases[i].name); fault = NULL;
        CHECK(h_fs_Commit("save-other") == RES_OK && same(other_marker, "other-next"), cases[i].name);
        CHECK(h_fs_Commit("save") == (cases[i].poisoned ? RES_FAIL : RES_OK), cases[i].name);
        CHECK(same(marker, cases[i].poisoned ? "old" : "next"), cases[i].name);
        CHECK(g_nfail == cases[i].poisoned && (!g_nfail || g_lasterr == cases[i].error), cases[i].name);
        if (cases[i].poisoned) {
            snprintf(p, sizeof p, "%s/marker", g_mnt[0].dir); CHECK(same(p, "old"), cases[i].name);
            put(p, "recovered"); CHECK(h_fs_Commit("alias") == RES_OK && same(marker, "recovered"), cases[i].name);
        }
        if (rm_tree(root)) abort();
    }
    printf("t_fs_mutation: %d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
