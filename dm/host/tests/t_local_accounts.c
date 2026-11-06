// Exercise the nn::account hooks with the launch-time registry and the original single user.
#include "../hle_account.c"
#include <assert.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
__thread _Atomic int dm_host_work;
void hle_signal_system_event(void *ev) { (void)ev; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}

static const Uid alice = {{0x78626c0000000001ull, 0x53574e4154495645ull}};
static const Uid bob = {{2, 0xaabbccddull}};
static const Uid unknown = {{2, 1}}; // Same low word as Bob.

static int same(const Uid *a, const Uid *b) { return !memcmp(a, b, sizeof *a); }

static void legacy(void) {
    int n;
    Uid uid;
    char nick[33];
    UserHandle h;
    assert(h_acc_UserCount(&n) == RES_OK && n == 1);
    assert(h_acc_ListOpen(&n, &uid, 1) == RES_OK && n == 1 && same(&uid, &alice));
    assert(h_acc_GetNick(nick, &alice) == RES_OK && !strcmp(nick, "Legacy player"));
    assert(h_acc_OpenPre(&h) == RES_OK); // SDK 6 tests the Result.
    assert(h_acc_GetUserId(&uid, &h) == RES_OK && same(&uid, &alice));
    h_acc_CloseUser(&h);
    assert(h_acc_ListOpen(&n, &uid, 1) == RES_OK && n == 1 && same(&uid, &alice));
}

static void configured(void) {
    int n;
    Uid uids[2], uid;
    uint8_t exists;
    _Alignas(Uid) uint8_t settings[0x90] = {0};
    char nick[33];
    UserHandle first, second, pre, copy;

    assert(h_acc_UserCount(&n) == RES_OK && n == 2);
    assert(h_acc_ListAll(&n, uids, 2) == RES_OK && n == 2);
    assert(same(&uids[0], &alice) && same(&uids[1], &bob));
    uids[1] = unknown;
    assert(h_acc_ListQual(&n, uids, 1) == RES_OK && n == 1);
    assert(same(&uids[0], &alice) && same(&uids[1], &unknown)); // Guest buffer capacity.
    assert(h_acc_ListAll(&n, NULL, 0) == RES_OK && n == 0);
    assert(h_acc_Exists(&exists, &bob) == RES_OK && exists);
    assert(h_acc_Exists(&exists, &unknown) == RES_OK && !exists);
    assert(h_acc_GetNick(nick, &alice) == RES_OK && !strcmp(nick, "Alice"));
    assert(h_acc_GetNick(nick, &bob) == RES_OK && !strcmp(nick, "Bob"));
    assert(h_acc_GetNick(nick, &unknown) != RES_OK);
    assert(h_acc_ListOpen(&n, uids, 2) == RES_OK && n == 0);
    assert(h_acc_LastOpened(&uid) == RES_OK && same(&uid, &bob));
    assert(h_acc_ShowSel(&uid, settings) == RES_OK && same(&uid, &bob));
    memcpy(settings, &bob, sizeof bob); // SDK UserSelectionSettings.invalidUidList[0].
    assert(h_acc_ShowSel(&uid, settings) != RES_OK);
    memset(settings, 0, sizeof settings);
    settings[0x81] = 1; // isNetworkServiceAccountRequired.
    assert(h_acc_ShowSel(&uid, settings) != RES_OK);
    h_acc_PopOpen(&n, &pre, 1);
    assert(n == 0); // The launcher hands off a choice, not an opened session.

    assert(h_acc_TryOpenPre(&pre) == 1);
    assert(h_acc_GetUserId(&uid, &pre) == RES_OK && same(&uid, &bob));
    assert(h_acc_OpenUser(&first, &alice) == RES_OK);
    copy = first;
    assert(h_acc_GetUserId(&uid, &copy) == RES_OK && same(&uid, &alice));
    memcpy(&copy, &bob, sizeof bob); // Different registered Uid with Alice's session token.
    assert(h_acc_GetUserId(&uid, &copy) != RES_OK);
    h_acc_CloseUser(&copy);
    copy = first;
    assert(h_acc_GetUserId(&uid, &copy) == RES_OK && same(&uid, &alice));
    assert(h_acc_OpenUser(&second, &alice) == RES_OK);
    assert(h_acc_ListOpen(&n, uids, 2) == RES_OK && n == 2);
    assert(same(&uids[0], &alice) && same(&uids[1], &bob));
    h_acc_CloseUser(&copy);
    h_acc_CloseUser(&first); // A copied handle names the same session.
    assert(h_acc_GetUserId(&uid, &first) != RES_OK);
    assert(h_acc_GetUserId(&uid, &second) == RES_OK && same(&uid, &alice));
    assert(h_acc_ListOpen(&n, uids, 2) == RES_OK && n == 2);
    h_acc_CloseUser(&second);
    assert(h_acc_ListOpen(&n, uids, 2) == RES_OK && n == 1 && same(&uids[0], &bob));
    assert(h_acc_LastOpened(&uid) == RES_OK && same(&uid, &alice));
    assert(h_acc_OpenUser(&first, &unknown) != RES_OK);
    assert(h_acc_LastOpened(&uid) == RES_OK && same(&uid, &alice));
    assert(h_acc_OpenPre(&first) == RES_OK);
    assert(h_acc_LastOpened(&uid) == RES_OK && same(&uid, &bob));
    h_acc_CloseUser(&first);
    h_acc_CloseUser(&pre);
    assert(h_acc_ListOpen(&n, uids, 2) == RES_OK && n == 0);
}

int main(int argc, char **argv) {
    if (argc == 2) { configured(); return 0; }
    unsetenv("DM_USERS_FILE");
    unsetenv("DM_USER_ID");
    setenv("DM_NICK", "Legacy player", 1);
    legacy();

    // Spawn a new launch. The registry is immutable after its first account call.
    char path[] = "/tmp/dm-local-accounts-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file);
    assert(fputs("{\"version\":1,\"users\":["
                 "{\"id\":\"53574e415449564578626c0000000001\",\"nickname\":\"Alice\"},"
                 "{\"id\":\"00000000aabbccdd0000000000000002\",\"nickname\":\"Bob\"}]}", file) >= 0);
    assert(fclose(file) == 0);
    setenv("DM_USERS_FILE", path, 1);
    setenv("DM_USER_ID", "00000000aabbccdd0000000000000002", 1);
    pid_t pid;
    char *args[] = {argv[0], "configured", NULL};
    int status = 0;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, args, environ);
    if (rc == 0) assert(waitpid(pid, &status, 0) == pid);
    assert(unlink(path) == 0);
    assert(rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("local account hooks passed");
    return 0;
}
