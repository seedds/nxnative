// Program handoffs inherit one valid local user; HOME launches keep the preselected-user fallback.
#include "../hle_account.c"
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
void hle_signal_system_event(void *g) { (void)g; }
static void rejected(int sig) { (void)sig; _exit(77); }

int main(void) {
    UserHandle handles[2] = {{{0}}, {{11, 22, 33}}}; Uid uid; int n = -1;
    unsetenv("DM_PREV_PROGRAM");
    h_acc_PopOpen(&n, handles, 2); assert(n == 0 && handles[0].v[2] == 0);
    assert(h_acc_TryOpenPre(&handles[0]) == 1);
    assert(h_acc_GetUserId(&uid, &handles[0]) == RES_OK && !memcmp(&uid, &k_uid, sizeof uid));
    const char *invalid[] = {"", "-1", "16", "garbage", "0junk", "99999999999999999999999999"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        setenv("DM_PREV_PROGRAM", invalid[i], 1);
        h_acc_PopOpen(&n, handles, 2); assert(n == 0);
    }
    setenv("DM_PREV_PROGRAM", "0", 1);
    h_acc_PopOpen(&n, handles, 0); assert(n == 0);
    h_acc_PopOpen(&n, NULL, 2); assert(n == 0);
    memset(&handles[0], 0, sizeof handles[0]);
    h_acc_PopOpen(&n, handles, 2); assert(n == 1);
    assert(h_acc_GetUserId(&uid, &handles[0]) == RES_OK && !memcmp(&uid, &k_uid, sizeof uid));
    assert(handles[1].v[0] == 11 && handles[1].v[1] == 22 && handles[1].v[2] == 33);
    h_acc_PopOpen(&n, handles, 2); assert(n == 0);
    pid_t child = fork(); assert(child >= 0);
    if (!child) { signal(SIGABRT, rejected); UserHandle bad = {{0}}; h_acc_GetUserId(&uid, &bad); _exit(1); }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    puts("account handoff, capacity, repeated pop, HOME fallback and invalid-handle checks passed");
    return 0;
}
