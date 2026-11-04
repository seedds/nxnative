// nn::friends (friends / presence / invitations, out of scope): every interface that connects to the friends service is cut to "unavailable".
// Not cut: the getters of Friend / Profile / FriendPresence and the constructors of Friend / Profile / UserPresence (they only read and write the game's own objects and do not go through the service).
// Return convention: Result returns RES_FAIL; bool returns false; event pointers get a SystemEvent that is bound to a kernel event and never fires.
#include "hle.h"
#include "account_local.h"
#include "nextendo_account.h"
#include "nextendo_social.h"
#include "net_socket.h"
#include "friends_history.h"

// Notification event that never fires (GetSystemEvent-style calls return a pointer; bound on the guest thread on first use)
static _Alignas(16) uint8_t g_ev_notify[0x100], g_ev_invite[0x100];
static void *quiet_event(uint8_t *ev) { hle_bind_system_event(ev, 0); return ev; }

HLE_FORCE("_ZN2nn7friends10InitializeEv", "connects to the friends service (cut)", void, h_fr_Initialize, void) {}
// AsyncContext: no request is sent; the completion event fires immediately and the result is failure (same as hle_account.c)
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextC1Ev", "owned friends request state", void, h_fr_AcCtor, void *self) { dm_nextendo_async_construct(self); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextD1Ev", "release friends request state", void, h_fr_AcDtor, void *self) { dm_nextendo_async_destroy(self); }
HLE_FORCE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "actual friends request completion event", Result, h_fr_AcGetEv, void *self, void *ev) {
    uint64_t lifetime=dm_nextendo_async_lifetime(self); if (!ev || !lifetime) return RES_FAIL;
    hle_bind_system_event(ev,0); return dm_nextendo_async_attach(self,lifetime,hle_system_event_binding(ev));
}
HLE_FORCE_WAIT("_ZNK2nn7friends12AsyncContext9GetResultEv", "actual friends request result", Result, h_fr_AcResult, const void *self) { return dm_nextendo_async_result((void *)self); }
// Lists / profiles / play history
HLE_FORCE_WAIT("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", "real provider friend snapshot in the verified SDK object layout", Result, h_fr_GetFriendList, int *count, void *out, int off, int n, const void *filt) {
    return dm_nextendo_friends_list(count,out,NULL,off,n,filt,false);
}
// Blocked users, SDK 4's overload without a Uid: an empty list and success. Dark Souls Remastered asks after the opening movie and turns a failure into an error for the error applet
// (main+0x8ff9f0). The Uid overload below reports a failure, which DRAGON QUEST XI S takes as it is
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdEii", "blocked user list is empty (cut)", Result, h_fr_GetBlocked4, int *count, void *out, int off, int n) {
    (void)out; (void)off; (void)n; if (count) *count = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", "profile query fails (cut)",
          Result, h_fr_GetProfileList, void *ctx, void *out, const void *uid, const void *ids, int n) { (void)ctx; (void)out; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE_WAIT("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", "refresh the requested actual friend records and their application-presence fields",
          Result, h_fr_UpdateFriendInfo, void *f, const void *uid, const uint64_t *ids, int n) { return dm_nextendo_friends_update(f,uid,ids,n); }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", "play history is not registered (cut)",
          Result, h_fr_AddPlayHistory, const void *k, const void *a, const void *b) { (void)k; (void)a; (void)b; return RES_FAIL; }
HLE_FORCE_WAIT("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", "local UUID play-history key signed with the privately imported firmware keyring; no linked Nintendo NSA ID",
          Result, h_fr_GetPHKey, void *key, const void *uid, unsigned input_flag) {
    if (!key) return RES_FAIL;
    if (!dm_account_uid_exists(uid)) { memset(key, 0, DM_FRIENDS_HISTORY_KEY_SIZE); return DM_FRIENDS_HISTORY_INVALID_UID; }
    return dm_friends_history_key(key, uid, input_flag);
}
// Notification queue
HLE_FORCE("_ZN2nn7friends17NotificationQueueC1Ev", "notification queue creates no session (cut)", void, h_fr_NqCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends17NotificationQueueD1Ev", "notification queue has no session to close (cut)", void, h_fr_NqDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", "notification queue init fails (cut)", Result, h_fr_NqInit, void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
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
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", "blocked user list is empty (cut)",
          Result, h_fr_GetBlocked, int *count, void *out, const void *uid, int off, int n) { (void)out; (void)uid; (void)off; (void)n; if (count) *count = 0; return RES_FAIL; }
