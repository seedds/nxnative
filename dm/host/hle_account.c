// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user, and more when the game asks for them (the user selector, below).
// Online accounts (network service account / Nintendo Account authorization / tokens) are out of scope and uniformly return RES_FAIL; the async context's completion event fires immediately.
// Environment switch: DM_NICK=nickname (default Player).
#include "hle.h"
#include <stdatomic.h>

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{DM_UID0, DM_UID1}};   // user 1; non-zero means valid
// The local users: user 1 from the start, users 2..DM_USERS (k_uid's first word plus the index, dm_uid_user in hle.h) made one at a time by the user selector when the game leaves
// every existing user out of it. A console with one user shows its selector with only "Add User" then, and the player makes one: a local co-op game that asks each new player for a
// user of their own (Minecraft Dungeons II: the selector with player 1's Uid left out, then the error applet when it got that Uid back, at every press of player 2's controller) gets one.
// A game that never asks sees one user, as before
static _Atomic int g_users = 1;
// Users the game has open (OpenUser, the preselected user's opens) minus those it closed; user 1, the user the game was launched with, always counts as open
static _Atomic int g_opened[DM_USERS];
static void note_open(const void *uid, int d) { int i = dm_uid_user(uid); if (i > 0) atomic_fetch_add(&g_opened[i], d); }
static int user_open(int i) { return i == 0 || atomic_load(&g_opened[i]) > 0; }
int dm_players_reported(void);   // the players the game sees (hle_hid.c)
static Uid user_uid(int i) { return (Uid){{DM_UID0 + (uint64_t)i, DM_UID1}}; }
static int list_users(Uid *out, int max) { int n = atomic_load(&g_users), k = 0; for (; out && k < n && k < max; k++) out[k] = user_uid(k); return k; }
// The handle's third word is the SDK's session object; the host has no sessions and writes this marker, which helps reading a dump but which nothing checks:
// games also pass handles they built themselves (see GetUserId)
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"

HLE_FORCE("_ZN2nn7account10InitializeEv", "connects to the acc:u0 account service; the host has one fixed local user", void, h_acc_Init, void) {}

