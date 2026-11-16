// friends service unit test: presence is accepted (Twelve Minutes stops its start-up on a failed UserPresence::Initialize, seedds/nxnative#217),
// while the calls that would reach the service still fail. Expected results are hand-written, not taken from the implementation.
#include "../hle_friends.c"
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", s); } while (0)
int main(void) {
    uint8_t presence[0x100] = {0}, uid[16] = {1};
    CHECK(h_fr_UpInit(presence, uid) == RES_OK, "UserPresence::Initialize succeeds");
    CHECK(h_fr_UpDesc(presence, "in game") == RES_OK, "UserPresence::SetDescription succeeds");
    CHECK(h_fr_UpCommit(presence) == RES_OK, "UserPresence::Commit succeeds");
    CHECK(h_fr_NqInit(presence, uid) == RES_FAIL, "NotificationQueue::Initialize still fails");
    CHECK(h_fr_OpenSession(uid) == RES_FAIL, "DeclareOpenOnlinePlaySession still fails");
    printf("t_friends: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