// Online play sessions (DRAGON QUEST XI S)
HLE_FORCE_WAIT("_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", "publish this user's actual online-play session", Result, h_fr_OpenSession, const void *uid) { return dm_net_nextendo_enabled()&&dm_account_uid_exists(uid)?dm_nextendo_social_publish(2,NULL,0):RES_FAIL; }
HLE_FORCE_WAIT("_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", "publish closure of this user's online-play session", Result, h_fr_CloseSession, const void *uid) { return dm_net_nextendo_enabled()&&dm_account_uid_exists(uid)?dm_nextendo_social_publish(1,NULL,0):RES_FAIL; }
// Presence
HLE_FORCE_WAIT("_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", "initialize the SDK presence object for the selected local user", Result, h_fr_UpInit, void *self, const void *uid) { return dm_nextendo_presence_initialize(self,uid); }
HLE_FORCE("_ZN2nn7friends12UserPresence14SetDescriptionEPKc", "presence unavailable (cut)", Result, h_fr_UpDesc, void *self, const char *s) { (void)self; (void)s; return RES_FAIL; }
HLE_FORCE_WAIT("_ZN2nn7friends12UserPresence6CommitEv", "publish the actual SDK application-presence field", Result, h_fr_UpCommit, void *self) { return dm_nextendo_presence_commit(self); }

// Additional SDK service overloads use the same provider and object lifetime.
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextC2Ev", "owned friends request state", void, h_fr_AcCtor2, void *self) { dm_nextendo_async_construct(self); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextD2Ev", "release friends request state", void, h_fr_AcDtor2, void *self) { dm_nextendo_async_destroy(self); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContext9GetResultEv", "actual friends request result", Result, h_fr_AcResultMutable, void *self) { return dm_nextendo_async_result(self); }
HLE_FORCE_WAIT("_ZNK2nn7friends12AsyncContext7HasDoneEPb", "actual friends request completion", Result, h_fr_AcHasDoneConst, void *self,uint8_t *done) { return dm_nextendo_async_done(self,done); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContext6CancelEv", "cancel a friends request without publishing a late result", Result, h_fr_AcCancel, void *self) { return dm_nextendo_async_cancel(self); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextaSEOS1_", "move friends request state and its event lifetime", void *, h_fr_AcMove,void *self,void *other) { return dm_nextendo_async_move(self,other); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextC1EOS1_", "move-construct friends request state", void, h_fr_AcMoveCtor,void *self,void *other) { dm_nextendo_async_move(self,other); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContextC2EOS1_", "move-construct friends request state", void, h_fr_AcMoveCtor2,void *self,void *other) { dm_nextendo_async_move(self,other); }
HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContext4SwapERS1_", "swap owned friends request states", void, h_fr_AcSwap,void *self,void *other) { dm_nextendo_async_swap(self,other); }
HLE_FORCE_WAIT("_ZNK2nn7friends12AsyncContextcvbEv", "whether a friends context owns an operation", unsigned, h_fr_AcValid,void *self) { return dm_nextendo_async_valid(self); }
HLE_FORCE("_ZN2nn7friends12AsyncContextD0Ev", "release friends request state and its guest allocation", void, h_fr_AcDeletingDtor,void *self) {
    dm_nextendo_async_destroy(self); void (*release)(void *)=(void (*)(void *))guest_sym("_ZdlPv");
    if (release) release(self); else LOG("[RED] friends: guest operator delete unavailable for AsyncContext");
}
HLE_FORCE_WAIT("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE", "real provider friend identities", Result, h_fr_GetFriendIds,int *count,void *out,int off,int n,const void *filter) { return dm_nextendo_friends_list(count,out,NULL,off,n,filter,true); }
HLE_FORCE_WAIT("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEiiRKNS0_12FriendFilterE", "real provider friend identities for the selected local user", Result, h_fr_GetFriendIdsUid,int *count,void *out,const void *uid,int off,int n,const void *filter) { return dm_nextendo_friends_list(count,out,uid,off,n,filter,true); }
HLE_FORCE_WAIT("_ZN2nn7friends12UserPresence10InitializeEv", "initialize the SDK presence object for the open local user", Result, h_fr_UpInitOpen,void *self) { return dm_nextendo_presence_initialize(self,NULL); }

HLE_FORCE_WAIT("_ZN2nn7friends12AsyncContext7HasDoneEPb", "actual friends request completion", Result, h_fr_AcHasDone, void *self,uint8_t *done) { return dm_nextendo_async_done(self,done); }

HLE_FORCE_WAIT("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "availability follows the real provider friend snapshot", Result, h_fr_ListAvail, uint8_t *out,const void *uid) { return dm_nextendo_friends_available(out,uid); }

HLE_FORCE_WAIT("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "fetch the actual friend snapshot and publish its completion result", Result, h_fr_ListEnsure,void *ctx,const void *uid) {
    if (!dm_account_uid_exists(uid)) return RES_FAIL;
    return dm_nextendo_async_execute(ctx,dm_nextendo_friends_ensure,uid);
}

HLE_FORCE_WAIT("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "real provider friend snapshot for the selected local user", Result, h_fr_GetFriendListUid, int *count,void *out,const void *uid,int off,int n,const void *filter) { return dm_nextendo_friends_list(count,out,uid,off,n,filter,false); }
