// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user.
// Online service identity requires an explicit provider and an actual unexpired NEX proof.
// Nintendo Account authorization remains unavailable; OAuth is not a game credential.
// Environment switch: DM_NICK=nickname (default Player).
#include "hle.h"
#include "account_local.h"
#include "nextendo_account.h"

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // Non-zero means valid
int dm_account_uid_exists(const void *uid) { return uid && !memcmp(uid, &k_uid, sizeof k_uid); }
int dm_account_copy_uid(void *output) { if (!output) return 0; memcpy(output, &k_uid, sizeof k_uid); return 1; }
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"
static int online_user(const UserHandle *h) { return h && h->v[2] == HANDLE_MAGIC && h->v[0] == k_uid.v[0] && h->v[1] == k_uid.v[1]; }

HLE_FORCE("_ZN2nn7account10InitializeEv", "connects to the acc:u0 account service; the host has one fixed local user", void, h_acc_Init, void) {}

HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "list of open users; always the one local user", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    *n = 0;
    if (max > 0) { out[0] = k_uid; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "list of all users; always the one local user (Super Mario 3D World + Bowser's Fury)", Result, h_acc_ListAll, int *n, Uid *out, int max) {
    *n = 0;
    if (max > 0) { out[0] = k_uid; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "user selection applet; the host picks the local user directly", Result, h_acc_ShowSel, Uid *out, const void *settings) { DM_APPLET("user selector");
    (void)settings; *out = k_uid; return RES_OK;
}
// Take the users the system opened on the application's behalf (Mario 1.2.1 asks this first and falls back to TryOpenPreselectedUser on 0; the game only reads the count, not the return value):
// the host has no such users and returns 0, so the game opens the preselected user itself, as in v0
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "users opened by the system: none (the game falls back to the preselected user)", void, h_acc_PopOpen, int *n, UserHandle *out, int max) { (void)out; (void)max; *n = 0; }
// Hand the open users to the next program of a multi-program application (Octopath Traveler 0 imports it with oe::ExecuteProgram): the host runs one program, so there is nothing to hand over
HLE_FORCE("_ZN2nn7account13PushOpenUsersEv", "hand the open users to the next program: the host runs only this one", void, h_acc_PushOpen, void) {}
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "user preselected at launch; always the local user (returns bool)", unsigned, h_acc_TryOpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return 1;
}
// The aborting form (Super Mario Odyssey, SDK 7): without the hook the SDK's IPC call fails and aborts with no message. In SDK 6 the same symbol returns a Result, and Fire Emblem: Three Houses
// skips its save mount when it is non-zero (main+0x53b7e0); returning void left the handle pointer in w0, so the game never mounted "save" and its loading screen waited forever for save:/system.
// It returns 0 now, which SDK 7+ callers ignore
HLE_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "user preselected at launch (returns a Result in SDK 6, later versions abort on failure); always the local user", Result, h_acc_OpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", "last opened user; always the local user (Super Mario Odyssey)", Result, h_acc_LastOpened, Uid *out) { *out = k_uid; return RES_OK; }
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "open user; the handle holds the Uid itself", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) {
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "gets the Uid from the handle", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    if (h->v[2] != HANDLE_MAGIC) die("account::GetUserId: the handle was not issued by the host (%#llx)", h->v[2]);
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
// nn::account::Nickname = struct { char name[33]; }, UTF-8
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "user nickname; taken from DM_NICK", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    (void)uid;
    const char *e = getenv("DM_NICK");
    memset(nick, 0, 33); strncpy(nick, e ? e : "Player", 32);
    return RES_OK;
}

