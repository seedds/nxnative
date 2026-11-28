// Exercise actual account selection and fs routing together, across fresh execs.
// Only synthetic files; the same filename in two accounts must never alias.
#include <sys/types.h>
static ssize_t account_pwrite(int fd, const void *buf, size_t n, off_t off);
#define pwrite account_pwrite
#include "../hle_fs.c"
#undef pwrite
#include "../hle_account.c"
#include <assert.h>
#include <sys/wait.h>
ssize_t pwrite(int, const void *, size_t, off_t);
static int fail_write, selected;
static ssize_t account_pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (fail_write) { errno = ENOSPC; return -1; }
    return pwrite(fd, buf, n, off);
}
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void dm_prof_add(int id, uint64_t ns, uint64_t b) { (void)id; (void)ns; (void)b; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
void hle_signal_system_event(void *g) { (void)g; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
void dm_thread_native_wait(int entering) { (void)entering; }
int front_user_select(const front_user *users, int n, int create, int skip) {
    (void)users; (void)n; (void)skip;
    assert(selected != FRONT_USER_CREATE || create); return selected;
}
static Result write_user(const char *path, const char *s) {
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    r = h_fs_SetFileSize(h, (int64_t)strlen(s));
    if (!r) r = h_fs_WriteFile(h, 0, s, strlen(s), NULL);
    h_fs_CloseFile(h); return r;
}
static void expect_user(const char *path, const char *s) {
    uint64_t h; size_t got = 0; char out[64] = "";
    assert(h_fs_OpenFile(&h, path, MODE_READ) == RES_OK);
    assert(h_fs_ReadFile(&got, h, 0, out, sizeof out - 1) == RES_OK); h_fs_CloseFile(h);
    assert(got == strlen(s) && !strcmp(out, s));
}
static void child(const char *exe, const char *mode, const char *save) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { execl(exe, exe, mode, save, NULL); _exit(127); }
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(int argc, char **argv) {
    if (argc != 3) {
        char root[] = "/private/tmp/t_account_save.XXXXXX", save[1024]; assert(mkdtemp(root));
        snprintf(save, sizeof save, "%s/save", root);
        child(argv[0], "--seed", save); child(argv[0], "--restart", save); child(argv[0], "--restart", save);
        assert(!rm_tree(root)); puts("t_account_save: OK (independent commits, failure isolation, two fresh boots)"); return 0;
    }
    setenv("DM_SAVE", argv[2], 1); unsetenv("DM_SAVE_SYNC"); unsetenv("DM_TEST_FS_FAIL"); unsetenv("DM_TEST_FS_FAILAT");
    Uid u2 = user_uid(1), picked, list[8]; uint8_t settings[0x88] = {0}, exists; int n;
    if (!strcmp(argv[1], "--seed")) {
        selected = FRONT_USER_CREATE;
        assert(h_acc_ShowSel(&picked, settings) == RES_OK && !memcmp(&picked, &u2, sizeof u2));
    }
    assert(h_acc_Exists(&exists, &u2) == RES_OK && exists); // no selector/recreation required on later boots
    assert(h_acc_ListAll(&n, list, 8) == RES_OK && n == 2);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    // The player can choose user 2 even with no controller-count input to the host.
    selected = 1; assert(h_acc_ShowSel(&picked, settings) == RES_OK && !memcmp(&picked, &u2, sizeof u2));
    UserHandle h1, h2;
    assert(h_acc_OpenPre(&h1) == RES_OK && h_acc_OpenUser(&h2, &u2) == RES_OK);
    assert(h_fs_MountSave("p1", &k_uid) == RES_OK && h_fs_MountSave("p2", &u2) == RES_OK);
    assert(h_fs_MountSave("alias2", &u2) == RES_OK);
    assert(h_fs_MountSave("p1", &u2) != RES_OK); // no success while pointing at another user's save
    assert(h_fs_MountSave("p1", &k_uid) == RES_OK);
    assert(g_mnt[0].txn_root != g_mnt[1].txn_root && g_mnt[1].txn_root == g_mnt[2].txn_root);
    Uid unknown = user_uid(7);
    assert(!h_fs_SaveExisting(DM_GAME_PROGRAM_ID, &unknown) && !h_fs_SaveExists(&unknown));
    assert(h_fs_MountSave("unknown", &unknown) != RES_OK && h_fs_MountSaveRO("unknown", DM_GAME_PROGRAM_ID, &unknown) != RES_OK);
    assert(h_fs_SaveExisting(DM_GAME_PROGRAM_ID, &u2) && h_fs_SaveExists(&u2) && h_fs_SaveExists(&k_uid));
    if (!strcmp(argv[1], "--seed")) {
        assert(h_fs_CreateFile("p1:/hero", 0) == RES_OK && h_fs_CreateFile("p2:/hero", 0) == RES_OK);
        assert(write_user("p1:/hero", "first-A") == RES_OK && write_user("p2:/hero", "first-B") == RES_OK);
        assert(h_fs_Commit("p1") == RES_OK && h_fs_Commit("p2") == RES_OK);
        assert(write_user("p1:/hero", "next-A") == RES_OK && write_user("p2:/hero", "next-B") == RES_OK);
        assert(h_fs_Commit("p1") == RES_OK);
        assert(h_fs_MountSaveRO("ro2", DM_GAME_PROGRAM_ID, &u2) == RES_OK); expect_user("ro2:/hero", "first-B");
        assert(h_fs_Commit("alias2") == RES_OK); expect_user("ro2:/hero", "next-B");
        // Warm negative lookup on ro2, then create/rename/delete + commit through the writable alias.
        uint64_t tmp_h = 0; int32_t tmp_t = -1;
        assert(h_fs_OpenFile(&tmp_h, "ro2:/fresh", MODE_READ) == RES_PATH_NOT_FOUND);
        assert(h_fs_GetEntryType(&tmp_t, "ro2:/fresh") == RES_PATH_NOT_FOUND);
        assert(h_fs_CreateFile("p2:/fresh", 0) == RES_OK && write_user("p2:/fresh", "born-B") == RES_OK);
        assert(h_fs_Commit("alias2") == RES_OK);
        expect_user("ro2:/fresh", "born-B");
        assert(h_fs_OpenFile(&tmp_h, "p1:/fresh", MODE_READ) == RES_PATH_NOT_FOUND);
        assert(h_fs_RenameFile("p2:/fresh", "p2:/renamed") == RES_OK && h_fs_Commit("p2") == RES_OK);
        assert(h_fs_OpenFile(&tmp_h, "ro2:/fresh", MODE_READ) == RES_PATH_NOT_FOUND);
        expect_user("ro2:/renamed", "born-B");
        assert(h_fs_DeleteFile("p2:/renamed") == RES_OK && h_fs_Commit("p2") == RES_OK);
        assert(h_fs_OpenFile(&tmp_h, "ro2:/renamed", MODE_READ) == RES_PATH_NOT_FOUND);
        assert(h_fs_CreateFile("ro2:/nope", 0) != RES_OK);
        assert(h_fs_OpenFile(&tmp_h, "ro2:/hero", MODE_WRITE) != RES_OK);
        assert(h_fs_DeleteFile("ro2:/hero") != RES_OK);
        // Immutable RomFS lookup keeps using dir_index while rejecting wrong case.
        char romdir[1024], romfile[1100];
        snprintf(romdir, sizeof romdir, "%s.rom", argv[2]);
        assert(!mkdir(romdir, 0755));
        snprintf(romfile, sizeof romfile, "%s/Data", romdir);
        FILE *rf = fopen(romfile, "w"); assert(rf); fputs("rom-bytes", rf); fclose(rf);
        assert(mount("romtest", romdir, 1) == RES_OK);
        expect_user("romtest:/Data", "rom-bytes");
        assert(h_fs_OpenFile(&tmp_h, "romtest:/data", MODE_READ) == RES_PATH_NOT_FOUND);
        snprintf(romfile, sizeof romfile, "%s/Late", romdir);
        rf = fopen(romfile, "w"); assert(rf); fputs("late", rf); fclose(rf);
        assert(h_fs_OpenFile(&tmp_h, "romtest:/Late", MODE_READ) == RES_PATH_NOT_FOUND);
        fail_write = 1; assert(write_user("p1:/hero", "bad-A") != RES_OK); fail_write = 0;
        assert(write_user("p2:/hero", "good-B") == RES_OK && h_fs_Commit("p2") == RES_OK);
        assert(h_fs_Commit("p1") != RES_OK); expect_user("p1:/hero", "next-A"); expect_user("p2:/hero", "good-B");
        // Reverse the failure and commit order to catch accidental global poison.
        fail_write = 1; assert(write_user("alias2:/hero", "bad-B") != RES_OK); fail_write = 0;
        assert(write_user("p1:/hero", "good-A") == RES_OK && h_fs_Commit("p1") == RES_OK);
        assert(h_fs_Commit("p2") != RES_OK); expect_user("p1:/hero", "good-A"); expect_user("p2:/hero", "good-B");
    } else {
        expect_user("p1:/hero", "good-A"); expect_user("alias2:/hero", "good-B");
        assert(h_fs_MountSaveRO("ro1", DM_GAME_PROGRAM_ID, &k_uid) == RES_OK);
        assert(h_fs_MountSaveRO("ro2", DM_GAME_PROGRAM_ID, &u2) == RES_OK);
        expect_user("ro1:/hero", "good-A"); expect_user("ro2:/hero", "good-B");
    }
    assert(write_user("p1:/hero", "pending-A") == RES_OK && write_user("p2:/hero", "pending-B") == RES_OK);
    h_acc_CloseUser(&h1); h_acc_CloseUser(&h2);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    return 0; // no commit: the next fresh process must see both older versions
}
