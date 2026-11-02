// Epic Online Services, offline. The host never loads the EOS SDK (hle_ro.c refuses an NRO that exports EOS_Platform_Create), so main's EOS_* imports come here: they answer
// as the real SDK does on a console without a network. The platform and its interfaces are valid handles; every request that completes through a callback completes with
// EOS_NoConnection on the game's next EOS_Platform_Tick (the SDK calls completion callbacks from Tick, on the thread that ticks); copies, packet receives and modifications
// fail; notifications are registered and never fire; counts are 0 and ids invalid.
// MARVEL Cosmic Invasion's EpicHelper.Init uses the platform without checking it (a NullReferenceException at boot with a null handle, which the UNIMPL stub was); Absolum
// checks it and played offline with the null handle ("[EOS] Failed to create platform"), now with a platform whose logins fail, as on a console without a network.
#include "hle.h"
#include "dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

enum { EOS_SUCCESS = 0, EOS_NO_CONNECTION = 1, EOS_INVALID_PARAMETERS = 10, EOS_INVALID_REQUEST = 11, EOS_NOT_FOUND = 18 };
enum { EOS_NS_OFFLINE = 2 };   // EOS_ENetworkStatus

// Opaque handles: the game only passes them back. One block per interface, so a handle never reads as another's
enum { H_PLATFORM, H_AUTH, H_CONNECT, H_FRIENDS, H_LOBBY, H_P2P, H_PRESENCE, H_REPORTS, H_SANCTIONS, H_USERINFO, H_N };
static _Alignas(16) uint8_t g_handle[H_N][64];

// Completions waiting for the next Tick: the callback and the first two fields of every EOS callback info, ResultCode and ClientData; the rest of the info is zero (no ids, no
// strings), which the SDK leaves unset on a failed request too
typedef void (*eos_cb)(const void *info);
typedef struct { eos_cb cb; void *client; const char *what; } pending;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pending g_q[256];
static int g_nq;
static atomic_ulong g_notify_id;

static void complete_later(const char *what, void *client, void *cb) {
    if (!cb) return;
    pthread_mutex_lock(&g_mu);
    if (g_nq < (int)(sizeof g_q / sizeof *g_q)) g_q[g_nq++] = (pending){(eos_cb)cb, client, what};
    else LOG("[RED] EOS offline: more than %d requests waiting for a Tick, %s dropped", (int)(sizeof g_q / sizeof *g_q), what);
    pthread_mutex_unlock(&g_mu);
}

HLE("EOS_Platform_Create", void *, h_EOS_Platform_Create, const void *options) {
    (void)options; LOG("EOS offline: platform created, no network (requests complete with EOS_NoConnection)");
    return g_handle[H_PLATFORM];
}
HLE("EOS_Platform_Release", void, h_EOS_Platform_Release, void *p) { (void)p; }
HLE("EOS_Platform_Tick", void, h_EOS_Platform_Tick, void *p) {
    (void)p;
    for (;;) {   // a callback may start another request: it completes on a later Tick, as in the SDK
        pthread_mutex_lock(&g_mu);
        if (!g_nq) { pthread_mutex_unlock(&g_mu); return; }
        pending r = g_q[0]; memmove(g_q, g_q + 1, --g_nq * sizeof *g_q);
        pthread_mutex_unlock(&g_mu);
        static atomic_int logged;
        if (atomic_fetch_add(&logged, 1) < 16) LOG("EOS offline: %s → EOS_NoConnection", r.what);
        _Alignas(16) uint64_t info[16] = {0};
        *(int32_t *)info = EOS_NO_CONNECTION; info[1] = (uint64_t)(uintptr_t)r.client;
        r.cb(info);   // guest code, on the thread that ticks (as the SDK calls it)
    }
}
HLE("EOS_Platform_GetNetworkStatus", int, h_EOS_Platform_GetNetworkStatus, void *p) { (void)p; return EOS_NS_OFFLINE; }

#define IFACE(name, k) HLE("EOS_Platform_Get" #name "Interface", void *, h_EOS_Get##name, void *p) { (void)p; return g_handle[k]; }
IFACE(Auth, H_AUTH) IFACE(Connect, H_CONNECT) IFACE(Friends, H_FRIENDS) IFACE(Lobby, H_LOBBY) IFACE(P2P, H_P2P) IFACE(Presence, H_PRESENCE) IFACE(Reports, H_REPORTS)
IFACE(Sanctions, H_SANCTIONS) IFACE(UserInfo, H_USERINFO)

// Requests: (handle, options, client data, completion callback)
#define ASYNC(fn) HLE(#fn, void, h_##fn, void *h, const void *options, void *client, void *cb) { (void)h; (void)options; complete_later(#fn, client, cb); }
ASYNC(EOS_Auth_Login) ASYNC(EOS_Auth_Logout) ASYNC(EOS_Auth_DeletePersistentAuth) ASYNC(EOS_Connect_Login) ASYNC(EOS_Connect_CreateUser)
ASYNC(EOS_Connect_QueryExternalAccountMappings) ASYNC(EOS_Connect_QueryProductUserIdMappings) ASYNC(EOS_Friends_QueryFriends) ASYNC(EOS_Lobby_CreateLobby)
ASYNC(EOS_Lobby_JoinLobby) ASYNC(EOS_Lobby_LeaveLobby) ASYNC(EOS_Lobby_UpdateLobby) ASYNC(EOS_Lobby_SendInvite) ASYNC(EOS_Lobby_KickMember) ASYNC(EOS_LobbySearch_Find)
ASYNC(EOS_Presence_SetPresence) ASYNC(EOS_Reports_SendPlayerBehaviorReport) ASYNC(EOS_Sanctions_QueryActivePlayerSanctions)

