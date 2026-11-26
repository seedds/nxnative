// Networking and peripheral system services (out of scope: online play, local wireless, friends, telemetry, SSL) as seen through newer SDKs: all made "unavailable", the game takes its offline branch.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
#include "hle.h"
#include "hle_net.h"

// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "telemetry report save (user); nothing is sent, reports success", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "telemetry report save; nothing is sent, reports success", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
// SDK 22's PlayReport::Send (Ocarina of Time saves one as the first scene starts, main+0x379164c) returns an AsyncContext in x8, which holds a pointer to the service's object. Null is what the SDK's
// own default context has: GetCompletionEvent makes a signaled event (sdk+0x5a9298), GetResult answers 0 and the destructor does nothing, so only this call is taken over. Naked because x8 is the
// hidden result pointer, 8 bytes on the caller's stack, which no C return type can name
__attribute__((naked)) static void h_prepo_Send(void) { __asm__("str xzr, [x8]\n    ret"); }
DM_REGISTER("swforce", hle_force_ent, hle_force_h_prepo_Send, {"_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE", (void *)h_prepo_Send, "telemetry report send; nothing is sent, a default context completes at once with success"});
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "telemetry init: the original connects to the prepo service (through sm), the host has no service process; no-op (Engage calls it explicitly)", Result, h_prepo_Init, void) { return RES_OK; }
// Add the session ID / connection status to a report: on hardware these first connect to the prepo service for the session (Super Mario 3D World + Bowser's Fury calls them entering the save select); the host adds nothing and reports success
HLE_FORCE("_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc", "telemetry session ID: no service connection, nothing added, reports success", Result, h_prepo_AddSess, void *rep, const char *key) { (void)rep; (void)key; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "telemetry internet connection status: no service connection, nothing added, reports success", Result, h_prepo_AddNet, void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "telemetry immediate upload; nothing is sent, reports success", Result, h_prepo_Tx, void) { return RES_OK; }
HLE_FORCE("_ZN2nn5prepo21GetTransmissionStatusEPNS0_18TransmissionStatusE", "telemetry upload status: idle (nothing is queued)", Result, h_prepo_TxStatus, uint32_t *st) { if (st) *st = 0; return RES_OK; }

// ---------- bluetooth: BLE (Pokémon Violet 3.0.1 at boot, likely its Pokémon GO Plus + support). InitializeBle connects to bt and btm:u (an SDK abort without them); the host has no radio:
// events never fire, scans find nothing, so the GATT calls after a connect are never reached ----------
HLE_FORCE("_ZN2nn9bluetooth13InitializeBleEv", "BLE init (cut): no service connection", void, h_bt_InitBle, void) {}
HLE_FORCE("_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE", "BLE scan event: bound, never fires", Result, h_bt_EvScan, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE", "BLE connection event: bound, never fires", Result, h_bt_EvConn, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE", "BLE discovery event: bound, never fires", Result, h_bt_EvDisc, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE", "BLE MTU event: bound, never fires", Result, h_bt_EvMtu, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE", "BLE GATT event: bound, never fires", Result, h_bt_EvGatt, void *ev) { hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth19GetBleScanParameterEPNS0_17GattAttributeUuidEi", "BLE scan parameter (cut)", Result, h_bt_ScanPrm, void *uuid, int id) { (void)uuid; (void)id; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE", "BLE scan (cut)", Result, h_bt_ScanStart, const void *uuid) { (void)uuid; return RES_FAIL; }
HLE_FORCE("_ZN2nn9bluetooth22StopBleScanSmartDeviceEv", "BLE scan stop (cut)", Result, h_bt_ScanStop, void) { return RES_OK; }
HLE_FORCE("_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi", "BLE scan results (cut): 0 found", int, h_bt_ScanRes, void *out, int max) { (void)out; (void)max; return 0; }
HLE_FORCE("_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi", "BLE connections (cut): 0", int, h_bt_ConnList, void *out, int max) { (void)out; (void)max; return 0; }
HLE_FORCE("_ZN2nn9bluetooth19ConnectToGattServerERKNS0_7AddressE", "BLE connect (cut)", Result, h_bt_Connect, const void *a) { (void)a; return RES_FAIL; }

