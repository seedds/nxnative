// A failed fetch worker must never turn a bounded cloud read into a synchronous one.
// Only save_sync.c is built with pthread_create renamed to the EAGAIN injector below;
// the real filesystem mount, reads, writes and Commit still run against a synthetic local save.
#include "../hle_fs.c"
#include <pthread.h>

struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }

static int creates, fetches, stores, checks, fails;
int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    (void)thread; (void)attr; (void)fn; (void)arg; creates++;
    return EAGAIN;
}
// The platform's fetch may block indefinitely (for example in an iCloud download).
// Return an error here so the old synchronous fallback fails deterministically without hanging the suite.
int front_save_sync_fetch(const char *cloud, const char *local) { (void)cloud; (void)local; fetches++; return -1; }
int front_save_sync_store(const char *local, const char *cloud) { (void)local; (void)cloud; stores++; return 1; }

#define CHECK(c, label) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static int read_save(char *out, size_t n) {
    uint64_t h; size_t got = 0;
    if (h_fs_OpenFile(&h, "save:/game.dat", MODE_READ)) return 0;
    Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0;
    return !r;
}
static int disk_has(const char *path, const char *value) {
    char b[32] = {0}; FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f);
    return n == strlen(value) && !memcmp(b, value, n);
}
int main(void) {
    char root[1024], save[1200], file[1400], cloud[1200], text[32] = {0};
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/t_save_sync_fetch.XXXXXX", tmp ? tmp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    snprintf(save, sizeof save, "%s/save", root); mkdir(save, 0755);
    snprintf(file, sizeof file, "%s/game.dat", save);
    FILE *f = fopen(file, "w"); if (!f) { perror("local save"); return 1; }
    fputs("before", f); fclose(f);
    snprintf(cloud, sizeof cloud, "%s/cloud", root); mkdir(cloud, 0755);
    setenv("DM_SAVE", save, 1); setenv("DM_SAVE_SYNC", cloud, 1);

    CHECK(h_fs_MountSave("save", NULL) == RES_OK, "failed cloud worker does not fail the local save mount");
    CHECK(creates > 0, "mount attempts to create a cloud worker");
    CHECK(fetches == 0, "mount does not fetch synchronously after pthread_create returns EAGAIN");
    CHECK(read_save(text, sizeof text) && !strcmp(text, "before"), "the mounted local save remains readable");
    uint64_t h = 0;
    Result r = h_fs_OpenFile(&h, "save:/game.dat", MODE_WRITE);
    CHECK(r == RES_OK, "the local save remains writable");
    if (!r) {
        CHECK(h_fs_SetFileSize(h, 5) == RES_OK, "resize the local save");
        CHECK(h_fs_WriteFile(h, 0, "after", 5, NULL) == RES_OK, "write the next local save");
        h_fs_CloseFile(h);
    }
    CHECK(h_fs_Commit("save") == RES_OK && disk_has(file, "after"), "Commit persists the local save while cloud workers are unavailable");
    CHECK(save_sync_after_exit(save, DM_GAME_ID) == 0, "later send reports the unavailable cloud fetch");
    CHECK(fetches == 0 && stores == 0, "later send neither fetches synchronously nor overwrites the cloud");
    CHECK(disk_has(file, "after"), "a failed later send preserves the committed local save");

    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); system(cmd);
    printf("save sync worker failure: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
