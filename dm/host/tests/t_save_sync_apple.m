// Verify the Apple NSFileCoordinator wrapper (dm/host/save_sync_apple.m) on macOS:
// coordinated fetch/identity capture, conditional replacement on absent and existing targets,
// concurrent two-sender race serialization, placeholder detection, and error reporting.
#import <Foundation/Foundation.h>
#include "save_sync.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(text, 1, strlen(text), f) != strlen(text) || fclose(f)) abort();
}
static int is_text(const char *path, const char *text) {
    char b[128] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    return n == strlen(text) && !memcmp(b, text, n);
}

typedef struct {
    pthread_mutex_t *mu;
    pthread_cond_t *cv;
    int *ready;
    const char *local, *cloud;
    ss_cloud_id expected;
    int result;
} race_ctx;

static void *race_store_worker(void *p) {
    race_ctx *c = p;
    pthread_mutex_lock(c->mu);
    (*c->ready)++;
    if (*c->ready >= 2) pthread_cond_broadcast(c->cv);
    while (*c->ready < 2) pthread_cond_wait(c->cv, c->mu);
    pthread_mutex_unlock(c->mu);
    c->result = front_save_sync_store(c->local, c->cloud, &c->expected);
    return NULL;
}

int main(void) {
    @autoreleasepool {
        char root[PATH_MAX], title_dir[PATH_MAX], cf[PATH_MAX], ph[PATH_MAX], fetched[PATH_MAX], src_a[PATH_MAX], src_b[PATH_MAX];
        const char *tmp = getenv("TMPDIR");
        snprintf(root, sizeof root, "%s/t_save_sync_apple.XXXXXX", tmp ? tmp : "/tmp");
        if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
        snprintf(title_dir, sizeof title_dir, "%s/0100000000010000", root);
        snprintf(cf, sizeof cf, "%s/save.dmsync", title_dir);
        snprintf(ph, sizeof ph, "%s/.save.dmsync.icloud", title_dir);
        snprintf(fetched, sizeof fetched, "%s/fetched.dmsync", root);
        snprintf(src_a, sizeof src_a, "%s/a.dmsync", root);
        snprintf(src_b, sizeof src_b, "%s/b.dmsync", root);
        mkdir(title_dir, 0755);
        put(src_a, "payload-A");
        put(src_b, "payload-B-longer");

        // 1. Absent fetch returns 0 with present=0; first store creates it; second store with stale absent id is rejected.
        ss_cloud_id id0 = { .present = 1 };
        CHECK(front_save_sync_fetch(cf, fetched, &id0) == 0 && id0.present == 0, "absent fetch returns 0 and clears identity");
        CHECK(front_save_sync_store(src_a, cf, &id0) == SS_STORE_OK && is_text(cf, "payload-A"), "absent conditional store creates file");
        CHECK(front_save_sync_store(src_b, cf, &id0) == SS_STORE_STALE && is_text(cf, "payload-A"), "stale absent store does not overwrite existing file");

        // 2. Present fetch captures stat identity; matching store replaces; stale store fails without overwriting.
        ss_cloud_id id1 = {0};
        struct stat st;
        CHECK(stat(cf, &st) == 0, "stat created cloud file");
        CHECK(front_save_sync_fetch(cf, fetched, &id1) == 1 && is_text(fetched, "payload-A"), "present fetch copies file");
        CHECK(id1.present == 1 && id1.dev == (uint64_t)st.st_dev && id1.ino == (uint64_t)st.st_ino && id1.size == (int64_t)st.st_size, "present fetch records destination identity");

        // 3. Concurrent two-thread store race through NSFileCoordinator with the same observed identity.
        pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
        int ready = 0;
        put(src_a, "race-winner-A");
        put(src_b, "race-winner-B");
        race_ctx ca = {&mu, &cv, &ready, src_a, cf, id1, -99};
        race_ctx cb = {&mu, &cv, &ready, src_b, cf, id1, -99};
        pthread_t ta, tb;
        if (pthread_create(&ta, NULL, race_store_worker, &ca) || pthread_create(&tb, NULL, race_store_worker, &cb)) abort();
        pthread_join(ta, NULL);
        pthread_join(tb, NULL);
        CHECK((ca.result == SS_STORE_OK && cb.result == SS_STORE_STALE) || (cb.result == SS_STORE_OK && ca.result == SS_STORE_STALE),
              "exactly one NSFileCoordinator store succeeds and one is stale (%d, %d)", ca.result, cb.result);
        const char *winner = ca.result == SS_STORE_OK ? "race-winner-A" : "race-winner-B";
        CHECK(is_text(cf, winner), "cloud keeps winner after concurrent NSFileCoordinator race");

        // 4. Unreadable file and unreadable parent directory both fail fetch (-1), never reporting absent (0).
        chmod(cf, 0);
        CHECK(front_save_sync_fetch(cf, fetched, &id1) == -1, "unreadable cloud file returns -1");
        chmod(cf, 0644);
        chmod(title_dir, 0);
        CHECK(front_save_sync_fetch(cf, fetched, &id1) == -1, "unreadable cloud directory returns -1, not absent");
        chmod(title_dir, 0755);

        // 5. Placeholder appearing prevents absent-target creation.
        unlink(cf);
        put(ph, "placeholder");
        CHECK(front_save_sync_store(src_a, cf, &id0) == SS_STORE_STALE && access(cf, F_OK) == -1, "placeholder blocks absent-target store");
        unlink(ph);

        unlink(fetched); unlink(src_a); unlink(src_b); unlink(cf); rmdir(title_dir); rmdir(root);
        printf("save-sync apple: %d checks, %d failed\n", checks, fails);
        return fails != 0;
    }
}
