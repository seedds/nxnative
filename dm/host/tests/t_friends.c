// friends service unit test: presence is accepted (Twelve Minutes stops its start-up on a failed UserPresence::Initialize, seedds/nxnative#217),
// while the calls that would reach the service still fail. Expected results are hand-written, not taken from the implementation.
#include "../hle_friends.c"
#include <mach-o/getsect.h>
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
extern const struct mach_header_64 _mh_execute_header;
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", s); } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) {
        if (strcmp(entries[i].name, name)) continue;
        count++; fn = entries[i].fn;
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
int main(void) {
    uint8_t presence[0x100] = {0}, uid[16] = {1};
    CHECK(h_fr_UpInit(presence, uid) == RES_OK, "UserPresence::Initialize succeeds");
    CHECK(h_fr_UpDesc(presence, "in game") == RES_OK, "UserPresence::SetDescription succeeds");
    CHECK(h_fr_UpCommit(presence) == RES_OK, "UserPresence::Commit succeeds");
    CHECK(h_fr_NqInit(presence, uid) == RES_FAIL, "NotificationQueue::Initialize still fails");
    CHECK(h_fr_OpenSession(uid) == RES_FAIL, "DeclareOpenOnlinePlaySession still fails");
    // Resolve the actual takeovers: a missing or misspelled overload would run the SDK's friends client instead (PR #2162).
    Result (*key_uid)(void *, const void *, int) = hook("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb");
    Result (*key_no_uid)(void *, int) = hook("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyEb");
    uint8_t key[0x100], before[sizeof key];
    memset(before, 0xa5, sizeof before);
    const int local_args[] = {0, 1, 0x100, 0x101};   // guest bools may leave the upper register bits set (hle.h)
    for (size_t i = 0; i < sizeof local_args / sizeof *local_args; i++) {
        if (key_uid) {
            memcpy(key, before, sizeof key);
            CHECK(key_uid(key, uid, local_args[i]) == RES_FAIL, "play history key with Uid is unavailable");
            CHECK(!memcmp(key, before, sizeof key), "failed key query with Uid leaves output untouched");
        }
        if (key_no_uid) {
            memcpy(key, before, sizeof key);
            CHECK(key_no_uid(key, local_args[i]) == RES_FAIL, "play history key without Uid is unavailable");
            CHECK(!memcmp(key, before, sizeof key), "failed key query without Uid leaves output untouched");
        }
    }
    printf("t_friends: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
