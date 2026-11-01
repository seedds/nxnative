// Saves written before backslashes separated path components: expectations are hand-written; runs real file operations on APFS (clonefile).
// A build before that change took save:\savedata0.bin as a file named "\savedata0.bin" (The Legend of Zelda: Link's Awakening). The mount copies each such entry to the path
// its name names now, in the work directory only, never over an existing entry, once (an erased copy stays erased), and keeps the old entry.
#include <sys/clonefile.h>                // declared before the define, so only hle_fs.c's calls go to the fake
static int fake_clonefile(const char *src, const char *dst, uint32_t flags);
#define clonefile fake_clonefile
#include "../hle_fs.c"
#undef clonefile
static int g_clone_backslash_fail;       // 1: cloning an entry with a backslash in its name fails as on a full disk
static int fake_clonefile(const char *src, const char *dst, uint32_t flags) {
    if (g_clone_backslash_fail && strchr(src, '\\')) { errno = ENOSPC; return -1; }
    return clonefile(src, dst, flags);
}
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
static int checks, fails;
#define CHECK(c, text) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", text); } while (0)
static void put(const char *dir, const char *name, const char *s) { char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "w"); if (f) { fputs(s, f); fclose(f); } }
static int same(const char *dir, const char *name, const char *s) {
    char p[1300], b[64] = {0}; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "r"); if (!f) return 0;
    size_t k = fread(b, 1, sizeof b - 1, f); fclose(f); return k == strlen(s) && !memcmp(b, s, k);
}
static int there(const char *dir, const char *name) { char p[1300]; snprintf(p, sizeof p, "%s/%s", dir, name); return !access(p, F_OK); }
static int content(const char *path, const char *want) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0;
    char b[64] = {0}; size_t n = 0; Result r = h_fs_ReadFile(&n, h, 0, b, sizeof b);
    h_fs_CloseFile(h); return !r && n == strlen(want) && !memcmp(b, want, n);
}
static int write_text(const char *path, const char *text) {
    uint64_t h; if (h_fs_OpenFile(&h, path, MODE_WRITE)) return 0;
    h_fs_SetFileSize(h, (int64_t)strlen(text));
    Result r = h_fs_WriteFile(h, 0, text, strlen(text), NULL); h_fs_CloseFile(h); return !r;
}
static int listed(const char *path, const char *name) {   // how many times the listing has name
    uint64_t h; if (h_fs_OpenDirectory(&h, path, DIRMODE_DIR | DIRMODE_FILE)) return -1;
    DirectoryEntry e[32]; int64_t n = 0; int found = 0;
    h_fs_ReadDirectory(&n, e, h, 32); h_fs_CloseDirectory(h);
    for (int i = 0; i < n; i++) found += !strcmp(e[i].name, name);
    return found;
}
int main(void) {
    char root[1024], save[1100], work[1200], p[1300];
    const char *tmp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/backslash.XXXXXX", tmp ? tmp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    snprintf(save, sizeof save, "%s/save", root); snprintf(work, sizeof work, "%s.work", save); mkdir(save, 0755);
    // The committed save as a build before the change left it
    put(save, "\\savedata0.bin", "slot0"); put(save, "\\savedata3.bin", "slot3"); put(save, "\\screenshot0.bin", "shot0");
    put(save, "\\kept.bin", "old"); put(save, "kept.bin", "current");                     // both names: the new one is never overwritten
    snprintf(p, sizeof p, "%s/\\dir", save); mkdir(p, 0755); put(p, "f", "in-dir");       // save:\dir made as a directory, then save:\dir/f
    put(save, "\\a\\b.bin", "two");                                                        // save:\a\b.bin: one name with two backslashes
    snprintf(p, sizeof p, "%s/sub", save); mkdir(p, 0755); put(p, "x\\y.bin", "deep");    // save:/sub/x\y.bin: below an ordinary directory
    put(save, "case.bin", "lower"); put(save, "\\Case.bin", "upper");                     // the copy's name differs from an entry only by case
    put(save, "\\..\\up.bin", "up");                                                       // a name that leads above the save's root
    setenv("DM_SAVE", save, 1);
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "mount the save");
    CHECK(content("save:\\savedata0.bin", "slot0") && content("save:/savedata0.bin", "slot0"), "save:\\savedata0.bin reads the old \\savedata0.bin");
    CHECK(content("save:\\savedata3.bin", "slot3") && content("save:\\screenshot0.bin", "shot0"), "and so do the other slot and the screenshot");
    CHECK(content("save:/kept.bin", "current"), "an entry that has the new name already is not overwritten");
    CHECK(content("save:\\dir\\f", "in-dir"), "a directory with an old name is copied with its files");
    CHECK(content("save:\\a\\b.bin", "two"), "a name with two backslashes becomes a file in a new directory");
    CHECK(content("save:/sub/x/y.bin", "deep"), "an old name below an ordinary directory is copied too");
    CHECK(content("save:/Case.bin", "upper") && content("save:/case.bin", "lower"), "a copy whose name differs from an entry by case only gets an entry of its own");
    CHECK(!there(root, "up.bin") && !there(work, "up.bin") && h_fs_OpenFile(&(uint64_t){0}, "save:/up.bin", MODE_READ) == RES_PATH_NOT_FOUND, "a name that leads above the root is not copied anywhere");
    CHECK(there(work, "\\savedata0.bin") && there(work, "\\dir/f"), "the old entries stay");
    CHECK(!there(save, "savedata0.bin") && same(save, "\\savedata0.bin", "slot0") && !there(save, "dir"), "the committed save is untouched before the game commits");
    CHECK(h_fs_GetEntryType(&(int32_t){0}, "save:/savedata0.bin") == RES_OK && listed("save:/", "savedata0.bin") == 1, "the copy is listed once");

    g_nmnt = 0;   // a restart before any Commit: the work directory is cloned again from the untouched committed save
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && content("save:\\savedata0.bin", "slot0"), "a remount before a Commit copies again");
    CHECK(listed("save:/", "savedata0.bin") == 1 && listed("save:/", "Case.bin") == 1, "still once each, no second alias");

    CHECK(write_text("save:\\savedata0.bin", "progress") && h_fs_Commit("save") == RES_OK, "the game saves slot 0 and commits");
    CHECK(same(save, "savedata0.bin", "progress") && same(save, "\\savedata0.bin", "slot0"), "the committed save has the new name, and the old entry as it was");
    CHECK(same(save, "savedata3.bin", "slot3") && same(save, "dir/f", "in-dir"), "with every copy");
    g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && content("save:\\savedata0.bin", "progress"), "after a remount the game reads its own save, not the old entry");

    CHECK(h_fs_DeleteFile("save:\\savedata3.bin") == RES_OK && h_fs_DeleteDirRec("save:\\dir") == RES_OK && h_fs_Commit("save") == RES_OK, "the game erases slot 3 and a directory, and commits");
    g_nmnt = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "remount");
    CHECK(h_fs_OpenFile(&(uint64_t){0}, "save:\\savedata3.bin", MODE_READ) == RES_PATH_NOT_FOUND && h_fs_GetEntryType(&(int32_t){0}, "save:\\dir") == RES_PATH_NOT_FOUND, "what the game erased stays erased");
    CHECK(same(save, "\\savedata3.bin", "slot3"), "while the old entry is still there");

    g_nmnt = 0;   // a copy the disk refuses (full): the mount still works, the old entry is not marked, and the next mount copies it
    char save2[1100], work2[1200], old[1300]; snprintf(save2, sizeof save2, "%s/save2", root); snprintf(work2, sizeof work2, "%s.work", save2); mkdir(save2, 0755);
    put(save2, "\\savedata0.bin", "slot0"); snprintf(old, sizeof old, "%s/\\savedata0.bin", work2);
    setenv("DM_SAVE", save2, 1); g_clone_backslash_fail = 1;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && h_fs_OpenFile(&(uint64_t){0}, "save:/savedata0.bin", MODE_READ) == RES_PATH_NOT_FOUND, "the copy fails, the mount does not");
    CHECK(there(work2, "\\savedata0.bin") && getxattr(old, FS_BACKSLASH_COPIED, NULL, 0, 0, XATTR_NOFOLLOW) < 0, "the old entry is not marked");
    g_nmnt = 0; g_clone_backslash_fail = 0;
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && content("save:/savedata0.bin", "slot0"), "the next mount copies it");
    CHECK(getxattr(old, FS_BACKSLASH_COPIED, NULL, 0, 0, XATTR_NOFOLLOW) >= 0, "and marks it");

    rm_tree(root);
    printf("== backslash: %d checks, %d failed ==\n", checks, fails); return fails != 0;
}
