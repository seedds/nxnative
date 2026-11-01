// Only synthetic signing keys. This fixture neither reads the user's firmware
// nor creates an online identity.
#include "../hle.h"
#undef HLE_FORCE
#undef HLE_FORCE_WAIT
#define HLE_FORCE(name, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#define HLE_FORCE_WAIT HLE_FORCE
#include "../hle_friends.c"
#include <CommonCrypto/CommonHMAC.h>
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(test) do { ++checks; if (!(test)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #test); abort(); } } while (0)
static const uint8_t local_uid[16] = {1};
static _Thread_local int wait_depth;
static _Atomic unsigned begins, ends;
void dm_guest_host_wait_begin(void) { ++wait_depth; atomic_fetch_add(&begins, 1); }
void dm_guest_host_wait_end(void) { assert(wait_depth > 0); --wait_depth; atomic_fetch_add(&ends, 1); }
void hle_bind_system_event(void *p, int clear) { (void)p; (void)clear; }
void hle_signal_system_event(void *p) { (void)p; }
int dm_account_uid_exists(const void *uid) { return uid && !memcmp(uid, local_uid, sizeof local_uid); }
static bool unavailable(uint8_t keys[8][32]) { (void)keys; return false; }
static bool synthetic_keys(uint8_t keys[8][32]) {
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 32; ++j) keys[i][j] = (uint8_t)(32*i+j);
    return true;
}
static void fixed_random(void *p, unsigned size) { memset(p, 0x22, size); }
static int all_zero(const uint8_t *p, unsigned n) { uint8_t any = 0; for (unsigned i = 0; i < n; ++i) any |= p[i]; return !any; }
static int valid_key(const uint8_t key[64], unsigned flag) {
    if (key[0] != 1 || key[1] != 1 || key[2] > 7 || key[3] || key[4] != (flag&1) || !all_zero(key+5, 11) ||
        (key[22]&0xf0) != 0x40 || (key[24]&0xc0) != 0x80) return 0;
    uint8_t keys[8][32], expected[32]; synthetic_keys(keys);
    CCHmac(kCCHmacAlgSHA256, keys[key[2]], 32, key, 32, expected);
    return !memcmp(expected, key+32, 32);
}
static void write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0600); assert(fd >= 0); assert(write(fd, data, size) == (ssize_t)size); assert(!close(fd));
}
static unsigned fd_count(void) { unsigned n = 0; for (int fd = 0; fd < 512; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++n; return n; }
static int cleanup(const char *path, const struct stat *st, int type, struct FTW *tree) { (void)st; (void)type; (void)tree; return remove(path); }
static void *parallel_keys(void *opaque) {
    const uint8_t *uuid = opaque; uint8_t key[64];
    for (unsigned i = 0; i < 80; ++i) {
        assert(dm_friends_history_key(key, local_uid, i) == 0 && valid_key(key, i) && !memcmp(key+16, uuid, 16));
        assert(wait_depth == 0);
    }
    return NULL;
}
int main(void) {
    char root[] = "/tmp/nx-friends-history-XXXXXX"; CHECK(mkdtemp(root)); CHECK(!setenv("DM_ROOT", root, 1));
    CHECK(!unsetenv("DM_SYSTEM_DATA_DIR"));
    uint8_t output[66], zero_uid[16] = {0}, other_uid[16] = {2};
    memset(output, 0x6a, sizeof output);
    CHECK(h_fr_GetPHKey(NULL, local_uid, 1) == DM_FRIENDS_HISTORY_FAIL);
    CHECK(h_fr_GetPHKey(output+1, zero_uid, 1) == DM_FRIENDS_HISTORY_INVALID_UID && all_zero(output+1, 64));
    CHECK(h_fr_GetPHKey(output+1, other_uid, 1) == DM_FRIENDS_HISTORY_INVALID_UID && all_zero(output+1, 64));
    CHECK(h_fr_GetPHKey(output+1, NULL, 1) == DM_FRIENDS_HISTORY_INVALID_UID);
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0 && all_zero(output+1, 64));
    CHECK(output[0] == 0x6a && output[65] == 0x6a && !wait_depth);
    dm_friends_history_set_key_provider(unavailable);
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0 && all_zero(output+1, 64));
    dm_friends_history_set_key_provider(synthetic_keys); dm_friends_history_set_random_provider(fixed_random);
    CHECK(h_fr_GetPHKey(output+1, local_uid, 0xffffffffu) == 0);
    // Independently calculated with Python's hmac/hashlib over this specified
    // synthetic body/key. It catches packing, source-key selection and coverage.
    static const uint8_t known[64] = {
        1,1,2,0,1,0,0,0,0,0,0,0,0,0,0,0,0x22,0x22,0x22,0x22,0x22,0x22,0x42,0x22,0xa2,0x22,0x22,0x22,0x22,0x22,0x22,0x22,
        0xc6,0x2e,0x99,0x42,0x02,0xfb,0x56,0x65,0xc0,0x60,0x01,0x61,0x95,0x47,0xec,0x13,
        0x0d,0x8b,0x73,0x6c,0x55,0x79,0xc4,0xbb,0x27,0xe5,0x4f,0xa1,0x0a,0x14,0x6c,0x6b};
    CHECK(!memcmp(output+1, known, 64) && output[0] == 0x6a && output[65] == 0x6a);
    uint8_t uuid[16]; memcpy(uuid, output+17, 16);
    char directory[PATH_MAX], path[PATH_MAX], bad[PATH_MAX];
    snprintf(directory, sizeof directory, "%s/friends-history", root);
    snprintf(path, sizeof path, "%s/01000000000000000000000000000000.uuid.bin", directory);
    struct stat st; CHECK(!stat(directory, &st) && (st.st_mode&0777) == 0700);
    CHECK(!stat(path, &st) && (st.st_mode&0777) == 0600 && st.st_size == 16);
    CHECK(h_fr_GetPHKey(output+1, local_uid, 2) == 0 && valid_key(output+1, 0) && !memcmp(uuid, output+17, 16));
    CHECK(memcmp(output+33, known+32, 32));
    dm_friends_history_set_random_provider(NULL);
    CHECK(dm_friends_history_key(output+1, other_uid, 1) == 0 && valid_key(output+1, 1) && memcmp(uuid, output+17, 16));
    unsigned before = fd_count(); pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) CHECK(!pthread_create(threads+i, NULL, parallel_keys, uuid));
    for (unsigned i = 0; i < 8; ++i) CHECK(!pthread_join(threads[i], NULL));
    CHECK(before == fd_count() && atomic_load(&begins) == atomic_load(&ends));
    // Separate processes race the first creation. The persisted winner must be
    // atomic and stable for every successful call, rather than overwriting UUIDs.
    int pipefd[2]; CHECK(!pipe(pipefd)); pid_t children[8];
    for (unsigned i = 0; i < 8; ++i) {
        children[i] = fork(); CHECK(children[i] >= 0);
        if (!children[i]) {
            close(pipefd[0]); uint8_t key[64], uid[16] = {3};
            if (dm_friends_history_key(key, uid, 1) || !valid_key(key, 1) || write(pipefd[1], key, 64) != 64) _exit(1);
            close(pipefd[1]); _exit(0);
        }
    }
    close(pipefd[1]); uint8_t results[8][64]; const size_t capacity = sizeof results; size_t have = 0;
    while (have < capacity) { ssize_t n = read(pipefd[0], (char *)results+have, capacity-have); assert(n > 0); have += (size_t)n; }
    close(pipefd[0]);
    for (unsigned i = 0; i < 8; ++i) { int status; CHECK(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) && !WEXITSTATUS(status)); CHECK(!memcmp(results[0]+16, results[i]+16, 16)); }
    CHECK(!chmod(path, 0644)); CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0 && all_zero(output+1, 64)); CHECK(!chmod(path, 0600));
    write_file(path, uuid, 15); CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0); CHECK(!stat(path, &st) && st.st_size == 15);
    uint8_t broken[16] = {0}; write_file(path, broken, 16); CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0);
    write_file(path, uuid, 16);
    snprintf(bad, sizeof bad, "%s/uuid-target", root); CHECK(!rename(path, bad)); CHECK(!symlink(bad, path));
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0 && all_zero(output+1, 64)); CHECK(!unlink(path)); CHECK(!rename(bad, path));
    CHECK(!chmod(directory, 0755)); CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0); CHECK(!chmod(directory, 0700));
    // The default provider uses only the exact private firmware archive path.
    char archive[PATH_MAX], files[PATH_MAX], keypath[PATH_MAX];
    snprintf(archive, sizeof archive, "%s/010000000000000e", root); CHECK(!mkdir(archive, 0700));
    snprintf(files, sizeof files, "%s/files", archive); CHECK(!mkdir(files, 0700));
    snprintf(keypath, sizeof keypath, "%s/play-history.keyring", files);
    uint8_t keys[8][32]; synthetic_keys(keys); write_file(keypath, keys, sizeof keys);
    dm_friends_history_set_key_provider(NULL); CHECK(!setenv("DM_SYSTEM_DATA_DIR", root, 1));
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) == 0 && valid_key(output+1, 1) && !memcmp(uuid, output+17, 16));
    write_file(keypath, keys, sizeof keys-1); CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0 && all_zero(output+1, 64));
    write_file(keypath, keys, sizeof keys); snprintf(bad, sizeof bad, "%s/key-target", root); CHECK(!rename(keypath, bad)); CHECK(!symlink(bad, keypath));
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0); CHECK(!unlink(keypath)); CHECK(!mkfifo(keypath, 0600));
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0); CHECK(!unlink(keypath)); CHECK(!rename(bad, keypath));
    snprintf(bad, sizeof bad, "%s/archive-target", root); CHECK(!rename(archive, bad)); CHECK(!symlink(bad, archive));
    CHECK(h_fr_GetPHKey(output+1, local_uid, 1) != 0); CHECK(!unlink(archive)); CHECK(!rename(bad, archive));
    CHECK(!wait_depth && atomic_load(&begins) == atomic_load(&ends) && before == fd_count());
    CHECK(!nftw(root, cleanup, 32, FTW_DEPTH|FTW_PHYS));
    printf("Friends history: %u checks; known synthetic HMAC vector, flags, persistent UUID, process race, failures, symlinks and FD lifetime passed.\n", checks);
    return 0;
}
