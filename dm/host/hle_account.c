// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user.
// Online service identity requires an explicit provider and an actual unexpired NEX proof.
// Nintendo Account authorization remains unavailable; OAuth is not a game credential.
// Environment switch: DM_NICK=nickname (default the Nextendo username in Nextendo mode, else Player).
#include "hle.h"
#include "account_local.h"
#include "nextendo_account.h"
#include "net_socket.h"

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // Non-zero means valid
int dm_account_uid_exists(const void *uid) { return uid && !memcmp(uid, &k_uid, sizeof k_uid); }
int dm_account_copy_uid(void *output) { if (!output) return 0; memcpy(output, &k_uid, sizeof k_uid); return 1; }
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"
static int online_user(const UserHandle *h) { return h && h->v[2] == HANDLE_MAGIC && h->v[0] == k_uid.v[0] && h->v[1] == k_uid.v[1]; }
// In Nextendo mode each online account call's answer is logged the first time it is given (the call, its result, and what the availability check said; never the
// token or the account), so a device log shows which step of a game's sign-in stopped. value: -1 none, 0 / 1 not available / available, -2 not the local user's handle
enum { ACC_NONE = -1, ACC_NOT_LOCAL = -2 };
static Result acc_note(const char *call, Result r, int value) {
    if (!dm_net_nextendo_enabled()) return r;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; static struct { const char *call; Result r; int value; } seen[64]; static unsigned count;
    pthread_mutex_lock(&lock); int fresh = 1;
    for (unsigned i = 0; i < count; i++) if (seen[i].call == call && seen[i].r == r && seen[i].value == value) fresh = 0;
    if (fresh && count < 64) { seen[count].call = call; seen[count].r = r; seen[count].value = value; count++; }
    pthread_mutex_unlock(&lock);
    if (fresh) LOG("%saccount: %s -> 0x%x%s", r ? "[RED] " : "", call, r,
                   value == ACC_NOT_LOCAL ? " (not the local user's handle)" : value == 1 ? " (available)" : value == 0 ? " (not available)" : "");
    return r;
}
#define ACC_USER(h) (online_user(h) ? ACC_NONE : ACC_NOT_LOCAL)

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
// nn::account::Nickname = struct { char name[33]; }, UTF-8. DM_NICK first; in Nextendo mode then the signed-in account's username (what other players and the
// Nextendo site see), read once and kept, as games ask for it often; else Player
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "user nickname: DM_NICK, else the Nextendo username in Nextendo mode, else Player", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    (void)uid;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; static char account[33];
    const char *e = getenv("DM_NICK");
    memset(nick, 0, 33);
    if (e) {   // whole UTF-8 characters only, as the Nextendo name is cut
        size_t n = strlen(e); if (n > 32) { n = 32; while (n && ((unsigned char)e[n] & 0xc0) == 0x80) n--; }
        memcpy(nick, e, n); return RES_OK;
    }
    if (dm_net_nextendo_enabled()) {
        pthread_mutex_lock(&lock);
        if (!account[0] && !dm_nextendo_account_username(account)) LOG("account: nickname from the Nextendo account");
        memcpy(nick, account, 33); pthread_mutex_unlock(&lock);
        if (nick[0]) return RES_OK;
    }
    strncpy(nick, "Player", 32);
    return RES_OK;
}

