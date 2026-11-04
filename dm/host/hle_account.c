// nn::account local users and open sessions (acc:u0 on Horizon).
// Online accounts (network service account / Nintendo Account authorization / tokens) are out of scope and uniformly return RES_FAIL; the async context's completion event fires immediately.
#include "hle.h"
#include "local_users.h"

typedef dm_user_uid Uid;
typedef struct { Uid uid; uint64_t session; } UserHandle;
_Static_assert(sizeof(Uid) == 16, "Uid");
_Static_assert(sizeof(UserHandle) == 24, "UserHandle");

// A handle may be copied by value. Each open gets a new token so a stale copy
// cannot close an independent open of the same user, even after storage is reused.
typedef struct UserSession {
    uint64_t token;
    const dm_local_user *user;
    struct UserSession *next;
} UserSession;
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
static UserSession *sessions;
static uint64_t next_token = 1;
static const dm_local_user *last_opened;

static Result open_user(UserHandle *h, const dm_local_user *user) {
    UserSession *s = user ? malloc(sizeof *s) : NULL;
    if (!s) { *h = (UserHandle){0}; return RES_FAIL; }
    dm_host_mutex_lock(&session_mutex);
    s->token = next_token++;
    s->user = user;
    s->next = sessions;
    sessions = s;
    last_opened = user;
    *h = (UserHandle){ user->uid, s->token };
    pthread_mutex_unlock(&session_mutex);
    return RES_OK;
}
// Hold session_mutex while looking up a guest handle or removing the returned entry.
static UserSession **find_session(const UserHandle *h) {
    UserSession **link = &sessions;
    while (*link && ((*link)->token != h->session || memcmp(&(*link)->user->uid, &h->uid, sizeof(Uid))))
        link = &(*link)->next;
    return link;
}

static Result list_users(int *n, Uid *out, int max, int open_only) {
    const dm_user_roster *roster = dm_users();
    unsigned mask = ~0u;
    *n = 0;
    if (!roster->count || max < 0 || (max && !out)) return RES_FAIL;
    if (open_only) {
        // The original single-user host advertised its player even before OpenUser.
        mask = roster->configured ? 0 : 1;
        dm_host_mutex_lock(&session_mutex);
        for (const UserSession *s = sessions; s; s = s->next)
            mask |= 1u << (s->user - roster->users);
        pthread_mutex_unlock(&session_mutex);
    }
    for (int i = 0; i < roster->count && *n < max; i++)
        if (mask & (1u << i)) out[(*n)++] = roster->users[i].uid;
    return RES_OK;
}

