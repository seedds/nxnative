// nn::account's user surface: the local user is handed over, not assumed. A title that reads a player's user
// id at startup (Borderlands GOTY, 20 ms after it maps the first gamepad, main+0xe96e00) only gets a valid one
// if the user is open *before* that read: ListOpenUsers reports what the application has opened (nothing at
// launch), and PopOpenUsers hands the launching user over once — a second call has nothing left to give, as on
// a console, where the system's queue is emptied by the first pop. The fallback those titles take when the
// list is empty is TryOpenPreselectedUser, and its handle must be shaped like OpenUser's.
#include "../hle.h"
#include <assert.h>
#include <stdarg.h>

static int failures;
static void chk(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); failures++; }
    else fprintf(stderr, "ok   %s\n", what);
}
// What hle_account.c takes from the rest of the host
void hle_signal_system_event(void *ev) { (void)ev; }
static int applet_enters;
void dm_applet_enter(const char *name) { (void)name; applet_enters++; }
void dm_applet_leave(void) {}   // the cleanup runs as the call returns   // hle.h's dm_applet_leave_ inline calls this
uint32_t thr_cur_handle(void) { return 0x1001; }
void dm_thread_caller(char *b, size_t n) { snprintf(b, n, "test+0x0"); }
uint64_t g_t0_ns;
uint64_t now_ns(void) { static uint64_t t; return t += 1000000; }

#include "../hle_account.c"

int main(void) {
    int n = -1;
    Uid u[4];
    memset(u, 0xAA, sizeof u);
    h_acc_ListOpen(&n, u, 4);
    chk(n == 0, "ListOpenUsers reports nothing before the application has opened a user");
    chk(u[0].v[0] == 0xAAAAAAAAAAAAAAAAull, "ListOpenUsers writes no user when the count is 0");

    UserHandle h[4];
    memset(h, 0, sizeof h);
    h_acc_PopOpen(&n, h, 4);
    chk(n == 1, "PopOpenUsers hands the launching user over once");
    chk(h[0].v[0] == k_uid.v[0] && h[0].v[1] == k_uid.v[1], "the popped handle holds the local user's Uid");
    chk(h[0].v[2] == HANDLE_MAGIC, "the popped handle carries the handle magic, as OpenUser's does");
    h_acc_PopOpen(&n, h, 4);
    chk(n == 0, "a second PopOpenUsers has nothing left to hand over");

    Uid back = {{0, 0}};
    h_acc_GetUserId(&back, &h[0]);
    chk(back.v[0] == k_uid.v[0] && back.v[1] == k_uid.v[1], "GetUserId accepts the popped handle");

    h_acc_ListOpen(&n, u, 4);
    chk(n == 1 && u[0].v[0] == k_uid.v[0], "a popped user is open, so ListOpenUsers reports it");

    h_acc_CloseUser(&h[0]);
    h_acc_ListOpen(&n, u, 4);
    chk(n == 0, "closing the handle leaves no open user");

    // A title that lists first and falls back to the preselected user (Mario 1.2.1, Borderlands GOTY) must
    // find the user on that fallback: the handle it gets is the one PopOpenUsers would have given.
    memset(h, 0, sizeof h);
    chk(h_acc_TryOpenPre(&h[0]) == 1, "TryOpenPreselectedUser opens the launching user");
    chk(h[0].v[2] == HANDLE_MAGIC && h[0].v[0] == k_uid.v[0], "the preselected handle is shaped like OpenUser's");
    h_acc_ListOpen(&n, u, 4);
    chk(n == 1, "the preselected user is open too");
    h_acc_ListOpen(&n, u, 0);
    chk(n == 0, "ListOpenUsers with room for none reports none");

    // OpenUser for the local user opens it as well; the user-selector applet is what the host answers instantly
    memset(h, 0, sizeof h);
    h_acc_OpenUser(&h[0], &k_uid);
    chk(h[0].v[2] == HANDLE_MAGIC, "OpenUser issues a handle with the magic");
    Uid sel = {{0, 0}};
    h_acc_ShowSel(&sel, NULL);
    chk(sel.v[0] == k_uid.v[0], "ShowUserSelector answers with the local user");
    chk(applet_enters == 1, "ShowUserSelector goes through the applet bookkeeping");

    if (failures) { fprintf(stderr, "%d account check(s) failed\n", failures); return 1; }
    fprintf(stderr, "account: all checks passed\n");
    return 0;
}