// ---------- Online accounts: cut ----------
HLE_FORCE_WAIT("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "network service account requires the user's actual unexpired Nextendo NEX proof", Result, h_acc_EnsureNsa, const UserHandle *h) { return acc_note("EnsureNetworkServiceAccountAvailable", online_user(h) ? dm_nextendo_account_ensure() : RES_FAIL, ACC_USER(h)); }
HLE_FORCE_WAIT("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "real provider PID or required server-reported BAAS identity", Result, h_acc_GetNsaId, uint64_t *id, const UserHandle *h) { if (id) *id = 0; return acc_note("GetNetworkServiceAccountId", online_user(h) ? dm_nextendo_account_id(id) : RES_FAIL, ACC_USER(h)); }
HLE_FORCE_WAIT("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "real NEX proof in the native game's cached account token", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    if (n) *n = 0; return acc_note("LoadNetworkServiceAccountIdTokenCache", online_user(h) ? dm_nextendo_account_load_token(n, buf, sz) : RES_FAIL, ACC_USER(h));
}
HLE_FORCE("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", "online membership notice applet, out of scope; cut", Result, h_acc_ShowLic, const UserHandle *h) { (void)h; return acc_note("ShowLicenseRequirementsForNetworkService", RES_FAIL, ACC_NONE); }
// The Nintendo Account's cached profile (birthday, country). Offline no Nintendo Account is linked, so loading fails and the game skips its getters (Big Brain Academy:
// Brain vs. Brain, main+0xab8bc). In Nextendo mode, with the account signed in, it loads, as Ryujinx-Nextendo and Mush8 answer the account service's 0x68-byte NasUserBase
// (birthday at 0, country at 0x20) that the SDK copies into this object. The host never writes the object, whose layout in a given SDK is unchecked: it takes over the
// object's four getters, which return a string and its length. Country: the Nextendo account's (dm_nextendo_account_country), else the device's region (DM_NEXTENDO_COUNTRY,
// which the launcher sets), else none. Birthday: the Nextendo account's (dm_nextendo_account_birthday), else the launcher's picker (DM_NEXTENDO_BIRTHDAY, YYYY-MM-DD), else 2000-01-01, the date both of those clients send. Gender and region: none
static char na_country[3], na_birthday[11] = "2000-01-01";   // set by a successful load, before the game reads the getters
static int na_country_ok(const char *c) { return c && c[0] >= 'A' && c[0] <= 'Z' && c[1] >= 'A' && c[1] <= 'Z' && !c[2]; }
static int na_birthday_ok(const char *b) {
    if (!b || strlen(b) != 10 || b[4] != '-' || b[7] != '-') return 0;
    for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && (b[i] < '0' || b[i] > '9')) return 0;
    int month = (b[5] - '0') * 10 + b[6] - '0', day = (b[8] - '0') * 10 + b[9] - '0'; return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}
HLE_FORCE_WAIT("_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm", "cached Nintendo Account info: the Nextendo account's country and the launcher's birthday; offline none linked, fails", Result, h_acc_LoadNaInfo,
          void *info, const UserHandle *h, void *buf, uint64_t sz) {
    (void)info; if (buf && sz) memset(buf, 0, sz);
    uint8_t available = 0; Result r = online_user(h) ? dm_nextendo_account_available(&available) : RES_FAIL; if (!r && !available) r = RES_FAIL;
    if (!r) {
        char country[3] = {0}; if (dm_nextendo_account_country(country) || !na_country_ok(country)) country[0] = 0;
        const char *device = getenv("DM_NEXTENDO_COUNTRY"); if (!country[0] && na_country_ok(device)) memcpy(country, device, 3);
        char account[11] = {0}; int own = !dm_nextendo_account_birthday(account) && na_birthday_ok(account);
        const char *birthday = own ? account : getenv("DM_NEXTENDO_BIRTHDAY");
        memcpy(na_country, country, 3); memcpy(na_birthday, na_birthday_ok(birthday) ? birthday : "2000-01-01", 11);
        static int told; if (!told++) LOG("account: Nintendo Account info: country %s, birthday %s", na_country[0] ? "set" : "none",
                                          own ? "from the Nextendo account" : na_birthday_ok(birthday) ? "from the launcher" : "2000-01-01");
    }
    return acc_note("LoadCachedNintendoAccountInfo", r, online_user(h) ? ACC_NONE : ACC_NOT_LOCAL);
}
static const char *na_string(const char *s, uint64_t *n) { if (n) *n = strlen(s); return s; }
HLE_FORCE("_ZNK2nn7account25CachedNintendoAccountInfo10GetCountryEPm", "the loaded Nintendo Account country (two letters, or none)", const char *, h_acc_NaCountry, const void *self, uint64_t *n) { (void)self; return na_string(na_country, n); }
HLE_FORCE("_ZNK2nn7account25CachedNintendoAccountInfo11GetBirthdayEPm", "the loaded Nintendo Account birthday, YYYY-MM-DD", const char *, h_acc_NaBirthday, const void *self, uint64_t *n) { (void)self; return na_string(na_birthday, n); }
HLE_FORCE("_ZNK2nn7account25CachedNintendoAccountInfo9GetGenderEPm", "Nintendo Account gender: none", const char *, h_acc_NaGender, const void *self, uint64_t *n) { (void)self; return na_string("", n); }
HLE_FORCE("_ZNK2nn7account25CachedNintendoAccountInfo9GetRegionEPm", "Nintendo Account region: none", const char *, h_acc_NaRegion, const void *self, uint64_t *n) { (void)self; return na_string("", n); }

// Online membership (NetworkServiceLicenseKind: 0 none, 1 subscribed). Ryujinx-Nextendo (LoadNetworkServiceLicenseKindAsync) and Mush8 (IManagerForApplication 170)
// answer subscribed once the id-token request completes; the request here is the same one (dm_nextendo_async_ensure), and the kind is subscribed only when it succeeded.
// AsyncNetworkServiceLicenseInfoContext derives from AsyncContext (it has no getters of its own but this one): its SDK constructor runs, and the AsyncContext takeovers
// below serve its event, completion and result. Its own move operations and deleting destructor stay the SDK's, as its fields past the AsyncContext are unchecked: if they
// do not go through AsyncContext's (taken over), the request state stays with the old object and the kind reads none, which the log shows
HLE_FORCE_WAIT("_ZN2nn7account34LoadNetworkServiceLicenseInfoAsyncEPNS0_37AsyncNetworkServiceLicenseInfoContextERKNS0_10UserHandleE", "online membership: the id-token request; subscribed when it succeeds; offline, cut", Result, h_acc_LicenseAsync,
               void *ctx, const UserHandle *h) { return acc_note("LoadNetworkServiceLicenseInfoAsync", online_user(h) ? dm_nextendo_async_ensure(ctx) : RES_FAIL, ACC_USER(h)); }
HLE_FORCE_WAIT("_ZN2nn7account37AsyncNetworkServiceLicenseInfoContext28GetNetworkServiceLicenseKindEv", "subscribed only after a successful Nextendo request", uint32_t, h_acc_LicenseKind, void *self) {
    uint32_t kind = dm_net_nextendo_enabled() && dm_nextendo_async_result(self) == 0 ? 1 : 0;
    acc_note("GetNetworkServiceLicenseKind", kind ? RES_OK : RES_FAIL, ACC_NONE); return kind;
}

// AsyncContext / NintendoAccountAuthorizationRequestContext.
// AsyncContext is built by the SDK's own constructor, which is not taken over: it writes the class's vtable and nulls the IPC object pointer at +0x18, and sends
// nothing (SDK 19.3.11: sdk+0x1ad6c8). No interface below sets that pointer, so the SDK's destructor, reached through the vtable, finds no session to close.
// The object is polymorphic: Pokémon Friends' C# binding deletes it from nn.account.AsyncContext's finalizer through the virtual deleting destructor, inlined
// in main (main+0x5b64f0), which the D1Ev takeover below never sees. A host constructor that wrote no memory left the allocator's bytes in the vtable word,
// and that call jumped through them (docs/reference/services.md, "An account AsyncContext needs its SDK constructor").
// With Nextendo on (dm_net_nextendo_enabled), the request state (hle_nextendo_apple.m) is created when a request starts (dm_nextendo_async_execute). With it off,
// no request ever starts, and a context answers as the offline host always has: the completion event fires immediately, it is done, and the result is failure.
// NintendoAccountAuthorizationRequestContext has no vtable: its constructor writes no memory, and its contents are read and written only by the interfaces below.
HLE_FORCE_WAIT("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "cache the real Nextendo NEX game token and publish its actual completion result; offline, cut", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { return acc_note("EnsureNetworkServiceAccountIdTokenCacheAsync", online_user(h) ? dm_nextendo_async_ensure(ctx) : RES_FAIL, ACC_USER(h)); }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "account completion event with a checked kernel-event lifetime; Nextendo off, it fires immediately", Result, h_acc_AcGetEv, void *self, void *ev) {
    if (!dm_net_nextendo_enabled()) { hle_signal_system_event(ev); return RES_OK; }
    uint64_t lifetime = dm_nextendo_async_lifetime(self); if (!ev || !lifetime) return RES_FAIL;
    hle_bind_system_event(ev, 0); return dm_nextendo_async_attach(self, lifetime, hle_system_event_binding(ev));
}
// Nextendo off, no request has started, and dm_nextendo_async_result returns DM_ACCOUNT_FAIL, RES_FAIL's value (nextendo_account.h)
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext9GetResultEv", "actual completed account request result; Nextendo off, failure", Result, h_acc_AcGetRes, void *self) { return acc_note("AsyncContext::GetResult", dm_nextendo_async_result(self), ACC_NONE); }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev", "Nintendo Account authorization, out of scope; the constructor writes no memory", void, h_acc_NaCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", "Nintendo Account authorization, out of scope", void, h_acc_NaDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",
          "Nintendo Account authorization, out of scope; cut", Result, h_acc_NaCreate, void *ctx, const UserHandle *h, const void *prm, void *buf, uint64_t sz) {
    (void)ctx; (void)h; (void)prm; (void)buf; (void)sz; return acc_note("CreateNintendoAccountAuthorizationRequest", RES_FAIL, ACC_NONE);
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
HLE_FORCE_WAIT("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "only a coherent unexpired Nextendo NEX credential enables the local user's NSA", Result, h_acc_NsaAvail, uint8_t *out, const UserHandle *h) {
    if (!out) return RES_FAIL; *out = 0;
    Result r = online_user(h) ? dm_nextendo_account_available(out) : RES_FAIL;
    return acc_note("IsNetworkServiceAccountAvailable", r, online_user(h) ? *out != 0 : ACC_NOT_LOCAL);
}
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close user; the handle holds no session", void, h_acc_CloseUser, const UserHandle *h) { (void)h; }
// Nextendo off (see AsyncContext above), a context has nothing to cancel and is done
static Result acc_done(void *self, uint8_t *done) { if (dm_net_nextendo_enabled()) return dm_nextendo_async_done(self, done); if (!done) return RES_FAIL; *done = 1; return RES_OK; }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext6CancelEv", "cancel an account request without publishing a late success; Nextendo off, nothing to cancel", Result, h_acc_AcCancel, void *self) { return dm_net_nextendo_enabled() ? dm_nextendo_async_cancel(self) : RES_OK; }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext7HasDoneEPb", "actual account request completion; Nextendo off, done", Result, h_acc_AcHasDone, void *self, uint8_t *done) { return acc_done(self, done); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextD1Ev", "destroy owned account request state", void, h_acc_AcDtor, void *self) { dm_nextendo_async_destroy(self); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextaSEOS1_", "move account request state and its event lifetimes", void *, h_acc_AcMove, void *self, void *other) { return dm_nextendo_async_move(self, other); }