HLE_FORCE("_ZN2nn7account10InitializeEv", "connects to the local account service", void, h_acc_Init, void) {}
HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "list local users with open sessions", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    return list_users(n, out, max, 1);
}
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "enumerate the local account registry", Result, h_acc_ListAll, int *n, Uid *out, int max) {
    return list_users(n, out, max, 0);
}
// Stable prefix of nn::account::UserSelectionSettings (libnx, switch/applets/psel.h).
typedef struct { Uid excluded[8]; uint8_t skip_enabled, network_required; } SelectionSettings;
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "use the account chosen at launch", Result, h_acc_ShowSel, Uid *out, const void *settings) { DM_APPLET("user selector");
    const dm_local_user *user = dm_selected_user();
    const SelectionSettings *filter = settings;
    *out = (Uid){0};
    if (!user) return RES_FAIL;
    if (dm_users()->configured && filter) {
        if (filter->network_required) return RES_FAIL;
        for (unsigned i = 0; i < 8; i++)
            if (!memcmp(&filter->excluded[i], &user->uid, sizeof(Uid))) return RES_FAIL;
    }
    *out = user->uid;
    return RES_OK;
}
// Take the users the system opened on the application's behalf (Mario 1.2.1 asks this first and falls back to TryOpenPreselectedUser on 0; the game only reads the count, not the return value):
// the host has no such users and returns 0, so the game opens the preselected user itself, as in v0
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "users opened by the system: none (the game falls back to the preselected user)", void, h_acc_PopOpen, int *n, UserHandle *out, int max) { (void)out; (void)max; *n = 0; }
// Hand the open users to the next program of a multi-program application (Octopath Traveler 0 imports it with oe::ExecuteProgram): the host runs one program, so there is nothing to hand over
HLE_FORCE("_ZN2nn7account13PushOpenUsersEv", "hand the open users to the next program: the host runs only this one", void, h_acc_PushOpen, void) {}
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "open the launcher's preselected user (returns bool)", unsigned, h_acc_TryOpenPre, UserHandle *h) {
    return open_user(h, dm_selected_user()) == RES_OK;
}
// The aborting form (Super Mario Odyssey, SDK 7): without the hook the SDK's IPC call fails and aborts with no message. In SDK 6 the same symbol returns a Result, and Fire Emblem: Three Houses
// skips its save mount when it is non-zero (main+0x53b7e0); returning void left the handle pointer in w0, so the game never mounted "save" and its loading screen waited forever for save:/system.
// It returns 0 now, which SDK 7+ callers ignore
HLE_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "open the launcher's preselected user (returns a Result in SDK 6)", Result, h_acc_OpenPre, UserHandle *h) {
    return open_user(h, dm_selected_user());
}
HLE_FORCE("_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", "last user opened, initially the launcher's choice", Result, h_acc_LastOpened, Uid *out) {
    dm_host_mutex_lock(&session_mutex);
    const dm_local_user *user = last_opened;
    pthread_mutex_unlock(&session_mutex);
    if (!user) user = dm_selected_user();
    *out = user ? user->uid : (Uid){0};
    return user ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "open a registered user", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) {
    return open_user(h, dm_user_find(uid));
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "resolve an open account handle", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    dm_host_mutex_lock(&session_mutex);
    const UserSession *s = *find_session(h);
    Result result = s ? RES_OK : RES_FAIL;
    *out = s ? s->user->uid : (Uid){0};
    pthread_mutex_unlock(&session_mutex);
    return result;
}
// nn::account::Nickname = struct { char name[33]; }, UTF-8
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "local profile nickname for this Uid", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    const dm_local_user *user = dm_user_find(uid);
    if (!user) { memset(nick, 0, 33); return RES_FAIL; }
    memcpy(nick, user->nickname, sizeof user->nickname);
    return RES_OK;
}

