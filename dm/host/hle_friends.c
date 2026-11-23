// nn::friends (friends / presence / invitations, out of scope): every interface that connects to the friends service is cut to "unavailable".
// Not cut: the getters of Friend / Profile / FriendPresence and the constructors of Friend / Profile / UserPresence (they only read and write the game's own objects and do not go through the service).
// Unless stated otherwise, Result returns RES_FAIL; bool returns false; event pointers get a SystemEvent that is bound to a kernel event and never fires.
#include "hle.h"
#include "nextendo_client.h"
#include <pthread.h>

// Notification event that never fires (GetSystemEvent-style calls return a pointer; bound on the guest thread on first use)
static _Alignas(16) uint8_t g_ev_notify[0x100], g_ev_invite[0x100];
static void *quiet_event(uint8_t *ev) { hle_bind_system_event(ev, 0); return ev; }

enum { FR_ASYNC_MAX = 32, FR_ENTRY_SIZE = 0x200 };
static pthread_mutex_t g_fr_mu = PTHREAD_MUTEX_INITIALIZER;
static void *g_fr_async_ok[FR_ASYNC_MAX];
static char g_fr_presence_desc[192];

void dm_friends_async_set(void *ctx, int ok) {
    if (!ctx) return;
    pthread_mutex_lock(&g_fr_mu);
    int free_slot = -1;
    for (int i = 0; i < FR_ASYNC_MAX; i++) {
        if (g_fr_async_ok[i] == ctx) { if (!ok) g_fr_async_ok[i] = NULL; pthread_mutex_unlock(&g_fr_mu); return; }
        if (!g_fr_async_ok[i] && free_slot < 0) free_slot = i;
    }
    if (ok) g_fr_async_ok[free_slot >= 0 ? free_slot : 0] = ctx;
    pthread_mutex_unlock(&g_fr_mu);
}
static int fr_async_is_ok(const void *ctx) {
    if (!ctx) return 0;
    pthread_mutex_lock(&g_fr_mu);
    int found = 0;
    for (int i = 0; i < FR_ASYNC_MAX; i++) if (g_fr_async_ok[i] == ctx) { found = 1; break; }
    pthread_mutex_unlock(&g_fr_mu);
    return found;
}

// Fill nn::friends::Friend (0x200 bytes each) from Nextendo cache, applying FriendFilter before slicing.
Result dm_friends_get_list_online(int *count, void *out, int off, int n, const void *filt) {
    if (count) *count = 0;
    if (!dm_nextendo_enabled() || !dm_nextendo_is_linked()) return RES_FAIL;
    dm_nextendo_friend all[DM_NEXTENDO_MAX_FRIENDS];
    int total = dm_nextendo_get_friends(all, DM_NEXTENDO_MAX_FRIENDS);
    uint32_t pres_filter = 0;
    uint8_t fav_only = 0;
    if (filt) {
        memcpy(&pres_filter, filt, sizeof pres_filter);
        fav_only = ((const uint8_t *)filt)[4];
    }
    int matched = 0, written = 0;
    for (int i = 0; i < total; i++) {
        const dm_nextendo_friend *f = &all[i];
        if (fav_only && !f->favorite) continue;
        if (pres_filter == 1 && f->online_status == 0) continue;
        if (pres_filter == 2 && f->online_status != 2) continue;
        if ((pres_filter == 3 || pres_filter == 4) && f->app_id != DM_GAME_PROGRAM_ID) continue;
        if (pres_filter == 4 && f->online_status != 2) continue;
        if (matched++ < off) continue;
        if (out && written < n) {
            uint8_t *dst = (uint8_t *)out + (size_t)written * FR_ENTRY_SIZE;
            memset(dst, 0, FR_ENTRY_SIZE);
            memcpy(dst + 0x00, &f->pid, sizeof f->pid);
            strlcpy((char *)(dst + 0x08), f->name, 33);
            memcpy(dst + 0x38, &f->online_status, sizeof f->online_status);
            memcpy(dst + 0x40, &f->app_id, sizeof f->app_id);
            if (f->app_field_len) {
                size_t fl = f->app_field_len > DM_NEXTENDO_APP_FIELD_SIZE ? DM_NEXTENDO_APP_FIELD_SIZE : f->app_field_len;
                memcpy(dst + 0x50, f->app_field, fl);
            }
            dst[0x1f0] = (uint8_t)(f->favorite ? 1 : 0);
            written++;
        }
    }
    if (count) *count = written;
    return RES_OK;
}

Result dm_friends_get_ids_online(int *count, uint64_t *out, int off, int n, const void *filt) {
    if (count) *count = 0;
    if (!dm_nextendo_enabled() || !dm_nextendo_is_linked()) return RES_FAIL;
    dm_nextendo_friend all[DM_NEXTENDO_MAX_FRIENDS];
    int total = dm_nextendo_get_friends(all, DM_NEXTENDO_MAX_FRIENDS);
    uint32_t pres_filter = 0;
    uint8_t fav_only = 0;
    if (filt) {
        memcpy(&pres_filter, filt, sizeof pres_filter);
        fav_only = ((const uint8_t *)filt)[4];
    }
    int matched = 0, written = 0;
    for (int i = 0; i < total; i++) {
        const dm_nextendo_friend *f = &all[i];
        if (fav_only && !f->favorite) continue;
        if (pres_filter == 1 && f->online_status == 0) continue;
        if (pres_filter == 2 && f->online_status != 2) continue;
        if ((pres_filter == 3 || pres_filter == 4) && f->app_id != DM_GAME_PROGRAM_ID) continue;
        if (pres_filter == 4 && f->online_status != 2) continue;
        if (matched++ < off) continue;
        if (out && written < n) out[written++] = f->pid;
    }
    if (count) *count = written;
    return RES_OK;
}