// Additional SDK service overloads use the same provider and object lifetime.
HLE_FORCE_WAIT("_ZNK2nn7account12AsyncContext9GetResultEv", "actual completed account request result, SDK 19 const overload", Result, h_acc_AcGetResConst, void *self) { return acc_note("AsyncContext::GetResult", dm_nextendo_async_result(self), ACC_NONE); }
HLE_FORCE_WAIT("_ZNK2nn7account12AsyncContext7HasDoneEPb", "actual account request completion, SDK 19 const overload; Nextendo off, done", Result, h_acc_AcHasDoneConst, void *self, uint8_t *done) { return acc_done(self, done); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextD2Ev", "destroy owned account request state", void, h_acc_AcDtor2, void *self) { dm_nextendo_async_destroy(self); }
HLE_FORCE("_ZN2nn7account12AsyncContextD0Ev", "destroy account request state and release its guest allocation", void, h_acc_AcDeletingDtor, void *self) {
    dm_nextendo_async_destroy(self); void (*release)(void *) = (void (*)(void *))guest_sym("_ZdlPv");
    if (release) release(self); else LOG("[RED] account: guest operator delete unavailable for AsyncContext");
}
// A move constructor builds a new object, so it writes what the SDK constructor writes (see AsyncContext above): the vtable, taken from the source, which the SDK
// constructed, and a null IPC object pointer at +0x18 (the source's is null too: no interface here sets it). The request state then moves with the object.
static void acc_move_construct(void *self, void *other) {
    if (self && other) { *(void **)self = *(void **)other; *(void **)((char *)self + 0x18) = NULL; }
    dm_nextendo_async_move(self, other);
}
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextC1EOS1_", "move-construct: the SDK's vtable and request state", void, h_acc_AcMoveCtor, void *self, void *other) { acc_move_construct(self, other); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContextC2EOS1_", "move-construct: the SDK's vtable and request state", void, h_acc_AcMoveCtor2, void *self, void *other) { acc_move_construct(self, other); }
HLE_FORCE_WAIT("_ZN2nn7account12AsyncContext4SwapERS1_", "swap owned account request state", void, h_acc_AcSwap, void *self, void *other) { dm_nextendo_async_swap(self, other); }HLE_FORCE_WAIT("_ZNK2nn7account12AsyncContextcvbEv", "whether an account request owns an actual operation", unsigned, h_acc_AcValid, void *self) { return dm_nextendo_async_valid(self); }