// Every user is open: the host keeps no sessions, and a user the selector handed out is one the game opens next
HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "list of open users; the local users", Result, h_acc_ListOpen, int *n, Uid *out, int max) { *n = list_users(out, max); return RES_OK; }
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "list of all users; the local users (Super Mario 3D World + Bowser's Fury)", Result, h_acc_ListAll, int *n, Uid *out, int max) { *n = list_users(out, max); return RES_OK; }
// UserSelectionSettings (SDK; libnx PselUserSelectionSettings): Uid invalidUidList[8] (the users the player cannot pick: those already playing), then isSkipEnabled,
// isNetworkServiceAccountRequired, showSkipButton, additionalSelect, isUnqualifiedUserSelectable. The host picks for the player: the first user not left out, else a new one
// (as the player adding a user), else canceled (account ResultCancelledByUser, 2124-0001) once all DM_USERS are out. The network service account is ignored, as everywhere here
#define RES_ACC_CANCELED (124u | 1u << 9)
static int left_out(const uint8_t *settings, int i) {
    Uid u = user_uid(i);
    for (int k = 0; settings && k < 8; k++) if (!memcmp(settings + 16 * k, &u, sizeof u)) return 1;
    return 0;
}
// Who is at the selector: with more players connected than users open, a new player (Minecraft Dungeons II shows it at player 2's first press with nothing left out,
// and shows its error 20, "Something went wrong", when it gets back player 1's user, which is open): the host picks a user nobody has open, or adds one. Otherwise the
// players already playing are choosing, and the first user not left out is picked, as before
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "user selection applet; the host picks a local user not left out (one nobody has open for a new player), or adds one", Result, h_acc_ShowSel, Uid *out, const void *settings) { DM_APPLET("user selector");
    const uint8_t *s = settings;
    int n = atomic_load(&g_users), i = 0, open = 0, players = dm_players_reported();
    for (int k = 0; k < n; k++) open += user_open(k);
    int fresh = players > open;
    int skipped = 0; for (int k = 0; s && k < 8; k++) skipped += memcmp(s + 16 * k, (const uint8_t[16]){0}, 16) != 0;
    LOG("account user selector: %d players, %d of %d users open, %d users left out by the game%s", players, open, n, skipped, fresh ? ": a new player picks a user nobody has open" : "");
    while (i < n && (left_out(s, i) || (fresh && user_open(i)))) i++;
    if (i == n) {
        if (n >= DM_USERS) { LOG("account user selector: all %d users left out by the game: canceled", n); return RES_ACC_CANCELED; }
        atomic_store(&g_users, n + 1);
        LOG("account user selector: no user to pick, user %d added (as the player adding one)", n + 1);
    }
    if (i || n > 1) LOG("account user selector: user %d picked%s", i + 1, s && s[0x81] ? " (the game asks for a network service account: the host has none)" : "");
    *out = user_uid(i); return RES_OK;
}
// Take the users the system opened on the application's behalf (Mario 1.2.1 asks this first and falls back to TryOpenPreselectedUser on 0; the game only reads the count, not the return value):
// the host has no such users and returns 0, so the game opens the preselected user itself, as in v0
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "users opened by the system: none (the game falls back to the preselected user)", void, h_acc_PopOpen, int *n, UserHandle *out, int max) { (void)out; (void)max; *n = 0; }
// Hand the open users to the next program of a multi-program application (Octopath Traveler 0 imports it with oe::ExecuteProgram): the host runs one program, so there is nothing to hand over
HLE_FORCE("_ZN2nn7account13PushOpenUsersEv", "hand the open users to the next program: the host runs only this one", void, h_acc_PushOpen, void) {}
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "user preselected at launch; always the local user (returns bool)", unsigned, h_acc_TryOpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; note_open(h, 1); return 1;
}
// The aborting form (Super Mario Odyssey, SDK 7): without the hook the SDK's IPC call fails and aborts with no message. In SDK 6 the same symbol returns a Result, and Fire Emblem: Three Houses
// skips its save mount when it is non-zero (main+0x53b7e0); returning void left the handle pointer in w0, so the game never mounted "save" and its loading screen waited forever for save:/system.
// It returns 0 now, which SDK 7+ callers ignore
HLE_FORCE("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", "user preselected at launch (returns a Result in SDK 6, later versions abort on failure); always the local user", Result, h_acc_OpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account17GetLastOpenedUserEPNS0_3UidE", "last opened user; always the local user (Super Mario Odyssey)", Result, h_acc_LastOpened, Uid *out) { *out = k_uid; return RES_OK; }
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "open user; the handle holds the Uid itself", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) {
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = HANDLE_MAGIC; note_open(uid, 1);
    if (dm_uid_user(uid) > 0) LOG("account: user %d opened", dm_uid_user(uid) + 1);
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "gets the Uid from the handle", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    // The SDK's accessor copies the Uid, the handle's first 16 bytes, and returns 0; it never reads the third word (FC 27: nn::account::GetUserId sdk+0x1c0040 calls
    // detail::GetUserId sdk+0x1bede0, ldp x8, x1, [x0]). EA SPORTS FC 27 passes a handle it built and never filled for a second controller's player (third word 0x2)
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
// nn::account::Nickname = struct { char name[33]; }, UTF-8. User 1's is DM_NICK; a user the selector added is "Player <n>"
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "user nickname; taken from DM_NICK for user 1, Player <n> for the others", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    const char *e = getenv("DM_NICK"); int i = dm_uid_user(uid);
    memset(nick, 0, 33);
    if (i > 0) snprintf(nick, 33, "Player %d", i + 1); else strncpy(nick, e ? e : "Player", 32);
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
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "user count; the local users", Result, h_acc_UserCount, int *n) { *n = atomic_load(&g_users); return RES_OK; }
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "list of qualified users; the local users", Result, h_acc_ListQual, int *n, Uid *out, int max) { *n = list_users(out, max); return RES_OK; }
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "whether a user exists; true for the local users", Result, h_acc_Exists, uint8_t *out, const Uid *uid) {
    int i = dm_uid_user(uid); *out = i >= 0 && i < atomic_load(&g_users); return RES_OK;
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
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close user; the handle holds no session", void, h_acc_CloseUser, const UserHandle *h) {
    int i = h ? dm_uid_user(h) : -1;
    if (i > 0 && atomic_load(&g_opened[i]) > 0) { atomic_fetch_sub(&g_opened[i], 1); LOG("account: user %d closed", i + 1); }
}
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "async context cancel; no request", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "async context: always done", Result, h_acc_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "async context destructor; no session to close", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "async context move assignment; the object holds no host state", void *, h_acc_AcMove, void *self, void *other) { (void)other; return self; }