HLE_FORCE("_ZN2nn7friends10InitializeEv", "connects to the friends service (cut)", void, h_fr_Initialize, void) {}
// AsyncContext: no request is sent offline; when DM_NEXTENDO=1, succeeded async contexts return RES_OK
HLE_FORCE("_ZN2nn7friends12AsyncContextC1Ev", "async context creates no session (cut)", void, h_fr_AcCtor, void *self) { dm_friends_async_set(self, 0); }
HLE_FORCE("_ZN2nn7friends12AsyncContextD1Ev", "async context has no session to close (cut)", void, h_fr_AcDtor, void *self) { dm_friends_async_set(self, 0); }
HLE_FORCE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "completion event fires immediately (cut)", Result, h_fr_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZNK2nn7friends12AsyncContext9GetResultEv", "async result fails offline; succeeds when Nextendo async succeeded", Result, h_fr_AcResult, const void *self) {
    return (dm_nextendo_enabled() && fr_async_is_ok(self)) ? RES_OK : RES_FAIL;
}
// Lists / profiles / play history
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", "friend list: empty/fails offline; Nextendo friends when DM_NEXTENDO=1", Result, h_fr_GetFriendList, int *count, void *out, int off, int n, const void *filt) {
    if (dm_nextendo_enabled()) return dm_friends_get_list_online(count, out, off, n, filt);
    (void)out; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL;
}
// Blocked users, SDK 4's overload without a Uid: an empty list and success. Dark Souls Remastered asks after the opening movie and turns a failure into an error for the error applet
// (main+0x8ff9f0). The Uid overload below reports a failure, which DRAGON QUEST XI S takes as it is
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdEii", "blocked user list is empty (cut)", Result, h_fr_GetBlocked4, int *count, void *out, int off, int n) {
    (void)out; (void)off; (void)n; if (count) *count = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", "profile query fails offline; succeeds when DM_NEXTENDO=1",
          Result, h_fr_GetProfileList, void *ctx, void *out, const void *uid, const void *ids, int n) {
    (void)uid; (void)ids;
    if (dm_nextendo_enabled() && dm_nextendo_is_linked()) {
        if (out && n > 0) memset(out, 0, (size_t)n * 0x100u);
        dm_friends_async_set(ctx, 1);
        return RES_OK;
    }
    (void)ctx; (void)out; (void)n; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", "friend info update fails offline; succeeds when DM_NEXTENDO=1",
          Result, h_fr_UpdateFriendInfo, void *f, const void *uid, const void *ids, int n) {
    (void)f; (void)uid; (void)ids; (void)n;
    return (dm_nextendo_enabled() && dm_nextendo_is_linked()) ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", "play history fails offline; accepted when DM_NEXTENDO=1",
          Result, h_fr_AddPlayHistory, const void *k, const void *a, const void *b) {
    (void)k; (void)a; (void)b;
    return (dm_nextendo_enabled() && dm_nextendo_is_linked()) ? RES_OK : RES_FAIL;
}
static Result fr_fill_play_history_key(void *k) {
    if (!dm_nextendo_enabled() || !dm_nextendo_is_linked()) return RES_FAIL;
    if (k) {
        memset(k, 0, 64);
        uint64_t pid = dm_nextendo_get_pid();
        memcpy(k, &pid, sizeof pid);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", "play history key unavailable offline; synthesized when DM_NEXTENDO=1",
          Result, h_fr_GetPHKey, void *k, const void *uid, int local) { (void)uid; (void)local; return fr_fill_play_history_key(k); }
// The form without a Uid (SDK 20; Super Nintendo Entertainment System – Nintendo Classics 6.0.0 asks first thing in nnMain, main+0xc608): the SDK's own opens the friends service and
// aborts with 0xe15 when it cannot; cut the same way as the Uid form
HLE_FORCE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyEb", "play history key unavailable offline; synthesized when DM_NEXTENDO=1",
          Result, h_fr_GetPHKeyNoUid, void *k, int local) { (void)local; return fr_fill_play_history_key(k); }