// Copies and new objects: (handle, options, out pointer) → nothing to give
#define NONE(fn, res) HLE(#fn, int, h_##fn, void *h, const void *options, void **out) { (void)h; (void)options; if (out) *out = NULL; return res; }
NONE(EOS_Auth_CopyUserAuthToken, EOS_NOT_FOUND) NONE(EOS_Connect_CopyProductUserExternalAccountByIndex, EOS_NOT_FOUND) NONE(EOS_Connect_CopyProductUserInfo, EOS_NOT_FOUND)
NONE(EOS_LobbyDetails_CopyAttributeByKey, EOS_NOT_FOUND) NONE(EOS_LobbyDetails_CopyInfo, EOS_NOT_FOUND) NONE(EOS_LobbyDetails_CopyMemberAttributeByKey, EOS_NOT_FOUND)
NONE(EOS_Lobby_CopyLobbyDetailsHandle, EOS_NOT_FOUND) NONE(EOS_Lobby_CopyLobbyDetailsHandleByInviteId, EOS_NOT_FOUND) NONE(EOS_Lobby_CopyLobbyDetailsHandleByUiEventId, EOS_NOT_FOUND)
NONE(EOS_LobbySearch_CopySearchResultByIndex, EOS_NOT_FOUND) NONE(EOS_Presence_CopyPresence, EOS_NOT_FOUND) NONE(EOS_Sanctions_CopyPlayerSanctionByIndex, EOS_NOT_FOUND)
NONE(EOS_UserInfo_CopyUserInfo, EOS_NOT_FOUND) NONE(EOS_Lobby_CreateLobbySearch, EOS_INVALID_REQUEST) NONE(EOS_Lobby_UpdateLobbyModification, EOS_INVALID_REQUEST)
NONE(EOS_Presence_CreatePresenceModification, EOS_INVALID_REQUEST)

// Peer-to-peer: no packet ever arrives and none can be sent
HLE("EOS_P2P_ReceivePacket", int, h_EOS_P2P_ReceivePacket, void *h, const void *options) { (void)h; (void)options; return EOS_NOT_FOUND; }
HLE("EOS_P2P_SendPacket", int, h_EOS_P2P_SendPacket, void *h, const void *options) { (void)h; (void)options; return EOS_NO_CONNECTION; }
HLE("EOS_P2P_AcceptConnection", int, h_EOS_P2P_AcceptConnection, void *h, const void *options) { (void)h; (void)options; return EOS_NO_CONNECTION; }

// Ids and strings: none is valid, and a buffer gets nothing
HLE("EOS_ProductUserId_ToString", int, h_EOS_ProductUserId_ToString, void *id, char *out, int32_t *len) { (void)id; if (out && len && *len > 0) out[0] = 0; return EOS_INVALID_PARAMETERS; }
HLE("EOS_EpicAccountId_ToString", int, h_EOS_EpicAccountId_ToString, void *id, char *out, int32_t *len) { (void)id; if (out && len && *len > 0) out[0] = 0; return EOS_INVALID_PARAMETERS; }
HLE("EOS_ContinuanceToken_ToString", int, h_EOS_ContinuanceToken_ToString, void *t, char *out, int32_t *len) { (void)t; if (out && len && *len > 0) out[0] = 0; return EOS_INVALID_PARAMETERS; }
HLE("EOS_EResult_IsOperationComplete", int, h_EOS_EResult_IsOperationComplete, int res) { (void)res; return 1; }   // every result here is final

// Notifications: a valid id (EOS_INVALID_NOTIFICATIONID is 0); nothing ever fires
#define NOTIFY(fn) HLE(#fn, uint64_t, h_##fn, void *h, const void *options, void *client, void *cb) { (void)h; (void)options; (void)client; (void)cb; return atomic_fetch_add(&g_notify_id, 1) + 1; }
NOTIFY(EOS_Connect_AddNotifyAuthExpiration) NOTIFY(EOS_Connect_AddNotifyLoginStatusChanged) NOTIFY(EOS_Lobby_AddNotifyJoinLobbyAccepted) NOTIFY(EOS_Lobby_AddNotifyLobbyInviteAccepted)
NOTIFY(EOS_Lobby_AddNotifyLobbyInviteReceived) NOTIFY(EOS_Lobby_AddNotifyLobbyMemberStatusReceived) NOTIFY(EOS_Lobby_AddNotifyLobbyMemberUpdateReceived)
NOTIFY(EOS_Lobby_AddNotifyLobbyUpdateReceived) NOTIFY(EOS_P2P_AddNotifyPeerConnectionClosed) NOTIFY(EOS_P2P_AddNotifyPeerConnectionEstablished)
NOTIFY(EOS_P2P_AddNotifyPeerConnectionInterrupted) NOTIFY(EOS_P2P_AddNotifyPeerConnectionRequest) NOTIFY(EOS_Presence_AddNotifyOnPresenceChanged)
