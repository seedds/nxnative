// nn::account: durable local profiles, process-local open sessions, and the native user selector.
// Online accounts remain unavailable. DM_NICK names user 1 (default Player).
#include "hle.h"
#include "front.h"
#include "account_users.h"

typedef struct { uint64_t v[2]; } Uid;
typedef struct { uint64_t v[3]; } UserHandle;
_Static_assert(sizeof(Uid) == 16 && sizeof(UserHandle) == 24, "account ABI");
static const Uid k_uid = {{DM_UID0, DM_UID1}};
static Uid user_uid(int i) { return (Uid){{DM_UID0 + (uint64_t)i, DM_UID1}}; }
#define RES_ACC_CANCELED (124u | 1u << 9)

// The SDK's third handle word owns a session. Give each open a unique token:
// copied/stale/foreign handles must not close a different open of the same UID.
// GetUserId remains a plain copy, including handles constructed by games themselves.
typedef struct account_session { struct account_session *next; uint64_t token; int user; } account_session;
static pthread_mutex_t g_account_lock = PTHREAD_MUTEX_INITIALIZER;
static account_session *g_account_sessions;
static uint64_t g_account_token = UINT64_C(0x5553455200000000);
static int g_last_user;
static unsigned opened_users(void) {   // g_account_lock held
    unsigned mask = 0;
    for (account_session *s = g_account_sessions; s; s = s->next) mask |= 1u << s->user;
    return mask;
}
static int list_users(Uid *out, int max, unsigned mask) {
    int n = 0;
    for (int i = 0; out && i < DM_USERS && n < max; i++) if (mask & (1u << i)) out[n++] = user_uid(i);
    return n;
}
static void user_nickname(char *nick, int i) {
    memset(nick, 0, 33);
    if (i) snprintf(nick, 33, "Player %d", i + 1);
    else { const char *e = getenv("DM_NICK"); strncpy(nick, e ? e : "Player", 32); }
}
static Result open_user(UserHandle *h, const Uid *uid) {
    int i = dm_account_user(uid); if (i < 0) return RES_FAIL;
    account_session *s = calloc(1, sizeof *s); if (!s) return RES_FAIL;
    pthread_mutex_lock(&g_account_lock);
    s->user = i; s->token = ++g_account_token; s->next = g_account_sessions; g_account_sessions = s;
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = s->token; g_last_user = i;
    pthread_mutex_unlock(&g_account_lock); return RES_OK;
}
HLE_FORCE("_ZN2nn7account10InitializeEv", "connects to the local account service", void, h_acc_Init, void) { (void)dm_account_users(); }
HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "only users with a live account session", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    pthread_mutex_lock(&g_account_lock); *n = list_users(out, max, opened_users()); pthread_mutex_unlock(&g_account_lock); return RES_OK;
}
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "registered local users, including closed users", Result, h_acc_ListAll, int *n, Uid *out, int max) {
    unsigned mask = dm_account_users(); *n = list_users(out, max, mask); return mask ? RES_OK : RES_FAIL;
}