// ---------- socket: aliases of the offline BSD service, including its process-local UDP transport ----------
enum { SK_ENETDOWN = 50, SK_EAI_FAIL = 4 };
// The overloads the game's LDN data path can use go through hle_net.c's mpsk_*: Link Play on (hle_coop_ldn.c), a virtual descriptor is the coop module's UDP and any other
// gets -1 / ENETDOWN; Link Play off, the host's local UDP as before
int mpsk_errno(void); int mpsk_socket(int fam, int type, int proto); int mpsk_bind(int s, const void *a, unsigned n);
int mpsk_getname(int s, void *a, unsigned *n); int mpsk_getopt(int s, int l, int o, void *v, unsigned *n); int mpsk_setopt(int s, int l, int o, const void *v, unsigned n);
int mpsk_fcntl(int s, int cmd, int v); long mpsk_sendto(int s, const void *b, unsigned long n, int f, const void *to, unsigned tl);
long mpsk_recvfrom(int s, void *b, unsigned long n, int f, void *from, unsigned *fl); long mpsk_recv(int s, void *b, unsigned long n, int f); long mpsk_send(int s, const void *b, unsigned long n, int f);
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "pairs with Initialize (cut)", Result, h_sk_Fini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "local UDP (offline BSD service)", int, h_sk_Errno, void) { return mpsk_errno(); }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "local UDP (offline BSD service)", int, h_sk_Socket3i, int a, int b, int c) { return mpsk_socket(a, b, c); }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "local UDP (offline BSD service)", int, h_sk_Shutdown, int s, int h) { return dm_sk_shutdown(s, h); }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "local UDP (offline BSD service)", int, h_sk_Bind, int s, const void *a, unsigned n) { return mpsk_bind(s, a, n); }
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "waits on process-local UDP and event descriptors", int, h_sk_Poll, void *p, uint64_t n, int t) { return dm_sk_poll(p, n, t); }
// SDK 4's Select with plain fd_set / timeval (Dark Souls Remastered)
HLE_FORCE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", "Poll over process-local UDP and event descriptor sets", int, h_sk_Select2, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "local UDP (offline BSD service)", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return mpsk_sendto(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "local UDP (offline BSD service)", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return mpsk_sendto(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "local UDP (offline BSD service)", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return mpsk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "name resolution (cut): EAI_FAIL", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "resolver error text", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
// SDK 11's overloads with plain sockaddr / int arguments (Big Brain Academy: Brain vs. Brain, through Pia)
HLE_FORCE("_ZN2nn6socket7ConnectEiPK8sockaddrj", "local UDP (offline BSD service)", int, h_sk_Connect2, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
HLE_FORCE("_ZN2nn6socket6AcceptEiP8sockaddrPj", "offline (cut)", int, h_sk_Accept2, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", "local UDP (offline BSD service)", int, h_sk_GetSockName2, int s, void *a, unsigned *n) { return mpsk_getname(s, a, n); }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiiiPvPj", "local UDP (offline BSD service)", int, h_sk_GetSockOpt2, int s, int l, int o, void *v, unsigned *n) { return mpsk_getopt(s, l, o, v, n); }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiiiPKvj", "local UDP (offline BSD service)", int, h_sk_SetSockOpt2, int s, int l, int o, const void *v, unsigned n) { return mpsk_setopt(s, l, o, v, n); }
HLE_FORCE("_ZN2nn6socket5FcntlEiiz", "local UDP (offline BSD service)", int, h_sk_Fcntl2, int s, int c, int v) { return mpsk_fcntl(s, c, v); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmi", "local UDP (offline BSD service)", long, h_sk_Recv2, int s, void *b, unsigned long n, int f) { return mpsk_recv(s, b, n, f); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmi", "local UDP (offline BSD service)", long, h_sk_Send2, int s, const void *b, unsigned long n, int f) { return mpsk_send(s, b, n, f); }

// ---------- irsensor: the Joy-Con IR camera (Big Brain Academy: Brain vs. Brain's input layer makes one per controller). The host has none: every camera is unconnected,
// processors cannot run and report no states. The default-config getters and MakeRect only fill the game's structs and keep running in the SDK ----------
enum { IRS_UNCONNECTED = 2 };   // IrCameraStatus: 0 available, 1 unsupported, 2 unconnected (the game takes 0 as "available", main+0x497fd4)
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraHandleERKj", "IR camera handle of a controller (player number in the low byte)", uint32_t, h_irs_Handle, const uint32_t *npad) { return *npad & 0xff; }
HLE_FORCE("_ZN2nn8irsensor10InitializeERKNS0_14IrCameraHandleE", "IR camera (cut): nothing to initialize", void, h_irs_Init, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor8FinalizeERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Fini, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor17GetIrCameraStatusERKNS0_14IrCameraHandleE", "IR camera (cut): unconnected", int, h_irs_Status, const void *h) { (void)h; return IRS_UNCONNECTED; }
HLE_FORCE("_ZN2nn8irsensor18StopImageProcessorERKNS0_14IrCameraHandleE", "IR camera (cut)", void, h_irs_Stop, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn8irsensor18RunMomentProcessorERKNS0_14IrCameraHandleERKNS0_21MomentProcessorConfigE", "IR camera (cut)", Result, h_irs_RunMoment, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor22RunClusteringProcessorERKNS0_14IrCameraHandleERKNS0_25ClusteringProcessorConfigE", "IR camera (cut)", Result, h_irs_RunCluster, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor15RunHandAnalysisERKNS0_14IrCameraHandleERKNS0_18HandAnalysisConfigE", "IR camera (cut)", Result, h_irs_RunHand, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_28ImageTransferProcessorConfigEPvm", "IR camera (cut)", Result, h_irs_RunImage,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25RunImageTransferProcessorERKNS0_14IrCameraHandleERKNS0_30ImageTransferProcessorExConfigEPvm", "IR camera (cut)", Result, h_irs_RunImageEx,
          const void *h, const void *c, void *b, size_t n) { (void)h; (void)c; (void)b; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor24GetMomentProcessorStatesEPNS0_20MomentProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Moment,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor28GetClusteringProcessorStatesEPNS0_24ClusteringProcessorStateEPiiRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_Cluster,
          void *s, int *n, int max, const void *h) { (void)s; (void)max; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor25GetHandAnalysisImageStateEPNS0_22HandAnalysisImageStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandImage,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetHandAnalysisSilhouetteStateEPNS0_27HandAnalysisSilhouetteStateEPiilRKNS0_14IrCameraHandleE", "IR camera (cut): 0 states", Result, h_irs_HandSil,
          void *s, int *n, int max, int64_t since, const void *h) { (void)s; (void)max; (void)since; (void)h; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn8irsensor30GetImageTransferProcessorStateEPNS0_27ImageTransferProcessorStateEPvmRKNS0_14IrCameraHandleE", "IR camera (cut)", Result, h_irs_ImageState,
          void *s, void *b, size_t n, const void *h) { (void)s; (void)b; (void)n; (void)h; return RES_FAIL; }

// ---------- ssl: offline, creating a context fails ----------
HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL (cut)", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL (cut)", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL (cut)", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL (cut)", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

// ---------- nifm / nsd: offline ----------
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "offline: the local network mode switch is ignored", void, h_nifm_LocalMode, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "offline: the network request is not submitted (returns at once, IsNetworkAvailable is always false)", void, h_nifm_SubmitWait, void) {}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "offline: no IP configuration", Result, h_nifm_IpCfg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
// SDK 10's functions of the same name take socket::InAddr / in_addr, exactly the other way round from the generation above and in hle_net.c (Super Mario 3D World + Bowser's Fury)
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "offline: no IP configuration (SDK 10 overload)", Result, h_nifm_IpCfg10, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "offline: no IP (SDK 10 overload)", Result, h_nifm_GetIp10, uint32_t *a) { if (a) *a = 0; return RES_FAIL; }
// SDK 21 (Star Fox): the IPv4 and IPv6 configurations at once, with their counts
HLE_FORCE("_ZN2nn4nifm18GetCurrentIpConfigEPNS0_10Ipv4ConfigEPiPNS0_10Ipv6ConfigES3_ii", "offline: no IP configuration (SDK 21 overload)", Result, h_nifm_IpCfg21,
          void *v4, int *n4, void *v6, int *n6, int c4, int c6) { (void)v4; (void)v6; (void)c4; (void)c6; if (n4) *n4 = 0; if (n6) *n6 = 0; return RES_FAIL; }
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "server environment name: retail lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }
// nsd::GetApplicationServerEnvironmentType returns oe::ServerEnvironmentType (0 dd1, 1 lp1, 2 sd1, 3 sp1, 4 dp1: Hogwarts Legacy's table at main+0x42e6c50) and asks nsd:a / nsd:u for it;
// the host serves neither, so the SDK aborts on the failed request (Result 0xe8d). oe::GetServerEnvironmentType is a branch through the sdk's PLT to this function in every sdk module on hand
// (18, Switch 1 and Switch 2 titles), so this one takeover answers both
HLE_FORCE("_ZN2nn3nsd35GetApplicationServerEnvironmentTypeEv", "server environment type: retail lp1 (1), as GetEnvironmentIdentifier; nsd is not served", int, h_nsd_AppSrvEnv, void) { return 1; }

// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "friends library allocator; no session is created, ignored", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "friend list is empty (cut)", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
// Super Kirby Clash (SDK 7.4) requests account IDs rather than Friend objects. These
// overloads need the same offline cut: no service connection, no entries, and failure.
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdEiiRKNS0_12FriendFilterE", "friend ID list is empty (cut)", Result, h_fr_IdList,
          int *count, void *out, int off, int n, const void *filt) { (void)out; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEiiRKNS0_12FriendFilterE", "friend ID list is empty (cut)", Result, h_fr_IdList2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "friend list unavailable (cut)", Result, h_fr_ListAvail, uint8_t *out, const void *uid) { (void)uid; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "friend list (cut): the request fails at once", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "async context: always done", Result, h_fr_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "play history is not registered (cut)",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; return RES_FAIL; }

// ---------- ngc: newer SDK overloads (with bool). Always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "profanity filter init; the host has no word list", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "profanity check: no profanity in anything", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words; for (uint64_t i = 0; out && i < n; i++) out[i] = 0; return RES_OK;
}
// The China-region checks (Cruis'n Blast's name entry after a race) go to the ngct:u service through nn::ngc::t::Match, which the host does not serve
HLE_FORCE("_ZN2nn3ngc47CheckProfanityWordsInTextForPlatformRegionChinaEPbPKc", "China-region profanity check (ngct:u): no profanity", Result, h_ngc_CheckCn, uint8_t *out, const char *text) { (void)text; if (out) *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc46MaskProfanityWordsInTextForPlatformRegionChinaEPc", "China-region profanity masking (ngct:u): nothing is masked", Result, h_ngc_MaskCn, char *text) { (void)text; return RES_OK; }

// ---------- err: error display applet. Only logged, no UI is shown ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "error display applet; the host only logs it", void, h_err_ShowCode, uint64_t code) { DM_APPLET("error"); LOG("game asks to show an error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "error display applet; the host only logs it", void, h_err_ShowVar, const void *v) { DM_APPLET("error"); (void)v; LOG("game asks to show an error (ErrorResultVariant)"); }
HLE_FORCE("_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", "add-on content version mismatch error applet; the host only logs it", void, h_err_AocVer, void) { DM_APPLET("error"); LOG("game asks to show an error: add-on content version mismatch"); }
// A Switch 2 Edition's program asks it when its upgrade add-on is older than it needs: Super Mario Bros. Wonder's Switch 2 program 1.2.1 reads aocup:/System/AocUpgradeVersion.txt and stops
// below 3 (main+0x132b9b4); the console then asks the player to update the add-on. Unhooked, the SDK's applet call crashed on a null handle (sdk StartLibraryAppletImmediately)
HLE_FORCE("_ZN2nn3err50ShowUnacceptableAddOnContentForUpgradeVersionErrorEv", "upgrade add-on too old error applet; the host only logs it", void, h_err_AocUpgVer, void) {
    DM_APPLET("error"); LOG("[RED] game asks to show an error: its upgrade add-on content is older than this program needs (pack the add-on of the same version as the game)");
}
HLE_FORCE("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "game version mismatch error applet; the host only logs it", void, h_err_AppVer, void) { DM_APPLET("error"); LOG("game asks to show an error: game version mismatch"); }
// Pokémon Legends: Z-A (SDK 19): the application's own error screen (an error code number and two messages in the argument) goes through the error applet as well
HLE_FORCE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet; the host only logs it", void, h_err_ShowApp, const void *arg) { DM_APPLET("error"); (void)arg; LOG("game asks to show an application error (ApplicationErrorArg)"); }
// Pokémon Sword (SDK 7.3.2): the error applet's network service form, after a failed online call
HLE_FORCE("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", "network service error applet; the host only logs it", void, h_err_ShowNet, uint64_t code) { DM_APPLET("error"); LOG("game asks to show a network service error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }

// SDK 7.4 also takes a Result here, not the separate ErrorCode pair. Keep the
// host's log-only network error behavior rather than entering the absent applet service.
HLE_FORCE("_ZN2nn3err23ShowNetworkServiceErrorENS_6ResultE", "network service error applet (Result); the host only logs it", void, h_err_ShowNetRes, Result r) {
    DM_APPLET("error"); LOG("err::ShowNetworkServiceError Result=%#x (module %u, description %u)", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- pdm: the system's play log ----------
// Pokémon Sword asks at boot how often this user has launched two other games (main+0xc88074, one ID each call, 0x18 bytes {application ID, play time, launch count} per entry; the result is
// not read, a launch count of at least 1 is): the host keeps no play log, so every one was never played. The SDK's own call opens the pdm service (null read at sdk+0x336ccc)
HLE_FORCE("_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsERKNS_7account3UidEPKNS_13ApplicationIdEi", "play statistics of other games: never played (the host keeps no play log)",
          Result, h_pdm_QueryStats, uint64_t *out, const void *uid, const uint64_t *ids, int n) {
    (void)uid; for (int i = 0; out && ids && i < n; i++) { out[3 * i] = ids[i]; out[3 * i + 1] = 0; out[3 * i + 2] = 0; }
    return RES_OK;
}
// The overload without a Uid returns the number of records written. In SDK 8 it gets that count through the applet proxy, which is null on the host.
// With no system play log there are zero records (Persona 5 Strikers checks the count at main+0x10e78c before reading the array).
HLE_FORCE("_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsEPKNS_13ApplicationIdEi", "play statistics: empty system play log",
          int, h_pdm_QueryStatsAll, void *out, const uint64_t *ids, int n) {
    (void)out; (void)ids; (void)n;
    return 0;
}

// ---------- web: web page applet (online lobby). Unavailable ----------
HLE_FORCE("_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", "online lobby web applet (cut)", Result, h_web_Lobby, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowLobbyPage (cut)"); return RES_FAIL; }
// Tears of the Kingdom imports the general web page applet too (the game constructs the return value itself, so it stays as constructed)
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web page applet (cut)", Result, h_web_Page, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowWebPage (cut)"); return RES_FAIL; }
// Octopath Traveler 0 imports the offline HTML page applet too (pages shipped in the game's HTML document NCA, which the host does not extract)

// ---------- C interface (nnsocket* / nnssl*): offline as well. The arguments are all in registers and the host does not look at them ----------
HLE_FORCE("nnsocketAccept", "offline (cut)", int, h_c_sk_Accept, void) { return -1; }
HLE_FORCE("nnsocketListen", "offline (cut)", int, h_c_sk_Listen, void) { return -1; }
HLE_FORCE("nnsocketRecvMsg", "offline (cut)", int, h_c_sk_RecvMsg, void) { return -1; }
HLE_FORCE("nnsocketSendMsg", "offline (cut)", int, h_c_sk_SendMsg, void) { return -1; }
HLE_FORCE("nnsocketShutdown", "local UDP (offline BSD service)", int, h_c_sk_Shutdown, int s, int h) { return dm_sk_shutdown(s, h); }
HLE_FORCE("nnsocketGetAddrInfoWithOptions", "name resolution with resolver options (cut): EAI_FAIL", int, h_c_sk_GaiOpt, const char *h, const char *s, const void *hint, void **res, const void *o, unsigned long n) { (void)h; (void)s; (void)hint; (void)o; (void)n; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketGetAddrInfo", "name resolution (cut): EAI_FAIL", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketFreeAddrInfo", "pairs with GetAddrInfo (cut)", void, h_c_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("nnsocketGAIStrError", "resolver error text", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }

// ---------- bcat: news / data delivered from the server (Super Mario Odyssey). Initialize talks to the bcat service (an SDK abort without it); mounting the delivery cache fails,
// which the game treats as "nothing delivered" (main+0x6c9258: a failure skips the read unless the caller asked for an error dialog, which only logs here). Syncing needs the network, which nifm reports unavailable ----------
HLE_FORCE("_ZN2nn4bcat10InitializeEv", "data delivery init (connects to the bcat service); the host has none, no-op", void, h_bcat_Init, void) {}
HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "mount delivery cache: no delivery data, fails", Result, h_bcat_Mount, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "unmount delivery cache", void, h_bcat_Unmount, void) {}

#if DM_GAME_S2_HID_STUBS   // profile key s2_hid_stubs (game.py): these inert stubs replace constructors too, so a game with real Switch 2 vibration support (Mario Kart World) must not get them
// ---------- Pokopia (SDK 20.5.25): the Switch 2 calls nxnative main does not force yet (its hle_misc.c has nn::streamplay, its hle_hid.c the Joy-Con 2 mouse, sensor and vibration node) ----------
HLE_FORCE("_ZN2nn10streamplay12CloseSessionEv", "SWITCH2 streamplay: close session; none open", Result, h_stpl_Close, void) { return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay14StartStreamingEv", "SWITCH2 streamplay: start streaming; reports success, nothing streams", Result, h_stpl_Start, void) { return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay21WriteGuestAudioStreamENS0_7GuestIdEPKvi", "SWITCH2 streamplay: guest audio; dropped", Result, h_stpl_GuestAudio, int guest, const void *p, int n) { (void)guest; (void)p; (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay13GetGuestNsaIdEPNS_7account23NetworkServiceAccountIdENS0_7GuestIdE", "SWITCH2 streamplay: guest network account id; zero", Result, h_stpl_GuestNsa, uint64_t *out, int guest) { (void)guest; if (out) *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay16GetGuestNicknameEPNS_7account8NicknameENS0_7GuestIdE", "SWITCH2 streamplay: guest nickname; empty", Result, h_stpl_GuestNick, char *out, int guest) { (void)guest; if (out) *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn10streamplay31OpenSessionWithMatchmakeSupportEPNS0_11SessionInfoERKNS0_15MatchmakeConfigE", "SWITCH2 streamplay: open a session; zeroed info, reports success", Result, h_stpl_OpenSess, uint8_t *info, const void *cfg) { (void)cfg; if (info) memset(info, 0, 0x40); return RES_OK; }
static _Alignas(16) uint8_t g_stpl_ev[0x100];   // connection-state event that never fires (bound on the guest thread on first use, as hle_friends.c does)

// ---------- SWITCH2 (private bring-up, Pokopia SDK 20.5.25): Switch 2 hid additions the host has no device for ----------
// Nmouse is the Joy-Con 2 mouse mode, Jcms its sensor; the SDK connects to the hid service in InitializeNmouse (sm 0xe15 → abort). The host reports no mouse and no sensor.
// The Switch 2 vibration graph (nodes, players, converters): the host plays no HD rumble 2 waveforms; every object is inert
HLE_FORCE("_ZN2nn3hid13VibrationNode6UpdateEv", "SWITCH2 hid: vibration node update; inert", void, h_hid_VnUpdate, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid13VibrationNode15SetModulationToEPKS1_RKNS0_19VibrationModulationE", "SWITCH2 hid: vibration modulation; inert", void, h_hid_VnMod, void *self, const void *to, const void *m) { (void)self; (void)to; (void)m; }
HLE_FORCE("_ZN2nn3hid23VibrationNodeConnectionC1Ev", "SWITCH2 hid: vibration connection ctor; inert", void, h_hid_VcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid23VibrationNodeConnectionD1Ev", "SWITCH2 hid: vibration connection dtor; inert", void, h_hid_VcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid23VibrationNodeConnection7ConnectEPNS0_13VibrationNodeES3_", "SWITCH2 hid: connect vibration nodes; reports success", Result, h_hid_VcConnect, void *self, void *a, void *b) { (void)self; (void)a; (void)b; return RES_OK; }
HLE_FORCE("_ZN2nn3hid23VibrationNodeConnection10DisconnectEv", "SWITCH2 hid: disconnect vibration nodes; inert", void, h_hid_VcDisc, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayerC1Ev", "SWITCH2 hid: vibration player ctor; inert", void, h_hid_VpCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayerD1Ev", "SWITCH2 hid: vibration player dtor; inert", void, h_hid_VpDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayer4LoadEPKvm", "SWITCH2 hid: vibration player load; reports success", Result, h_hid_VpLoad, void *self, const void *p, uint64_t n) { (void)self; (void)p; (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayer4PlayEv", "SWITCH2 hid: vibration player play; inert", void, h_hid_VpPlay, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayer4StopEv", "SWITCH2 hid: vibration player stop; inert", void, h_hid_VpStop, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayer7SetLoopEb", "SWITCH2 hid: vibration player loop; inert", void, h_hid_VpLoop, void *self, unsigned on) { (void)self; (void)on; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayer15SetLoopIntervalEi", "SWITCH2 hid: vibration player loop interval; inert", void, h_hid_VpLoopIv, void *self, int n) { (void)self; (void)n; }
HLE_FORCE("_ZN2nn3hid15VibrationPlayer18SetCurrentPositionEi", "SWITCH2 hid: vibration player position; inert", void, h_hid_VpPos, void *self, int n) { (void)self; (void)n; }
HLE_FORCE("_ZN2nn3hid15VibrationTargetC1Ev", "SWITCH2 hid: vibration target ctor; inert", void, h_hid_VtCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationTargetD1Ev", "SWITCH2 hid: vibration target dtor; inert", void, h_hid_VtDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid15VibrationTarget24GetVibrationDeviceHandleEv", "SWITCH2 hid: vibration target device handle; 0", uint64_t, h_hid_VtGet, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3hid15VibrationTarget24SetVibrationDeviceHandleERKNS0_21VibrationDeviceHandleE", "SWITCH2 hid: vibration target device handle; ignored", void, h_hid_VtSet, void *self, const void *h) { (void)self; (void)h; }
HLE_FORCE("_ZN2nn3hid18VibrationConverterC1Ei", "SWITCH2 hid: vibration converter ctor; inert", void, h_hid_VcvCtor, void *self, int rate) { (void)self; (void)rate; }
HLE_FORCE("_ZN2nn3hid18VibrationConverterD1Ev", "SWITCH2 hid: vibration converter dtor; inert", void, h_hid_VcvDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3hid18VibrationConverter12ProcessChunkEPNS0_14VibrationValueEPKsi", "SWITCH2 hid: vibration converter; 0 values", int, h_hid_VcvChunk, void *self, void *out, const short *pcm, int n) { (void)self; (void)out; (void)pcm; (void)n; return 0; }
#endif   // DM_GAME_S2_HID_STUBS
