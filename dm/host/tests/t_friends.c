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
    // Resolve the actual takeovers: a missing or misspelled overload would run the SDK's friends client instead (PR #2162, issue #3177).
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
    // Blocked-user list availability, no-Uid forms, and methods on cut objects (issue #3177).
    uint8_t ctx[0x40], obj[0x40], ids[16] = {2};
    memset(ctx, 0x5a, sizeof ctx);
    memset(obj, 0x5a, sizeof obj);
    Result (*ac_cancel)(void *) = hook("_ZN2nn7friends12AsyncContext6CancelEv");
    if (ac_cancel) CHECK(ac_cancel(ctx) == RES_OK, "AsyncContext::Cancel succeeds");
    Result (*up_init_nouid)(void *) = hook("_ZN2nn7friends12UserPresence10InitializeEv");
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
    if (nq_init_nouid) CHECK(nq_init_nouid(obj) == RES_FAIL, "NotificationQueue::Initialize without Uid fails");
    if (nq_clear) CHECK(nq_clear(obj) == RES_FAIL, "NotificationQueue::Clear fails");
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
