// friends service unit test: presence is accepted (Twelve Minutes stops its start-up on a failed UserPresence::Initialize, seedds/nxnative#217) and the notification queue opens
// empty (Beach Buggy Racing 2 stops its start-up on a failed NotificationQueue::Initialize),
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
    CHECK(h_fr_NqInit(presence, uid) == RES_OK, "NotificationQueue::Initialize succeeds (Beach Buggy Racing 2 stops its start-up otherwise)");
    uint8_t info[0x200] = {0};
    CHECK(h_fr_NqPop(presence, info) != RES_OK, "NotificationQueue::Pop finds no notification");
    CHECK(h_fr_OpenSession(uid) == RES_FAIL, "DeclareOpenOnlinePlaySession still fails");
    // Resolve the actual takeovers: a missing or misspelled overload would run the SDK's friends client instead (PR #2162, issue #3177).
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
    // Blocked-user list availability, no-Uid forms, and methods on cut objects (issue #3177).
    uint8_t ctx[0x40], obj[0x40], ids[16] = {2};
    memset(ctx, 0x5a, sizeof ctx);
    memset(obj, 0x5a, sizeof obj);
    // AsyncContext::Cancel is the Nextendo provider's (dm_nextendo_async_cancel), a wait entry: a context it never started does not cancel
    Result (*up_init_nouid)(void *) = hook("_ZN2nn7friends12UserPresence10InitializeEv") ? h_fr_UpInitOpen : NULL;   // a wait entry: the hook itself
    if (up_init_nouid) CHECK(up_init_nouid(presence) == RES_OK, "UserPresence::Initialize without Uid succeeds");
    Result (*open_nouid)(void) = hook("_ZN2nn7friends28DeclareOpenOnlinePlaySessionEv");
    Result (*close_nouid)(void) = hook("_ZN2nn7friends29DeclareCloseOnlinePlaySessionEv");
    if (open_nouid) CHECK(open_nouid() == RES_FAIL, "DeclareOpenOnlinePlaySession without Uid fails");
    if (close_nouid) CHECK(close_nouid() == RES_FAIL, "DeclareCloseOnlinePlaySession without Uid fails");
    Result (*prof_nouid)(void *, void *, const void *, int) =
        hook("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileEPKNS_7account23NetworkServiceAccountIdEi");
    if (prof_nouid) {
        CHECK(prof_nouid(ctx, obj, ids, 1) == RES_FAIL, "GetProfileList without Uid fails");
        CHECK(!memcmp(obj, ctx, sizeof obj), "failed GetProfileList leaves output untouched");
    }
    Result (*upd_nouid)(void *, const void *, int) =
        hook("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendEPKNS_7account23NetworkServiceAccountIdEi");
    if (upd_nouid) {
        CHECK(upd_nouid(obj, ids, 1) == RES_FAIL, "UpdateFriendInfo without Uid fails");
        CHECK(!memcmp(obj, ctx, sizeof obj), "failed UpdateFriendInfo leaves friend untouched");
    }
    Result (*fr_upd)(void *) = hook("_ZN2nn7friends6Friend6UpdateEv");
    if (fr_upd) CHECK(fr_upd(obj) == RES_FAIL, "Friend::Update fails");
    Result (*nq_init_nouid)(void *) = hook("_ZN2nn7friends17NotificationQueue10InitializeEv");
    Result (*nq_clear)(void *) = hook("_ZN2nn7friends17NotificationQueue5ClearEv");
    void (*nq_fini)(void *) = hook("_ZN2nn7friends17NotificationQueue8FinalizeEv");
    if (nq_init_nouid) CHECK(nq_init_nouid(obj) == RES_OK, "NotificationQueue::Initialize without Uid succeeds");
    if (nq_clear) CHECK(nq_clear(obj) == RES_OK, "NotificationQueue::Clear succeeds on the empty queue");
    if (nq_fini) { nq_fini(obj); CHECK(!memcmp(obj, ctx, sizeof obj), "NotificationQueue::Finalize leaves cut object untouched"); }
    Result (*fl_ensure_nouid)(void *) = hook("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextE");
    Result (*fl_avail_nouid)(uint8_t *) = hook("_ZN2nn7friends27CheckFriendListAvailabilityEPb");
    if (fl_ensure_nouid) CHECK(fl_ensure_nouid(ctx) == RES_FAIL, "EnsureFriendListAvailable without Uid fails");
    if (fl_avail_nouid) {
        uint8_t avail = 1;
        CHECK(fl_avail_nouid(&avail) == RES_OK && avail == 0, "CheckFriendListAvailability without Uid reports unavailable");
        CHECK(fl_avail_nouid(NULL) == RES_OK, "CheckFriendListAvailability without Uid tolerates null");
    }
    Result (*bl_ensure_nouid)(void *) = hook("_ZN2nn7friends30EnsureBlockedUserListAvailableEPNS0_12AsyncContextE");
    Result (*bl_ensure)(void *, const void *) = hook("_ZN2nn7friends30EnsureBlockedUserListAvailableEPNS0_12AsyncContextERKNS_7account3UidE");
    Result (*bl_avail_nouid)(uint8_t *) = hook("_ZN2nn7friends32CheckBlockedUserListAvailabilityEPb");
    Result (*bl_avail)(uint8_t *, const void *) = hook("_ZN2nn7friends32CheckBlockedUserListAvailabilityEPbRKNS_7account3UidE");
    if (bl_ensure_nouid) CHECK(bl_ensure_nouid(ctx) == RES_FAIL, "EnsureBlockedUserListAvailable without Uid fails");
    if (bl_ensure) CHECK(bl_ensure(ctx, uid) == RES_FAIL, "EnsureBlockedUserListAvailable with Uid fails");
    if (bl_avail_nouid) {
        uint8_t avail = 1;
        CHECK(bl_avail_nouid(&avail) == RES_OK && avail == 0, "CheckBlockedUserListAvailability without Uid reports unavailable");
        CHECK(bl_avail_nouid(NULL) == RES_OK, "CheckBlockedUserListAvailability without Uid tolerates null");
    }
    if (bl_avail) {
        uint8_t avail = 1;
        CHECK(bl_avail(&avail, uid) == RES_OK && avail == 0, "CheckBlockedUserListAvailability with Uid reports unavailable");
        CHECK(bl_avail(NULL, uid) == RES_OK, "CheckBlockedUserListAvailability with Uid tolerates null");
    }
    printf("t_friends: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
