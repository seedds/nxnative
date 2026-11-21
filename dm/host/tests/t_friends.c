// friends service unit test: presence is accepted (Twelve Minutes stops its start-up on a failed UserPresence::Initialize, seedds/nxnative#217),
// while the calls that would reach the service still fail. Expected results are hand-written, not taken from the implementation.
#include "../hle_friends.c"
#include <mach-o/getsect.h>
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { (void)g; }
// the local user here (t_nextendo_off.c has the offline Nextendo provider); the presence and key checks below use another Uid
static const uint8_t k_local[16] = {2};
int dm_account_uid_exists(const void *uid) { return uid && !memcmp(uid, k_local, sizeof k_local); }
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
static void check_friend_ids(void) {   // seedds's t_server_environment checks: the provider answers them offline as the cut there did
    Result (*ids)(int *, void *, int, int, const void *) = hook("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE") ? h_fr_GetFriendIds : NULL;
    Result (*ids_uid)(int *, void *, const void *, int, int, const void *) = hook("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEiiRKNS0_12FriendFilterE") ? h_fr_GetFriendIdsUid : NULL;
    const uint64_t before[] = {0x123456789abcdef0, 0xfedcba9876543210};
    uint64_t output[2];
    uint8_t uid[16] = {1}, filter[32] = {0};
    if (ids) {
        int count = 7;
        memcpy(output, before, sizeof output);
        CHECK(ids(&count, output, 2, 2, filter) == RES_FAIL, "friend IDs without Uid remain unavailable offline");
        CHECK(count == 0, "unavailable ID list reports zero entries");
        CHECK(!memcmp(output, before, sizeof output), "unavailable ID list leaves client storage untouched");
        count = 7;
        CHECK(ids(&count, NULL, 0, 0, filter) == RES_FAIL && count == 0, "zero-capacity ID query remains empty");
    }
    if (ids_uid) {
        int count = 7;
        memcpy(output, before, sizeof output);
        CHECK(ids_uid(&count, output, uid, 2, 2, filter) == RES_FAIL, "friend IDs with Uid remain unavailable offline");
        CHECK(count == 0, "unavailable per-user ID list reports zero entries");
        CHECK(!memcmp(output, before, sizeof output), "unavailable per-user ID list leaves client storage untouched");
        count = 7;
        CHECK(ids_uid(&count, NULL, uid, 0, 0, filter) == RES_FAIL && count == 0, "zero-capacity per-user query remains empty");
    }
}

int main(void) {
    uint8_t presence[0x100] = {0}, uid[16] = {1};
    CHECK(h_fr_UpInit(presence, uid) == RES_OK, "UserPresence::Initialize succeeds");
    CHECK(h_fr_UpDesc(presence, "in game") == RES_OK, "UserPresence::SetDescription succeeds");
    CHECK(h_fr_UpCommit(presence) == RES_OK, "UserPresence::Commit succeeds");
    CHECK(h_fr_NqInit(presence, uid) == RES_FAIL, "NotificationQueue::Initialize still fails");
    CHECK(h_fr_OpenSession(uid) == RES_FAIL, "DeclareOpenOnlinePlaySession still fails");
    // Resolve the actual takeovers: a missing or misspelled overload would run the SDK's friends client instead (PR #2162).
    // The Uid form is a wait entry (HLE_FORCE_WAIT): its table function is the gate into entry.S, which only a guest thread may enter, so the check calls the hook itself
    Result (*key_uid)(void *, const void *, unsigned) = hook("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb") ? h_fr_GetPHKey : NULL;
    Result (*key_no_uid)(void *, int) = hook("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyEb");
    uint8_t key[0x100], before[sizeof key];
    memset(before, 0xa5, sizeof before);
    const int local_args[] = {0, 1, 0x100, 0x101};   // guest bools may leave the upper register bits set (hle.h)
    for (size_t i = 0; i < sizeof local_args / sizeof *local_args; i++) {
        if (key_uid) {
            memcpy(key, before, sizeof key);
            // The Uid form is the host's own key (hle_friends_history.c, t_friends_history): a Uid other than the local user's is refused and the key cleared
            CHECK(key_uid(key, uid, local_args[i]) == DM_FRIENDS_HISTORY_INVALID_UID, "play history key for another Uid is refused");
            CHECK(!memcmp(key + DM_FRIENDS_HISTORY_KEY_SIZE, before + DM_FRIENDS_HISTORY_KEY_SIZE, sizeof key - DM_FRIENDS_HISTORY_KEY_SIZE), "the refusal writes only the key's 64 bytes");
        }
        if (key_no_uid) {
            memcpy(key, before, sizeof key);
            CHECK(key_no_uid(key, local_args[i]) == RES_FAIL, "play history key without Uid is unavailable");
            CHECK(!memcmp(key, before, sizeof key), "failed key query without Uid leaves output untouched");
        }
    }
    check_friend_ids();
    printf("t_friends: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
