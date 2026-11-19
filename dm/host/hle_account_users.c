// Local profiles belong to this app's save store. A tiny registry beside DM_SAVE
// survives process restarts without becoming part of any user's save transaction.
#include "dm.h"
#include "account_users.h"
#include <fcntl.h>
#include <unistd.h>

static pthread_mutex_t g_users_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_users_path[1024];
static unsigned g_users_mask;
static int g_users_loaded;

// Called under g_users_lock. A changed DM_SAVE is a different store (also used by
// synthetic fs tests); normal game runs keep it fixed for the process lifetime.
static void users_load(void) {
    const char *save = getenv("DM_SAVE");
    char path[1024] = "";
    if (save && *save) {
        size_t n = strlen(save); while (n > 1 && save[n - 1] == '/') n--;
        if (n + sizeof ".users" > sizeof path) { g_users_mask = 0; g_users_loaded = 0; return; }
        memcpy(path, save, n); memcpy(path + n, ".users", sizeof ".users");
    }
    if (g_users_loaded && !strcmp(path, g_users_path)) return;
    strlcpy(g_users_path, path, sizeof g_users_path);
    g_users_loaded = 1; g_users_mask = 1;
    if (!*path) return;   // user 1 still exists without a save store; creation cannot persist
    FILE *f = fopen(path, "r");
    if (!f && errno == ENOENT) return;
    unsigned mask = 0; char extra;
    int valid = f && fscanf(f, "nxnative-users-1 %x %c", &mask, &extra) == 1 && (mask & 1) && mask < (1u << DM_USERS) && !ferror(f);
    if (f) fclose(f);
    if (valid) g_users_mask = mask;
    else { g_users_mask = 0; LOG("[RED] account: cannot read user registry %s; leaving it intact", path); }
}

unsigned dm_account_users(void) {
    pthread_mutex_lock(&g_users_lock); users_load();
    unsigned mask = g_users_mask;
    pthread_mutex_unlock(&g_users_lock); return mask;
}
int dm_account_user(const void *uid) {
    int i = dm_uid_user(uid);
    return i >= 0 && (dm_account_users() & (1u << i)) ? i : -1;
}
int dm_account_create_user(int user) {
    if (user < 1 || user >= DM_USERS) return -1;
    pthread_mutex_lock(&g_users_lock); users_load();
    int result = -1, fd = -1; char tmp[1100] = "";
    if (!g_users_mask || !g_users_path[0] || (g_users_mask & (1u << user))) goto done;
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", g_users_path);
    fd = mkstemp(tmp);
    if (fd < 0) goto done;
    unsigned mask = g_users_mask | (1u << user);
    char data[64]; int n = snprintf(data, sizeof data, "nxnative-users-1 %02x\n", mask);
    if (write(fd, data, n) != n || fsync(fd)) goto done;
    if (close(fd)) { fd = -1; goto done; }
    fd = -1;
    if (rename(tmp, g_users_path)) goto done;
    g_users_mask = mask; result = 0;
done:
    if (fd >= 0) close(fd);
    if (result && tmp[0]) unlink(tmp);
    if (result) LOG("[RED] account: could not persist user %d in %s", user + 1, g_users_path[0] ? g_users_path : "an unset DM_SAVE");
    pthread_mutex_unlock(&g_users_lock); return result;
}