// ---------- Online accounts: cut ----------
HLE_FORCE_WAIT("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "network service account requires the user's actual unexpired Nextendo NEX proof", Result, h_acc_EnsureNsa, const UserHandle *h) { return online_user(h) ? dm_nextendo_account_ensure() : RES_FAIL; }
HLE_FORCE_WAIT("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "real provider PID or required server-reported BAAS identity", Result, h_acc_GetNsaId, uint64_t *id, const UserHandle *h) { if (id) *id = 0; return online_user(h) ? dm_nextendo_account_id(id) : RES_FAIL; }
HLE_FORCE_WAIT("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "real NEX proof in the native game's cached account token", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    if (n) *n = 0; return online_user(h) ? dm_nextendo_account_load_token(n, buf, sz) : RES_FAIL;
}
HLE_FORCE("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", "online membership notice applet, out of scope; cut", Result, h_acc_ShowLic, const UserHandle *h) { (void)h; return RES_FAIL; }
// The Nintendo Account's cached profile (birthday, country): no Nintendo Account is linked on the host, so loading fails and the game skips its getters (Big Brain Academy: Brain vs. Brain, main+0xab8bc)
HLE_FORCE("_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm", "cached Nintendo Account info: none linked, fails", Result, h_acc_LoadNaInfo,
          void *info, const UserHandle *h, void *buf, uint64_t sz) { (void)info; (void)h; (void)buf; (void)sz; return RES_FAIL; }

// AsyncContext / NintendoAccountAuthorizationRequestContext: object contents are read and written only by the interfaces below; construction writes no memory, the completion event fires immediately, and the result is always failure
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextC1Ev", "owned account async state", void, h_acc_AcCtor, void *self) { dm_nextendo_async_construct(self); }
HLE_FORCE_WAIT("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "cache the real Nextendo NEX game token and publish its actual completion result", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { return online_user(h) ? dm_nextendo_async_ensure(ctx) : RES_FAIL; }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "account completion event with a checked kernel-event lifetime", Result, h_acc_AcGetEv, void *self, void *ev) {
    uint64_t lifetime = dm_nextendo_async_lifetime(self); if (!ev || !lifetime) return RES_FAIL;
    hle_bind_system_event(ev, 0); return dm_nextendo_async_attach(self, lifetime, hle_system_event_binding(ev));
}
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext9GetResultEv", "actual completed account request result", Result, h_acc_AcGetRes, void *self) { return dm_nextendo_async_result(self); }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev", "Nintendo Account authorization, out of scope; the constructor writes no memory", void, h_acc_NaCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", "Nintendo Account authorization, out of scope", void, h_acc_NaDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",
          "Nintendo Account authorization, out of scope; cut", Result, h_acc_NaCreate, void *ctx, const UserHandle *h, const void *prm, void *buf, uint64_t sz) {
    (void)ctx; (void)h; (void)prm; (void)buf; (void)sz; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE", "authorization completion event; fires immediately", Result, h_acc_NaGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv", "authorization result; always fails", Result, h_acc_NaGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm", "authorization token; always fails", Result, h_acc_NaGetTok, void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext20GetAuthorizationCodeEPmPcm", "authorization code; always fails", Result, h_acc_NaGetCode, void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext8GetStateEPmPcm", "authorization state string; always fails", Result, h_acc_NaGetState, void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}

// ---------- Account interfaces used by newer SDKs: user enumeration / existence / profile image, the remaining AsyncContext members ----------
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "user count; always the one local user", Result, h_acc_UserCount, int *n) { *n = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "list of qualified users; always the one local user", Result, h_acc_ListQual, int *n, Uid *out, int max) {
    int k = 0; if (max > 0 && out) { out[0] = k_uid; k = 1; } *n = k; return RES_OK;
}
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "whether a user exists; true for the local user", Result, h_acc_Exists, uint8_t *out, const Uid *uid) {
    *out = uid->v[0] == k_uid.v[0] && uid->v[1] == k_uid.v[1]; return RES_OK;
}
HLE_FORCE("_ZN2nn7account15ShowUserCreatorEv", "user creation applet; the host already has its local user", Result, h_acc_ShowCreator, void) { DM_APPLET("user creator"); return RES_OK; }
HLE_FORCE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", "user profile image; the host has none (fails)", Result, h_acc_LoadImage, uint64_t *n, void *buf, uint64_t sz, const Uid *uid) {
    (void)buf; (void)sz; (void)uid; if (n) *n = 0; return RES_FAIL;
}
// The save-data thumbnail is what the system's data management screen shows next to a save (The Great Ace Attorney Chronicles, SDK 10); the host has no such screen and keeps none
HLE_FORCE("_ZN2nn7account27StoreSaveDataThumbnailImageERKNS0_3UidEPKvm", "save-data thumbnail for the system's data management screen; the host keeps none", Result, h_acc_StoreThumb, const Uid *uid, const void *img, uint64_t n) {
    (void)uid; (void)img; (void)n; return RES_OK;
}
HLE_FORCE("_ZN2nn7account28DeleteSaveDataThumbnailImageERKNS0_3UidE", "save-data thumbnail; the host keeps none", Result, h_acc_DeleteThumb, const Uid *uid) { (void)uid; return RES_OK; }
HLE_FORCE_WAIT("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "only a coherent unexpired Nextendo NEX credential enables the local user's NSA", Result, h_acc_NsaAvail, uint8_t *out, const UserHandle *h) {
    if (!out) return RES_FAIL; *out = 0;
    return online_user(h) ? dm_nextendo_account_available(out) : RES_FAIL;
}
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close user; the handle holds no session", void, h_acc_CloseUser, const UserHandle *h) { (void)h; }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext6CancelEv", "cancel an account request without publishing a late success", Result, h_acc_AcCancel, void *self) { return dm_nextendo_async_cancel(self); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext7HasDoneEPb", "actual account request completion", Result, h_acc_AcHasDone, void *self, uint8_t *done) { return dm_nextendo_async_done(self, done); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextD1Ev", "destroy owned account request state", void, h_acc_AcDtor, void *self) { dm_nextendo_async_destroy(self); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextaSEOS1_", "move account request state and its event lifetimes", void *, h_acc_AcMove, void *self, void *other) { return dm_nextendo_async_move(self, other); }

// Additional SDK service overloads use the same provider and object lifetime.
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextC2Ev", "owned account async state", void, h_acc_AcCtor2, void *self) { dm_nextendo_async_construct(self); }
HLE_FORCE_WAIT("_ZNK2nn7account12AsyncContext9GetResultEv", "actual completed account request result, SDK 19 const overload", Result, h_acc_AcGetResConst, void *self) { return dm_nextendo_async_result(self); }
HLE_FORCE_WAIT("_ZNK2nn7account12AsyncContext7HasDoneEPb", "actual account request completion, SDK 19 const overload", Result, h_acc_AcHasDoneConst, void *self, uint8_t *done) { return dm_nextendo_async_done(self, done); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextD2Ev", "destroy owned account request state", void, h_acc_AcDtor2, void *self) { dm_nextendo_async_destroy(self); }
HLE_FORCE("_ZN2nn7account12AsyncContextD0Ev", "destroy account request state and release its guest allocation", void, h_acc_AcDeletingDtor, void *self) {
    dm_nextendo_async_destroy(self); void (*release)(void *) = (void (*)(void *))guest_sym("_ZdlPv");
    if (release) release(self); else LOG("[RED] account: guest operator delete unavailable for AsyncContext");
}
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextC1EOS1_", "move-construct owned account request state", void, h_acc_AcMoveCtor, void *self, void *other) { dm_nextendo_async_move(self, other); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextC2EOS1_", "move-construct owned account request state", void, h_acc_AcMoveCtor2, void *self, void *other) { dm_nextendo_async_move(self, other); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext4SwapERS1_", "swap owned account request state", void, h_acc_AcSwap, void *self, void *other) { dm_nextendo_async_swap(self, other); }
HLE_FORCE_WAIT("_ZNK2nn7account12AsyncContextcvbEv", "whether an account request owns an actual operation", unsigned, h_acc_AcValid, void *self) { return dm_nextendo_async_valid(self); }
