// Account contracts: persistent registration, explicit choices and live sessions.
#include "../hle_account.c"
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void hle_signal_system_event(void *g) { (void)g; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
static int waiting;
void dm_thread_native_wait(int entering) { waiting += entering ? 1 : -1; assert(waiting >= 0); }
static int choice, calls, can_create, can_skip, shown;
static unsigned choices, shown_open;
int front_user_select(const front_user *users, int n, int create, int skip) {
    assert(waiting == 1); calls++; can_create = create; can_skip = skip; shown = n; choices = shown_open = 0;
    for (int i = 0; i < n; i++) { choices |= 1u << users[i].slot; if (users[i].open) shown_open |= 1u << users[i].slot; }
    return choice;
}
static void *session_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 100; i++) {
        UserHandle h; assert(h_acc_OpenUser(&h, &k_uid) == RES_OK); h_acc_CloseUser(&h); h_acc_CloseUser(&h);
    }
    return NULL;
}
static void child(const char *exe, const char *mode, const char *save) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { execl(exe, exe, mode, save, NULL); _exit(127); }
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(int argc, char **argv) {
    if (argc == 3) {
        setenv("DM_SAVE", argv[2], 1);
        int n; Uid list[8], u2 = user_uid(1), out;
        uint8_t exists;
        if (!strcmp(argv[1], "--restart")) {
            assert(h_acc_ListAll(&n, list, 8) == RES_OK && n == 3);
            assert(h_acc_Exists(&exists, &u2) == RES_OK && exists);
            assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
            assert(h_acc_LastOpened(&out) == RES_OK && !memcmp(&out, &k_uid, 16));
        } else if (!strcmp(argv[1], "--corrupt")) {
            assert(h_acc_ListAll(&n, list, 8) == RES_FAIL && n == 0);
            choice = FRONT_USER_CREATE; assert(h_acc_ShowCreator() == RES_FAIL && calls == 0);
        } else {
            choice = FRONT_USER_CREATE;
            assert(h_acc_ShowCreator() == RES_FAIL); // missing parent, never publish an unpersisted user
            assert(h_acc_Exists(&exists, &u2) == RES_OK && !exists);
        }
        return 0;
    }
    char root[] = "/private/tmp/t_account.XXXXXX", save[1024], path[1100]; assert(mkdtemp(root));
    snprintf(save, sizeof save, "%s/save", root); setenv("DM_SAVE", save, 1);
    Uid out, list[8]; UserHandle h, other; int n;
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    assert(h_acc_TryOpenPre(&h) && h_acc_OpenPre(&other) == RES_OK && h.v[2] != other.v[2]);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 1);
    h_acc_CloseUser(&h); h_acc_CloseUser(&h);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 1);
    h_acc_CloseUser(&other);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    // SDK GetUserId copies even foreign/empty handles, without validating a token.
    const uint64_t third[] = {2, 8, 0, UINT64_MAX};
    for (unsigned i = 0; i < sizeof third / sizeof third[0]; i++) {
        h = (UserHandle){{0x7012222b20, 0x400000001, third[i]}};
        assert(h_acc_GetUserId(&out, &h) == RES_OK && out.v[0] == h.v[0] && out.v[1] == h.v[1] && h.v[2] == third[i]);
        h_acc_CloseUser(&h);
        h.v[0] = h.v[1] = 0; assert(h_acc_GetUserId(&out, &h) == RES_OK && !out.v[0] && !out.v[1]);
    }
    uint8_t settings[0x88] = {0};
    // Even one user must be chosen explicitly unless the game allows skipping UI.
    choice = FRONT_USER_CANCEL; out = user_uid(7);
    assert(h_acc_ShowSel(&out, settings) == RES_ACC_CANCELED && dm_uid_user(&out) == 7 && calls == 1 && shown == 1 && can_create);
    settings[0x80] = 1;
    assert(h_acc_ShowSel(&out, settings) == RES_OK && dm_uid_user(&out) == 0 && calls == 1);
    settings[0x83] = 1;
    assert(h_acc_ShowSel(&out, settings) == RES_ACC_CANCELED && calls == 2);
    memset(settings, 0, sizeof settings); settings[0x82] = 1; choice = FRONT_USER_SKIP;
    assert(h_acc_ShowSel(&out, settings) == RES_OK && !out.v[0] && !out.v[1] && can_skip);
    settings[0x82] = 0;
    assert(h_acc_ShowSel(&out, settings) == RES_ACC_CANCELED && !can_skip);
    // A newly made UID is excluded too: skip slots 1 and 2, create slot 3 only.
    Uid u2 = user_uid(1), u3 = user_uid(2);
    memcpy(settings, &k_uid, 16); memcpy(settings + 16, &u2, 16); choice = FRONT_USER_CREATE;
    assert(h_acc_ShowSel(&out, settings) == RES_OK && dm_uid_user(&out) == 2 && shown == 0);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    assert(h_acc_ListAll(&n, list, 8) == RES_OK && n == 2 && dm_uid_user(&list[1]) == 2);
    assert(h_acc_OpenUser(&h, &u2) == RES_FAIL);
    assert(h_acc_OpenUser(&h, &u3) == RES_OK && h_acc_OpenUser(&other, &u3) == RES_OK);
    assert(h_acc_LastOpened(&out) == RES_OK && !memcmp(&out, &u3, 16));
    h_acc_CloseUser(&h); h_acc_CloseUser(&h);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 1 && dm_uid_user(list) == 2);
    memset(settings, 0, sizeof settings); choice = 2;
    assert(h_acc_ShowSel(&out, settings) == RES_OK && shown_open == 4);
    h_acc_CloseUser(&other);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    // Creation has real cancellation and durable registration, but opens no session.
    choice = FRONT_USER_CANCEL; assert(h_acc_ShowCreator() == RES_ACC_CANCELED);
    choice = FRONT_USER_CREATE; assert(h_acc_ShowCreator() == RES_OK);
    char nick[33]; assert(h_acc_GetNick(nick, &u2) == RES_OK && !strcmp(nick, "Player 2"));
    assert(h_acc_ListQual(&n, list, 1) == RES_OK && n == 1);
    assert(h_acc_ListAll(&n, NULL, 8) == RES_OK && n == 0);
    assert(h_acc_UserCount(&n) == RES_OK && n == 3);
    // Exclusions cannot be bypassed by a stale or faulty front-end answer.
    memcpy(settings, &u2, 16); choice = 1;
    assert(h_acc_ShowSel(&out, settings) == RES_ACC_CANCELED && !(choices & 2));
    for (int i = 0; i < 8; i++) { Uid u = user_uid(i); memcpy(settings + i * 16, &u, 16); }
    int before = calls; choice = FRONT_USER_CREATE;
    assert(h_acc_ShowSel(&out, settings) == RES_ACC_CANCELED && calls == before);
    memset(settings, 0, sizeof settings); settings[0x81] = 1;
    assert(h_acc_ShowSel(&out, settings) == RES_FAIL && calls == before);
    settings[0x84] = 1; choice = 1;
    assert(h_acc_ShowSel(&out, settings) == RES_OK && dm_uid_user(&out) == 1);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, session_thread, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0 && waiting == 0);
    child(argv[0], "--restart", save);
    snprintf(path, sizeof path, "%s.users", save); FILE *f = fopen(path, "w"); assert(f); fputs("broken registry\n", f); fclose(f);
    child(argv[0], "--corrupt", save);
    unlink(path);
    snprintf(path, sizeof path, "%s/missing/save", root); child(argv[0], "--unwritable", path);
    rmdir(root);
    puts("t_account: OK (selection, sessions, concurrent closes, fresh-process persistence)");
    return 0;
}
