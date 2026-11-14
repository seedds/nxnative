// account: program handoffs inherit one valid local user and HOME launches keep the preselected-user fallback; GetUserId copies the Uid of any handle, as the SDK's
// accessor does: the host's own handles and handles a game built itself, whatever their third word.
#include "../hle_account.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void hle_signal_system_event(void *g) { (void)g; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}

static void handoff(void) {   // first: PopOpenUsers hands its user over once per process
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
}

static void any_handle(void) {
    UserHandle h; Uid out;
    assert(h_acc_TryOpenPre(&h));
    assert(h_acc_GetUserId(&out, &h) == RES_OK);
    assert(!memcmp(&out, &k_uid, sizeof out));
    // A handle the game built: EA SPORTS FC 27's second controller passes third word 0x2, the FC 26 patch reports 8
    const uint64_t third[] = {2, 8, 0, UINT64_MAX};
    for (unsigned i = 0; i < sizeof third / sizeof third[0]; i++) {
        h = (UserHandle){{0x7012222b20, 0x400000001, third[i]}};
        assert(h_acc_GetUserId(&out, &h) == RES_OK);
        assert(out.v[0] == 0x7012222b20 && out.v[1] == 0x400000001);
        assert(h.v[2] == third[i]);
        h.v[0] = h.v[1] = 0;
        assert(h_acc_GetUserId(&out, &h) == RES_OK);
        assert(out.v[0] == 0 && out.v[1] == 0);
    }
}

int main(void) {
    handoff();
    any_handle();
    puts("t_account: OK (handoff, capacity, repeated pop, HOME fallback, any handle)");
    return 0;
}
