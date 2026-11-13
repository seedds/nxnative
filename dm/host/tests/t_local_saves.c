// Exercise the guest save APIs on real APFS transactions. Each child is a fresh host launch.
#define dm_game_imports test_game_imports
#include "../hle_fs.c"
#undef dm_game_imports
#include <limits.h>
#include <sys/wait.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
static int mixed_title = 1;
int test_game_imports(const char *prefix) {
    return mixed_title && !strcmp(prefix, "_ZN2nn2fs13MountSaveData");
}
#define ALICE "53574e415449564578626c0000000001"
#define BOB   "1234567890abcdef0000000000000002"
static const uint64_t alice[2] = {0x78626c0000000001, 0x53574e4154495645};
static const uint64_t bob[2] = {2, 0x1234567890abcdef};
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static int write_guest(const char *path, const char *value) {
    Result r = h_fs_CreateFile(path, 0);
    if (r && r != RES_PATH_EXISTS) return 0;
    uint64_t h = UINT64_MAX; size_t len = strlen(value);
    if (h_fs_OpenFile(&h, path, MODE_WRITE) || h == UINT64_MAX) return 0;
    r = h_fs_SetFileSize(h, (int64_t)len);
    if (!r) r = h_fs_WriteFile(h, 0, value, len, NULL);
    h_fs_CloseFile(h);
    return !r;
}
static int guest_has(const char *path, const char *value) {
    uint64_t h = UINT64_MAX; char buf[64]; size_t got = 0;
    if (h_fs_OpenFile(&h, path, MODE_READ) || h == UINT64_MAX) return 0;
    Result r = h_fs_ReadFile(&got, h, 0, buf, sizeof buf);
    h_fs_CloseFile(h);
    return !r && got == strlen(value) && !memcmp(buf, value, got);
}
static int file_has(const char *path, const char *value) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[64]; size_t got = fread(buf, 1, sizeof buf, f); fclose(f);
    return got == strlen(value) && !memcmp(buf, value, got);
}
static int put(const char *path, const char *value) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fputs(value, f) >= 0;
    return !fclose(f) && ok;
}

