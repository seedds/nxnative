// account::GetUserId copies the Uid of any handle, as the SDK's accessor does: the host's own handles and handles a game built itself, whatever their third word.
#include "../hle_account.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void hle_signal_system_event(void *g) { (void)g; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}

int main(void) {
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
    puts("t_account: OK");
    return 0;
}