// ---------- Online accounts: cut ----------
HLE_FORCE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "network service account, out of scope; cut: always unavailable", Result, h_acc_EnsureNsa, const UserHandle *h) { (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "network service account, out of scope; cut", Result, h_acc_GetNsaId, void *id, const UserHandle *h) { (void)id; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "network service account token, out of scope; cut", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    (void)buf; (void)sz; (void)h; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", "online membership notice applet, out of scope; cut", Result, h_acc_ShowLic, const UserHandle *h) { (void)h; return RES_FAIL; }
// The Nintendo Account's cached profile (birthday, country): no Nintendo Account is linked on the host, so loading fails and the game skips its getters (Big Brain Academy: Brain vs. Brain, main+0xab8bc)
HLE_FORCE("_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm", "cached Nintendo Account info: none linked, fails", Result, h_acc_LoadNaInfo,
          void *info, const UserHandle *h, void *buf, uint64_t sz) { (void)info; (void)h; (void)buf; (void)sz; return RES_FAIL; }

// AsyncContext / NintendoAccountAuthorizationRequestContext: no request is sent, the completion event fires immediately, and the result is always failure.
// AsyncContext is built by the SDK's own constructor, which is not taken over: it writes the class's vtable and nulls the IPC object pointer at +0x18, and sends
// nothing (SDK 19.3.11: sdk+0x1ad6c8). No interface below sets that pointer, so the SDK's destructor, reached through the vtable, finds no session to close.
// The object is polymorphic: Pokémon Friends' C# binding deletes it from nn.account.AsyncContext's finalizer through the virtual deleting destructor, inlined
// in main (main+0x5b64f0), which the D1Ev takeover below never sees. A host constructor that wrote no memory left the allocator's bytes in the vtable word,
// and that call jumped through them (docs/reference/services.md, "An account AsyncContext needs its SDK constructor").
// NintendoAccountAuthorizationRequestContext has no vtable: its constructor writes no memory, and its contents are read and written only by the interfaces below.
HLE_FORCE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "network service account token, out of scope; cut", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { (void)ctx; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "async completion event; fires immediately", Result, h_acc_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext9GetResultEv", "async result; networking is cut, always fails", Result, h_acc_AcGetRes, void *self) { (void)self; return RES_FAIL; }
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
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "local account count", Result, h_acc_UserCount, int *n) { *n = dm_users()->count; return *n ? RES_OK : RES_FAIL; }
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "all local users qualify for offline play", Result, h_acc_ListQual, int *n, Uid *out, int max) {
    return list_users(n, out, max, 0);
}
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "look up the Uid in the local registry", Result, h_acc_Exists, uint8_t *out, const Uid *uid) {
    *out = dm_user_find(uid) != NULL; return RES_OK;
}
HLE_FORCE("_ZN2nn7account15ShowUserCreatorEv", "user creation applet; the host already has its local user", Result, h_acc_ShowCreator, void) { DM_APPLET("user creator"); return RES_OK; }
// Every user on a console has a profile image, a 256x256 JPEG, and games decode it without checking the Result: Burnout Paradise Remastered handed the empty buffer of a failed load to
// nn::image::JpegDecoder, which asserted (GitHub issue #1118). The local user's is a plain 256x256 baseline JPEG (670 bytes); the size is the image's, the copy what the buffer holds
static const char k_profile_jpeg[] =
    "\xff\xd8\xff\xe0\x00\x10\x4a\x46\x49\x46\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00\xff\xdb\x00\x43\x00\x08\x06\x06\x07\x06\x05\x08\x07\x07\x07\x09\x09\x08\x0a\x0c"
    "\x14\x0d\x0c\x0b\x0b\x0c\x19\x12\x13\x0f\x14\x1d\x1a\x1f\x1e\x1d\x1a\x1c\x1c\x20\x24\x2e\x27\x20\x22\x2c\x23\x1c\x1c\x28\x37\x29\x2c\x30\x31\x34\x34\x34\x1f\x27"
    "\x39\x3d\x38\x32\x3c\x2e\x33\x34\x32\xff\xdb\x00\x43\x01\x09\x09\x09\x0c\x0b\x0c\x18\x0d\x0d\x18\x32\x21\x1c\x21\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
    "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\xff\xc0"
    "\x00\x11\x08\x01\x00\x01\x00\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01\xff\xc4\x00\x15\x00\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x05"
    "\xff\xc4\x00\x14\x10\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xc4\x00\x15\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x05\xff\xc4\x00\x14\x11\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f"
    "\x00\x8e\x02\xe2\x50\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x3f\xff\xd9";
HLE_FORCE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", "user profile image: the local user's, a plain 256x256 JPEG", Result, h_acc_LoadImage, uint64_t *n, void *buf, uint64_t sz, const Uid *uid) {
    (void)uid; uint64_t len = sizeof k_profile_jpeg - 1;
    if (buf) memcpy(buf, k_profile_jpeg, sz < len ? sz : len);
    if (n) *n = len; return RES_OK;
}
// The save-data thumbnail is what the system's data management screen shows next to a save (The Great Ace Attorney Chronicles, SDK 10); the host has no such screen and keeps none
HLE_FORCE("_ZN2nn7account27StoreSaveDataThumbnailImageERKNS0_3UidEPKvm", "save-data thumbnail for the system's data management screen; the host keeps none", Result, h_acc_StoreThumb, const Uid *uid, const void *img, uint64_t n) {
    (void)uid; (void)img; (void)n; return RES_OK;
}
HLE_FORCE("_ZN2nn7account28DeleteSaveDataThumbnailImageERKNS0_3UidE", "save-data thumbnail; the host keeps none", Result, h_acc_DeleteThumb, const Uid *uid) { (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "network service account, out of scope; always unavailable", Result, h_acc_NsaAvail, uint8_t *out, const UserHandle *h) { (void)h; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "release this open session", void, h_acc_CloseUser, const UserHandle *h) {
    dm_host_mutex_lock(&session_mutex);
    UserSession **link = find_session(h);
    UserSession *s = *link;
    if (s) *link = s->next;
    pthread_mutex_unlock(&session_mutex);
    free(s);
}
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "async context cancel; no request", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "async context: always done", Result, h_acc_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "async context destructor; no session to close", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "async context move assignment; the object holds no host state", void *, h_acc_AcMove, void *self, void *other) { (void)other; return self; }
