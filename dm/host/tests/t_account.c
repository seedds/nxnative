// Match the SDK accessor for ordinary and locally constructed guest handles.
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
    // FC 26 builds a separate local guest handle. Its third word is not a host marker.
    // Zero and other manager words have exactly the same accessor semantics in the SDK.
    const uint64_t managers[] = {8, 0, UINT64_MAX};
    for (unsigned i = 0; i < sizeof managers / sizeof managers[0]; i++) {
        h = (UserHandle){{0x1234, 0x5678, managers[i]}};
        assert(h_acc_GetUserId(&out, &h) == RES_OK);
        assert(out.v[0] == 0x1234 && out.v[1] == 0x5678);
        assert(h.v[2] == managers[i]);
        h.v[0] = h.v[1] = 0;
        assert(h_acc_GetUserId(&out, &h) == RES_OK);
        assert(out.v[0] == 0 && out.v[1] == 0);
    }
    puts("t_account: OK");
    return 0;
}