// Public UserSelectionSettings ABI (libnx's PselUserSelectionSettings): eight
// excluded UIDs, skip-enabled, network-required, show-skip, additional-select,
// unqualified-selectable. Selection alone never opens a user. Only an explicit
// Add user choice registers one; controller counts are not account-selection intent.
static unsigned excluded_users(const uint8_t *s) {
    unsigned mask = 0;
    for (int k = 0; s && k < DM_USERS; k++) { int i = dm_uid_user(s + 16 * k); if (i >= 0) mask |= 1u << i; }
    return mask;
}
static pthread_mutex_t g_selector_lock = PTHREAD_MUTEX_INITIALIZER;
static Result select_user(Uid *out, const uint8_t *settings, int creator) {
    unsigned users = dm_account_users(); if (!users) return RES_FAIL;
    unsigned excluded = excluded_users(settings), eligible = creator ? 0 : users & ~excluded;
    // No local profile has a Nintendo Account. Never silently return one when
    // qualification is required and the game forbids unqualified users.
    if (settings && settings[0x81] && !settings[0x84]) return RES_FAIL;
    if (!creator && settings && settings[0x80] && !settings[0x83] && !excluded &&
        !(users & (users - 1)) && !settings[0x81]) { *out = k_uid; return RES_OK; }
    int fresh = -1;
    for (int i = 1; i < DM_USERS; i++) if (!((users | excluded) & (1u << i))) { fresh = i; break; }
    front_user choices[DM_USERS]; int n = 0;
    pthread_mutex_lock(&g_account_lock); unsigned opened = opened_users(); pthread_mutex_unlock(&g_account_lock);
    for (int i = 0; i < DM_USERS; i++) if (eligible & (1u << i)) {
        choices[n].slot = i; choices[n].open = !!(opened & (1u << i)); user_nickname(choices[n].nickname, i); n++;
    }
    int skip = settings && settings[0x82];
    if (!n && fresh < 0 && !skip) return RES_ACC_CANCELED;
    int choice = front_user_select(choices, n, fresh >= 0, skip);
    if (choice == FRONT_USER_SKIP && skip) { *out = (Uid){{0, 0}}; return RES_OK; }
    if (choice == FRONT_USER_CREATE && fresh >= 0) {
        if (dm_account_create_user(fresh)) return RES_FAIL;
        choice = fresh;
    } else if (choice < 0 || choice >= DM_USERS || !(eligible & (1u << choice))) return RES_ACC_CANCELED;
    *out = user_uid(choice); LOG("account user selector: user %d selected", choice + 1); return RES_OK;
}
static Result show_selector(Uid *out, const void *settings, int creator) {
    // A dialog is a blocking applet: peers must still run while the player chooses.
    // Serialize only selectors; games can open/close/enumerate accounts meanwhile.
    dm_thread_native_wait(1); pthread_mutex_lock(&g_selector_lock);
    Result r = select_user(out, settings, creator);
    pthread_mutex_unlock(&g_selector_lock); dm_thread_native_wait(0); return r;
}
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "native user choice, respecting excluded UIDs and skip settings", Result, h_acc_ShowSel, Uid *out, const void *settings) {
    DM_APPLET("user selector"); return show_selector(out, settings, 0);
}
// The host runs one program and has no sessions handed over by an earlier program.
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "no users handed over by another program", void, h_acc_PopOpen, int *n, UserHandle *out, int max) { (void)out; (void)max; *n = 0; }
HLE_FORCE("_ZN2nn7account13PushOpenUsersEv", "the host runs only this program", void, h_acc_PushOpen, void) {}
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "open a session for the launch user (returns bool)", unsigned, h_acc_TryOpenPre, UserHandle *h) { return open_user(h, &k_uid) == RES_OK; }
// SDK 6 uses the Result; newer SDKs ignore it. Preserve that shared ABI.
HLE_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "open a session for the launch user", Result, h_acc_OpenPre, UserHandle *h) { return open_user(h, &k_uid); }
HLE_FORCE("_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", "the most recently opened user (the launch user initially)", Result, h_acc_LastOpened, Uid *out) {
    pthread_mutex_lock(&g_account_lock); *out = user_uid(g_last_user); pthread_mutex_unlock(&g_account_lock); return RES_OK;
}
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "open a session for a registered local user", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) { return open_user(h, uid); }
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "gets the Uid from any handle, as the SDK accessor does", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    // FC 27 sdk+0x1c0040 copies the first 16 bytes without inspecting the session
    // word. The game's own handles (third word 2 or 8) must continue to work.
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "DM_NICK for user 1, Player N for other registered users", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    int i = dm_account_user(uid); if (i < 0) return RES_FAIL;
    user_nickname(nick, i); return RES_OK;
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
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "number of registered local users", Result, h_acc_UserCount, int *n) {
    unsigned mask = dm_account_users(); *n = __builtin_popcount(mask); return mask ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "local users qualified for offline play", Result, h_acc_ListQual, int *n, Uid *out, int max) { return h_acc_ListAll(n, out, max); }
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "whether the UID is registered", Result, h_acc_Exists, uint8_t *out, const Uid *uid) { *out = dm_account_user(uid) >= 0; return RES_OK; }
HLE_FORCE("_ZN2nn7account15ShowUserCreatorEv", "native user creation, persisted before returning", Result, h_acc_ShowCreator, void) { DM_APPLET("user creator"); Uid uid; return show_selector(&uid, NULL, 1); }
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
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close exactly this session; other opens remain live", void, h_acc_CloseUser, const UserHandle *h) {
    if (!h) return;
    int user = dm_uid_user(h);
    pthread_mutex_lock(&g_account_lock);
    account_session **p = &g_account_sessions;
    while (*p && ((*p)->token != h->v[2] || (*p)->user != user)) p = &(*p)->next;
    if (*p) { account_session *s = *p; *p = s->next; free(s); }
    pthread_mutex_unlock(&g_account_lock);
}
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "async context cancel; no request", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "async context: always done", Result, h_acc_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "async context destructor; no session to close", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "async context move assignment; the object holds no host state", void *, h_acc_AcMove, void *self, void *other) { (void)other; return self; }