static int unknown_users(void) {
    const uint64_t unknown[2] = {99, 0};
    const void *invalid[] = {NULL, unknown};
    for (size_t i = 0; i < 2; i++) {
        const void *uid = invalid[i];
        CHECK(h_fs_MountSave("invalid", uid) != RES_OK);
        CHECK(!h_fs_IsMounted("invalid") && access("save", F_OK));
        CHECK(h_fs_EnsureSave(uid) != RES_OK && !h_fs_SaveExists(uid));
        CHECK(!h_fs_SaveExisting(DM_GAME_PROGRAM_ID, uid));
        CHECK(h_fs_MountSaveRO("ro", DM_GAME_PROGRAM_ID, uid) != RES_OK);
        int64_t bytes = -1, journal = -1;
        CHECK(h_fs_SaveSize(&bytes, &journal, uid) != RES_OK && bytes == -1 && journal == -1);
        CHECK(h_fs_ExtendSave(uid, 1, 1) != RES_OK);
    }
    char too_long[1100]; memset(too_long, 'a', sizeof too_long - 1); too_long[sizeof too_long - 1] = 0;
    setenv("DM_SAVE", too_long, 1);
    CHECK(h_fs_MountSave("alice", alice) != RES_OK && h_fs_MountSave("bob", bob) != RES_OK);
    CHECK(h_fs_MountDeviceSave("dev") != RES_OK && !h_fs_IsMounted("dev"));
    // The base fits but the UID or transaction suffix would be truncated.
    too_long[1019] = 0;
    setenv("DM_SAVE", too_long, 1);
    CHECK(h_fs_MountSave("alice", alice) != RES_OK && h_fs_MountSave("bob", bob) != RES_OK);
    setenv("DM_SAVE", "save", 1);
    CHECK(!symlink("save", "save.user-" BOB));
    CHECK(h_fs_MountSave("alias-to-legacy", bob) != RES_OK && !h_fs_IsMounted("alias-to-legacy"));
    CHECK(!unlink("save.user-" BOB));
    return 0;
}
static int ensure_accounts(void) {
    setenv("DM_SAVE", "existing", 1); setenv("DM_SAVE_SYNC", "query-cloud", 1);
    CHECK(!mkdir("existing", 0755) && put("existing/slot", "alice"));
    CHECK(h_fs_SaveExists(alice) && h_fs_SaveExisting(DM_GAME_PROGRAM_ID, alice));
    CHECK(!h_fs_SaveExists(bob) && !h_fs_SaveExisting(DM_GAME_PROGRAM_ID, bob));
    CHECK(access("existing.user-" BOB, F_OK) && h_fs_EnsureSave(bob) == RES_OK);
    CHECK(h_fs_SaveExists(bob) && h_fs_SaveExisting(DM_GAME_PROGRAM_ID, bob) && h_fs_EnsureSave(bob) == RES_OK);
    CHECK(!rmdir("existing.user-" BOB) && file_has("existing/slot", "alice"));
    CHECK(!symlink("existing", "existing.user-" BOB));
    CHECK(!h_fs_SaveExists(bob) && h_fs_EnsureSave(bob) != RES_OK && file_has("existing/slot", "alice"));
    CHECK(access("query-cloud", F_OK) && access("existing.work", F_OK) && access("existing.sync", F_OK));
    return 0;
}
static int overlong_exit_root(void) {
    // The old launcher truncated this to the existing "save/" prefix and published Alice's slot.
    char path[PATH_MAX + 16];
    memcpy(path, "save", 4);
    memset(path + 4, '/', PATH_MAX - 4);
    strcpy(path + PATH_MAX, "missing");
    setenv("DM_SAVE_SYNC", "truncated-cloud", 1);
    int synced = save_sync_after_exit(path, DM_GAME_ID);
    CHECK(access("truncated-cloud/" DM_GAME_ID "/save.dmsync", F_OK) && access("save.sync", F_OK));
    CHECK(!synced);
    return 0;
}
static int local_only(void) {
    setenv("DM_SAVE_SYNC", "cloud", 1);
    CHECK(h_fs_MountSave("save", bob) == RES_OK && write_guest("save:/slot", "bob"));
    CHECK(h_fs_Commit("save") == RES_OK && save_sync_flush(10000));
    CHECK(file_has("save.user-" BOB "/slot", "bob") && file_has("save/slot", "legacy"));
    CHECK(access("cloud/" DM_GAME_ID "/save.dmsync", F_OK) && access("save.sync", F_OK));
    return 0;
}
static int cloud_commits(void) {
    // The sender is already running for the legacy save. A secondary commit must not wake it.
    setenv("DM_SAVE", "wake", 1); setenv("DM_SAVE_SYNC", "wake-cloud", 1);
    CHECK(h_fs_MountSave("first", alice) == RES_OK && h_fs_MountSave("second", bob) == RES_OK);
    CHECK(put("wake/unsent", "legacy") && write_guest("second:/slot", "private"));
    CHECK(h_fs_Commit("second") == RES_OK && save_sync_flush(10000));
    CHECK(access("wake-cloud/" DM_GAME_ID "/save.dmsync", F_OK) && access("wake.sync", F_OK));
    CHECK(write_guest("first:/slot", "shared") && h_fs_Commit("first") == RES_OK && save_sync_flush(10000));
    ss_meta packed;
    CHECK(!mkdir("live-peek", 0755) && !ss_read("wake-cloud/" DM_GAME_ID "/save.dmsync", "live-peek", &packed));
    CHECK(packed.files == 1 && file_has("live-peek/save/slot", "shared"));
    return 0;
}
static int simultaneous(void) {
    CHECK(h_fs_MountSave("player", alice) == RES_OK && guest_has("player:/slot", "legacy"));
    CHECK(h_fs_MountSave("other", bob) == RES_OK && guest_has("other:/slot", "bob"));
    CHECK(h_fs_MountSave("player", bob) != RES_OK && guest_has("player:/slot", "legacy"));
    CHECK(h_fs_EnsureSave(bob) == RES_OK && h_fs_SaveExists(alice) && h_fs_SaveExisting(DM_GAME_PROGRAM_ID, bob));
    CHECK(write_guest("player:/slot", "alice") && write_guest("other:/slot", "bob2"));
    CHECK(h_fs_MountSave("alias", alice) == RES_OK && guest_has("alias:/slot", "alice"));
    const char *both[] = {"player", "other"};
    CHECK(h_fs_CommitList(both, 2) != RES_OK && file_has("save/slot", "legacy"));
    CHECK(h_fs_Commit("other") == RES_OK && file_has("save.user-" BOB "/slot", "bob2"));
    CHECK(file_has("save/slot", "legacy") && h_fs_Commit("alias") == RES_OK && file_has("save/slot", "alice"));
    CHECK(h_fs_RenameFile("other:/slot", "player:/moved") != RES_OK && guest_has("other:/slot", "bob2"));
    CHECK(h_fs_MountDeviceSave("common") == RES_OK && write_guest("common:/slot", "device"));
    CHECK(h_fs_Commit("common") == RES_OK && file_has("save_device/slot", "device"));
    int64_t bytes, journal;
    CHECK(h_fs_DeviceSaveSize(&bytes, &journal) == RES_OK && bytes > 0 && journal > 0);
    CHECK(h_fs_ExtendDeviceSave(bytes, journal) == RES_OK);
    h_fs_Unmount("player");
    CHECK(!h_fs_IsMounted("player") && h_fs_MountSave("player", bob) == RES_OK);
    CHECK(guest_has("player:/slot", "bob2") && guest_has("alias:/slot", "alice"));
    // Unmount one alias with open I/O, then bind its old name to the other user.
    uint64_t h;
    CHECK(h_fs_OpenFile(&h, "other:/slot", MODE_WRITE) == RES_OK);
    h_fs_Unmount("other");
    CHECK(h_fs_MountSave("other", alice) == RES_OK && h_fs_WriteFile(h, 0, "hold", 4, NULL) == RES_OK);
    h_fs_CloseFile(h);
    CHECK(guest_has("player:/slot", "hold") && guest_has("other:/slot", "alice"));
    h_fs_Unmount("player");
    CHECK(h_fs_MountSave("player", bob) == RES_OK && guest_has("player:/slot", "bob2"));
    return 0;
}
static int account_read_only(void) {
    CHECK(h_fs_MountSaveRO("alice-ro", DM_GAME_PROGRAM_ID, alice) == RES_OK);
    CHECK(h_fs_MountSaveRO("bob-ro", DM_GAME_PROGRAM_ID, bob) == RES_OK);
    CHECK(h_fs_MountSaveRO("alice-ro", DM_GAME_PROGRAM_ID, bob) == RES_PATH_EXISTS);
    uint64_t old;
    CHECK(h_fs_OpenFile(&old, "alice-ro:/slot", MODE_READ) == RES_OK);
    h_fs_Unmount("alice-ro");
    CHECK(!h_fs_IsMounted("alice-ro"));
    CHECK(h_fs_MountSaveRO("alice-ro", DM_GAME_PROGRAM_ID, bob) == RES_OK && guest_has("alice-ro:/slot", "bob2"));
    char value[6] = {0}; size_t read = 0;
    CHECK(h_fs_ReadFile(&read, old, 0, value, 5) == RES_OK && read == 5 && !strcmp(value, "alice"));
    h_fs_CloseFile(old);
    h_fs_Unmount("alice-ro");
    CHECK(h_fs_MountSaveRO("alice-ro", DM_GAME_PROGRAM_ID, alice) == RES_OK);
    CHECK(guest_has("alice-ro:/slot", "alice") && guest_has("bob-ro:/slot", "bob2"));
    CHECK(!write_guest("alice-ro:/slot", "damage") && !write_guest("bob-ro:/slot", "damage"));
    CHECK(h_fs_MountSaveRO("other-app", DM_GAME_PROGRAM_ID + 1, alice) == RES_TARGET_NOT_FOUND);
    CHECK(!h_fs_IsMounted("other-app") && file_has("save/slot", "alice") && file_has("save.user-" BOB "/slot", "bob2"));
    return 0;
}
static int read_only_fallback(void) {
    setenv("DM_TEST_FS_FAILAT", "mountclone:1", 1);
    CHECK(h_fs_MountSave("slot", bob) == RES_OK && guest_has("slot:/slot", "bob2"));
    CHECK(!write_guest("slot:/slot", "damage") && h_fs_MountSave("slot", alice) != RES_OK);
    CHECK(guest_has("slot:/slot", "bob2") && file_has("save/slot", "alice"));
    h_fs_Unmount("slot");
    CHECK(h_fs_MountSave("slot", alice) == RES_OK && guest_has("slot:/slot", "alice"));
    CHECK(h_fs_MountSave("slot", bob) != RES_OK && guest_has("slot:/slot", "alice"));
    return 0;
}
static int reopen(void) {
    setenv("DM_USER_ID", ALICE, 1);
    CHECK(h_fs_MountSave("save", bob) == RES_OK && guest_has("save:/slot", "bob2"));
    CHECK(h_fs_MountSave("other", alice) == RES_OK && guest_has("other:/slot", "alice"));
    CHECK(h_fs_MountDeviceSave("common") == RES_OK && guest_has("common:/slot", "device"));
    return 0;
}
static int device_only(void) {
    mixed_title = 0;
    CHECK(h_fs_MountDeviceSave("island") == RES_OK && guest_has("island:/slot", "alice"));
    return 0;
}
extern void *(*t_guest_sym)(const char *);
static int sdk_cache_mounted, sdk_queries, sdk_unmounts, sdk_host_lock_free = 1;
static int sdk_is_mounted(const char *name) {
    int locked = pthread_mutex_trylock(&g_path_lock);
    if (locked) sdk_host_lock_free = 0;
    else pthread_mutex_unlock(&g_path_lock);
    sdk_queries++;
    return !strcmp(name, "cache") && sdk_cache_mounted;
}
static void sdk_unmount(const char *name) {
    if (strcmp(name, "cache") || !sdk_cache_mounted) abort();
    int locked = pthread_mutex_trylock(&g_path_lock);
    if (locked) sdk_host_lock_free = 0;
    else pthread_mutex_unlock(&g_path_lock);
    sdk_unmounts++; sdk_cache_mounted = 0;
}
static void *sdk_symbol(const char *name) {
    if (!strcmp(name, "_ZN2nn2fs9IsMountedEPKc")) return (void *)sdk_is_mounted;
    if (!strcmp(name, "_ZN2nn2fs7UnmountEPKc")) return (void *)sdk_unmount;
    return NULL;
}
static int sdk_mounts_with_users(void) {
    CHECK(dm_users()->configured && h_fs_MountSave("user-save", bob) == RES_OK);
    CHECK(mount("host-temp", "host-temp", 0) == RES_OK);
    t_guest_sym = sdk_symbol; sdk_cache_mounted = 1;
    h_fs_Unmount("user-save");
    CHECK(!sdk_queries && !sdk_unmounts); // the host unmount itself stays local
    CHECK(!h_fs_IsMounted("user-save") && sdk_queries == 1 && !sdk_unmounts); // main also queries the SDK for an unowned name
    h_fs_Unmount("host-temp");
    CHECK(h_fs_IsMounted("host-temp") && sdk_queries == 1 && !sdk_unmounts);
    h_fs_Unmount("cache");
    CHECK(sdk_queries == 2 && sdk_unmounts == 1 && !sdk_cache_mounted && sdk_host_lock_free);
    h_fs_Unmount("cache"); h_fs_Unmount(NULL); h_fs_Unmount("");
    CHECK(sdk_queries == 3 && sdk_unmounts == 1 && sdk_host_lock_free);
    t_guest_sym = NULL;
    return 0;
}
static int legacy(void) {
    unsetenv("DM_USERS_FILE"); unsetenv("DM_USER_ID");
    CHECK(h_fs_MountSave("save", NULL) == RES_OK && guest_has("save:/slot", "alice"));
    h_fs_Unmount("save");
    CHECK(h_fs_IsMounted("save")); // the singleton host historically keeps its mount table for the session
    return 0;
}
static int session(int (*body)(void)) {
    pid_t child = fork();
    if (child == 0) _exit(body());
    int status;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
}
static int bob_only_game(void) {
    CHECK(h_fs_MountSave("save", bob) == RES_OK && write_guest("save:/slot", "bob-private"));
    CHECK(h_fs_Commit("save") == RES_OK);
    return 0;
}
static int legacy_game(void) {
    CHECK(h_fs_MountSave("save", alice) == RES_OK && guest_has("save:/slot", "new-cloud"));
    CHECK(write_guest("save:/slot", "new-local") && h_fs_Commit("save") == RES_OK);
    return 0; // session exits immediately, before the sender's quiet period ends
}
static int launcher_session(int (*game)(void)) {
    int channel[2];
    save_sync_launch_prepare(channel);
    if (channel[0] < 0 || channel[1] < 0) return 0;
    int ok = session(game);
    close(channel[1]);
    return save_sync_launch_finish(channel[0], getenv("DM_SAVE"), DM_GAME_ID) && ok;
}
static int launcher_recovery(void) {
    const char *bundle = "late-cloud/" DM_GAME_ID "/save.dmsync";
    ss_meta packed;
    CHECK(!mkdir("stale", 0755) && put("stale/slot", "old-local"));
    struct timespec past[2] = {{1000000000, 0}, {1000000000, 0}};
    CHECK(!utimensat(AT_FDCWD, "stale/slot", past, 0));
    CHECK(!mkdir("remote", 0755) && put("remote/slot", "new-cloud"));
    CHECK(!mkdir("late-cloud", 0755) && !mkdir("late-cloud/" DM_GAME_ID, 0755));
    CHECK(!ss_pack("remote", DM_GAME_ID, bundle, &packed));
    setenv("DM_SAVE_SYNC", "late-cloud", 1); setenv("DM_SAVE", "stale", 1);
    CHECK(launcher_session(bob_only_game));
    CHECK(!mkdir("bob-exit", 0755) && !ss_read(bundle, "bob-exit", &packed));
    CHECK(file_has("bob-exit/save/slot", "new-cloud") && file_has("stale/slot", "old-local") && access("stale.sync", F_OK));
    // Bob is still selected. The guest explicitly mounts Alice, sees the newer cloud save, then commits.
    CHECK(launcher_session(legacy_game));
    CHECK(!mkdir("alice-exit", 0755) && !ss_read(bundle, "alice-exit", &packed));
    CHECK(packed.files == 1 && file_has("alice-exit/save/slot", "new-local"));
    return 0;
}
int main(void) {
    char root[1024];
    snprintf(root, sizeof root, "%s/local-saves.XXXXXX", getenv("TMPDIR") ?: "/private/tmp");
    CHECK(mkdtemp(root) && !chdir(root));
    CHECK(put("users.json", "{\"version\":1,\"users\":[{\"id\":\"" ALICE "\",\"nickname\":\"Alice\"},{\"id\":\"" BOB "\",\"nickname\":\"Bob\"}]}"));
    setenv("DM_USERS_FILE", "users.json", 1); setenv("DM_USER_ID", BOB, 1);
    setenv("DM_SAVE", "save///", 1); unsetenv("DM_SAVE_SYNC");
    CHECK(session(unknown_users));
    CHECK(session(ensure_accounts));
    CHECK(!mkdir("save", 0755) && put("save/slot", "legacy"));
    CHECK(session(overlong_exit_root));
    CHECK(session(local_only));
    CHECK(session(cloud_commits));
    CHECK(session(simultaneous));
    CHECK(session(account_read_only));
    CHECK(session(read_only_fallback));
    CHECK(session(reopen));
    CHECK(session(device_only));
    CHECK(session(sdk_mounts_with_users));
    CHECK(session(legacy));
    CHECK(session(launcher_recovery));
    // The Mac launcher checks for an unsent save after the host exits.
    setenv("DM_SAVE_SYNC", "cloud", 1);
    CHECK(save_sync_after_exit("save///", DM_GAME_ID));
    ss_meta packed;
    CHECK(!mkdir("peek", 0755) && !ss_read("cloud/" DM_GAME_ID "/save.dmsync", "peek", &packed));
    CHECK(packed.files == 2 && file_has("peek/save/slot", "alice") && file_has("peek/save_device/slot", "device"));
    CHECK(!chdir("/") && !rm_tree(root));
    puts("local saves passed");
    return 0;
}
