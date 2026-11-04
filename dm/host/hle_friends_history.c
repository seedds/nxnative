// The key is an opaque local-play-history identity, not an account login token.
// Format verified against the player's own firmware's friends IPC10700: body32
// {type u16=0x101, key-index u8, NSA-present u8, input-flag u8, reserved[3],
//  NSA-ID u64, UUID[16]}, followed by HMAC-SHA256(body32, selected key32).
// The eight signing keys come only from the privately imported firmware resource.
#include "friends_history.h"
#include <CommonCrypto/CommonHMAC.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void dm_guest_host_wait_begin(void);
void dm_guest_host_wait_end(void);
static pthread_mutex_t history_lock = PTHREAD_MUTEX_INITIALIZER;
static dm_friends_history_key_provider key_provider;
static void history_clear(void *memory, size_t size) {
    volatile uint8_t *bytes = memory; while (size--) *bytes++ = 0;
}
#ifdef DM_FRIENDS_HISTORY_TEST
static dm_friends_history_random_provider random_provider;
void dm_friends_history_set_random_provider(dm_friends_history_random_provider provider) {
    pthread_mutex_lock(&history_lock); random_provider = provider; pthread_mutex_unlock(&history_lock);
}
#endif
static void history_random(void *out, unsigned size) {
#ifdef DM_FRIENDS_HISTORY_TEST
    if (random_provider) { random_provider(out, size); return; }
#endif
    arc4random_buf(out, size);
}
void dm_friends_history_set_key_provider(dm_friends_history_key_provider provider) {
    pthread_mutex_lock(&history_lock); key_provider = provider; pthread_mutex_unlock(&history_lock);
}
static int exact_read(int fd, void *out, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, (char *)out + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    char extra; ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    return n == 0;
}
static int exact_write(int fd, const void *bytes, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, (const char *)bytes + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        done += (size_t)n;
    }
    return fsync(fd) == 0;
}
static bool firmware_keys(uint8_t keys[8][32]) {
    const char *root = getenv("DM_SYSTEM_DATA_DIR");
    if (!root || !*root) return false;
    int base = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base < 0) return false;
    int archive = openat(base, "010000000000000e", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(base); if (archive < 0) return false;
    int files = openat(archive, "files", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(archive); if (files < 0) return false;
    int fd = openat(files, "play-history.keyring", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(files); if (fd < 0) return false;
    struct stat st; int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) &&
        st.st_size == DM_FRIENDS_HISTORY_KEYRING_SIZE && exact_read(fd, keys, DM_FRIENDS_HISTORY_KEYRING_SIZE);
    close(fd); return ok;
}
static int uuid_valid(const uint8_t uuid[16]) {
    return (uuid[6] & 0xf0) == 0x40 && (uuid[8] & 0xc0) == 0x80;
}
static int read_uuid(int directory, const char *name, uint8_t uuid[16]) {
    int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st; int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
        !(st.st_mode & 077) && st.st_nlink == 1 && st.st_size == 16 && exact_read(fd, uuid, 16) && uuid_valid(uuid);
    close(fd); return ok ? 1 : -1;
}
static void hex_bytes(char *out, const uint8_t *bytes, unsigned count) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < count; ++i) { out[2*i] = hex[bytes[i] >> 4]; out[2*i+1] = hex[bytes[i] & 15]; }
    out[2*count] = 0;
}
static int local_uuid(const uint8_t uid[16], uint8_t uuid[16]) {
    const char *root = getenv("DM_ROOT"); if (!root || !*root) root = ".";
    int base = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base < 0) return 0;
    int made = mkdirat(base, "friends-history", 0700);
    if (made && errno != EEXIST) { close(base); return 0; }
    int directory = openat(base, "friends-history", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(base); if (directory < 0) return 0;
    struct stat st;
    if (fstat(directory, &st) || st.st_uid != geteuid() || (st.st_mode & 077)) { close(directory); return 0; }
    char name[42]; hex_bytes(name, uid, 16); memcpy(name+32, ".uuid.bin", 10);
    int exists = read_uuid(directory, name, uuid);
    if (exists) { close(directory); return exists > 0; }
    uint8_t random[16]; history_random(uuid, 16); uuid[6] = (uuid[6] & 15) | 0x40; uuid[8] = (uuid[8] & 63) | 0x80;
    history_random(random, sizeof random); char temporary[38] = ".tmp-"; hex_bytes(temporary+5, random, 16);
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) { close(directory); return 0; }
    int ok = exact_write(fd, uuid, 16), committed = 0; close(fd);
    if (ok) {
        if (!renameatx_np(directory, temporary, directory, name, RENAME_EXCL)) committed = 1;
        else if (errno != EEXIST) ok = 0;
    }
    if (!committed && unlinkat(directory, temporary, 0)) ok = 0;
    if (ok && fsync(directory)) ok = 0;
    // Another process may have committed first. Always use the committed file.
    if (ok) ok = read_uuid(directory, name, uuid) == 1;
    close(directory); return ok;
}
uint32_t dm_friends_history_key(void *out, const void *uid_pointer, unsigned input_flag) {
    if (!out) return DM_FRIENDS_HISTORY_FAIL;
    uint8_t uid[16] = {0}; if (uid_pointer) memcpy(uid, uid_pointer, sizeof uid);
    memset(out, 0, DM_FRIENDS_HISTORY_KEY_SIZE);
    uint8_t present = 0; for (unsigned i = 0; i < sizeof uid; ++i) present |= uid[i];
    if (!present) return DM_FRIENDS_HISTORY_INVALID_UID;
    uint8_t keys[8][32] = {{0}}, result[64] = {1, 1}, uuid[16]; uint32_t status = DM_FRIENDS_HISTORY_FAIL;
    dm_guest_host_wait_begin(); pthread_mutex_lock(&history_lock);
    if ((key_provider ? key_provider(keys) : firmware_keys(keys)) && local_uuid(uid, uuid)) {
        uint8_t index; history_random(&index, 1); result[2] = index & 7; result[4] = input_flag & 1;
        memcpy(result+16, uuid, sizeof uuid);
        CCHmac(kCCHmacAlgSHA256, keys[result[2]], 32, result, 32, result+32);
        memcpy(out, result, sizeof result); status = DM_FRIENDS_HISTORY_OK;
    }
    history_clear(keys, sizeof keys); history_clear(result, sizeof result);
    pthread_mutex_unlock(&history_lock); dm_guest_host_wait_end();
    return status;
}