// Notification queue
HLE_FORCE("_ZN2nn7friends17NotificationQueueC1Ev", "notification queue creates no session (cut)", void, h_fr_NqCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends17NotificationQueueD1Ev", "notification queue has no session to close (cut)", void, h_fr_NqDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", "notification queue init fails offline; succeeds when DM_NEXTENDO=1", Result, h_fr_NqInit, void *self, const void *uid) {
    (void)self; (void)uid;
    return (dm_nextendo_enabled() && dm_nextendo_is_linked()) ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn7friends17NotificationQueue14GetSystemEventEv", "notification event never fires (cut)", void *, h_fr_NqGetEv, void *self) { (void)self; return quiet_event(g_ev_notify); }
HLE_FORCE("_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE", "notification queue is always empty (cut)", Result, h_fr_NqPop, void *self, void *info) { (void)self; (void)info; return RES_FAIL; }
// Invitations
HLE_FORCE("_ZN2nn7friends36GetFriendInvitationNotificationEventEv", "invitation notification event never fires (cut)", void *, h_fr_InvEv, void) { return quiet_event(g_ev_invite); }
HLE_FORCE("_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", "no invitations (cut)", int, h_fr_InvPop, void *uid, uint64_t *sz, void *buf, size_t n) {
    (void)uid; (void)sz; (void)buf; (void)n; return 0;
}
HLE_FORCE("_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", "invitations unavailable (cut)",
          Result, h_fr_StartInv, const void *uid, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", "invitations unavailable (cut)",
          Result, h_fr_SendInv, const void *uid, const void *ids, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)ids; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", "friend detail applet (cut)",
          Result, h_fr_ShowDetail, const void *uid, const void *id, const void *a, const void *b) { (void)uid; (void)id; (void)a; (void)b; return RES_FAIL; }
// Friend list applets (Engage's friends menu): the original starts an applet, the host has no applet proxy
HLE_FORCE("_ZN2nn7friends14ShowFriendListERKNS_7account3UidE", "friend list applet (cut)", Result, h_fr_ShowList, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends19ShowBlockedUserListERKNS_7account3UidE", "blocked user list applet (cut)", Result, h_fr_ShowBlocked, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends29ShowReceivedFriendRequestListERKNS_7account3UidE", "received friend request applet (cut)", Result, h_fr_ShowRecv, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends23StartFacedFriendRequestERKNS_7account3UidE", "in-person friend request applet (cut)", Result, h_fr_StartFaced, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", "send friend request applet (cut)",
          Result, h_fr_StartSend, const void *uid, const void *nsa, const void *mine, const void *theirs) { (void)uid; (void)nsa; (void)mine; (void)theirs; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends33ShowMethodsOfSendingFriendRequestERKNS_7account3UidE", "send friend request applet (cut)", Result, h_fr_ShowSend, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", "blocked user list is empty (cut offline, OK online)",
          Result, h_fr_GetBlocked, int *count, void *out, const void *uid, int off, int n) {
    (void)out; (void)uid; (void)off; (void)n; if (count) *count = 0;
    return (dm_nextendo_enabled() && dm_nextendo_is_linked()) ? RES_OK : RES_FAIL;
}
// Online play sessions (DRAGON QUEST XI S)
HLE_FORCE("_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", "online play session: unavailable offline; updates Nextendo presence when DM_NEXTENDO=1", Result, h_fr_OpenSession, const void *uid) {
    (void)uid;
    if (dm_nextendo_enabled() && dm_nextendo_is_linked()) {
        (void)dm_nextendo_update_presence(2, NULL, 0, g_fr_presence_desc);
        return RES_OK;
    }
    return RES_FAIL;
}
HLE_FORCE("_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", "online play session: unavailable offline; updates Nextendo presence when DM_NEXTENDO=1", Result, h_fr_CloseSession, const void *uid) {
    (void)uid;
    if (dm_nextendo_enabled() && dm_nextendo_is_linked()) {
        (void)dm_nextendo_update_presence(1, NULL, 0, "");
        return RES_OK;
    }
    return RES_FAIL;
}
// A user's own presence succeeds offline. When DM_NEXTENDO=1, Commit also publishes presence to Nextendo.
// Returning failure here can stop a game's startup; see services.md, "Friends presence is accepted offline".
HLE_FORCE("_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", "presence accepted, published when DM_NEXTENDO=1", Result, h_fr_UpInit, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn7friends12UserPresence14SetDescriptionEPKc", "presence description stored, published on Commit when DM_NEXTENDO=1", Result, h_fr_UpDesc, void *self, const char *s) {
    (void)self;
    if (s) {
        pthread_mutex_lock(&g_fr_mu);
        strlcpy(g_fr_presence_desc, s, sizeof g_fr_presence_desc);
        pthread_mutex_unlock(&g_fr_mu);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7friends12UserPresence6CommitEv", "presence accepted, published when DM_NEXTENDO=1", Result, h_fr_UpCommit, void *self) {
    if (dm_nextendo_enabled() && dm_nextendo_is_linked()) {
        char desc[192];
        pthread_mutex_lock(&g_fr_mu);
        strlcpy(desc, g_fr_presence_desc, sizeof desc);
        pthread_mutex_unlock(&g_fr_mu);
        // nn::friends::UserPresence stores the 0xC0-byte app_field at +0x20 when populated.
        const uint8_t *app_field = self ? ((const uint8_t *)self + 0x20) : NULL;
        (void)dm_nextendo_update_presence(2, app_field, app_field ? DM_NEXTENDO_APP_FIELD_SIZE : 0, desc);
    }
    return RES_OK;
}
