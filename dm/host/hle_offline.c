// Local IPv4 sockets and the LDN bridge are opt-in; other peripheral/online services retain their offline fallbacks.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
#include "hle.h"
#include "net_socket.h"



// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "telemetry report save (user); nothing is sent, reports success", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "telemetry report save; nothing is sent, reports success", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "telemetry init: the original connects to the prepo service (through sm), the host has no service process; no-op (Engage calls it explicitly)", Result, h_prepo_Init, void) { return RES_OK; }
// Add the session ID / connection status to a report: on hardware these first connect to the prepo service for the session (Super Mario 3D World + Bowser's Fury calls them entering the save select); the host adds nothing and reports success
HLE_FORCE("_ZN2nn5prepo12AddSessionIdEPNS0_10PlayReportEPKc", "telemetry session ID: no service connection, nothing added, reports success", Result, h_prepo_AddSess, void *rep, const char *key) { (void)rep; (void)key; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "telemetry internet connection status: no service connection, nothing added, reports success", Result, h_prepo_AddNet, void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "telemetry immediate upload; nothing is sent, reports success", Result, h_prepo_Tx, void) { return RES_OK; }

// nn::ldn lives in hle_ldn.c (explicit DM_NETWORK=ldn).

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

// ---------- socket: offline by default. Explicit Mac LAN uses the translated host transport ----------
enum { SK_ENETDOWN = 100, SK_EAI_FAIL = 4 };
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE_WAIT("_ZN2nn6socket8FinalizeEv", "release the host's LAN descriptors, if enabled", Result, h_sk_Fini, void) { dm_net_finalize(); return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "the game's errno in a network mode (as detail::GetLastErrno reads it); ENETDOWN otherwise", int, h_sk_Errno, void) { return dm_net_enabled() ? dm_net_guest_errno_get() : SK_ENETDOWN; }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "socket transport, offline by default", int, h_sk_Socket3i, int a, int b, int c) { return dm_net_guest_errno(dm_net_socket(a, b, c)); }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "socket transport, offline by default", int, h_sk_Shutdown, int s, int h) { return dm_net_guest_errno(dm_net_shutdown(s, h)); }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "socket transport, offline by default", int, h_sk_Bind, int s, const void *a, unsigned n) { return dm_net_guest_errno(dm_net_bind(s, a, n)); }
int dm_sk_poll(void *fds, uint64_t n, int ms);   // hle_net.c: nothing to wait on → the timeout passes, 0; else -1
int dm_sk_select(int nfds, const uint64_t *r, const uint64_t *w, const uint64_t *e, const int64_t *tv);
HLE_FORCE_WAIT("_ZN2nn6socket4PollEP6pollfdmi", "offline: waits out the timeout when there is nothing to wait on, else fails", int, h_sk_PollPlain, void *p, uint64_t n, int t) { return dm_net_guest_errno(dm_sk_poll(p, n, t)); }
// SDK 4's Select with plain fd_set / timeval (Dark Souls Remastered)
HLE_FORCE_WAIT("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", "offline: waits out the timeout when there is nothing to wait on, else fails", int, h_sk_Select2, int n, void *r, void *w, void *e, void *t) { return dm_net_guest_errno(dm_sk_select(n, r, w, e, t)); }
HLE_FORCE_WAIT("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "socket transport, offline by default", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return dm_net_guest_errno(dm_net_send(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "socket transport, offline by default", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return dm_net_guest_errno(dm_net_send(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "socket transport, offline by default", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_net_guest_errno(dm_net_recv(s, b, n, f, a, al)); }
HLE_FORCE_WAIT("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "SDK 4 addrinfo layout; name resolution, offline by default", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) { return dm_net_getaddrinfo_legacy(h, s, hint, res); }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "release only resolver allocations owned by the host", void, h_sk_FreeAi2, void *ai) { dm_net_freeaddrinfo(ai); }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "resolver error text", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
// SDK 11's overloads with plain sockaddr / int arguments (Big Brain Academy: Brain vs. Brain, through Pia)
HLE_FORCE_WAIT("_ZN2nn6socket7ConnectEiPK8sockaddrj", "socket transport, offline by default", int, h_sk_Connect2, int s, const void *a, unsigned n) { return dm_net_guest_errno(dm_net_connect(s, a, n)); }
HLE_FORCE_WAIT("_ZN2nn6socket6AcceptEiP8sockaddrPj", "socket transport, offline by default", int, h_sk_Accept2, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_accept(s, a, n)); }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", "socket transport, offline by default", int, h_sk_GetSockName2, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 0)); }
HLE_FORCE("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", "SDK 4 socket transport, offline by default", int, h_sk_GetPeerName4, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_name(s, a, n, 1)); }
HLE_FORCE_WAIT("_ZN2nn6socket13GetHostByNameEPKc", "SDK 4 resolver, offline by default", void *, h_sk_GetHostByName4, const char *name) { return dm_net_gethostbyname(name); }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiiiPvPj", "socket transport, offline by default", int, h_sk_GetSockOpt2, int s, int l, int o, void *v, unsigned *n) { return dm_net_guest_errno(dm_net_sockopt(s, l, o, v, n, 0)); }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiiiPKvj", "socket transport, offline by default", int, h_sk_SetSockOpt2, int s, int l, int o, const void *v, unsigned n) { return dm_net_guest_errno(dm_net_sockopt(s, l, o, (void *)v, &n, 1)); }
HLE_FORCE("_ZN2nn6socket5FcntlEiiz", "socket transport; Switch variadic argument is in x2", int, h_sk_Fcntl2, int s, int c, int flags) { return dm_net_guest_errno(dm_net_fcntl(s, c, flags)); }
HLE_FORCE_WAIT("_ZN2nn6socket4RecvEiPvmi", "socket transport, offline by default", long, h_sk_Recv2, int s, void *b, unsigned long n, int f) { return dm_net_guest_errno(dm_net_recv(s, b, n, f, NULL, NULL)); }
HLE_FORCE_WAIT("_ZN2nn6socket4SendEiPKvmi", "socket transport, offline by default", long, h_sk_Send2, int s, const void *b, unsigned long n, int f) { return dm_net_guest_errno(dm_net_send(s, b, n, f, NULL, 0)); }

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

// nn::ssl lives in hle_ssl.c; it remains unavailable unless networking is enabled.

// ---------- nifm / nsd: explicit LAN uses host IP configuration; NSD retains its offline boundary ----------
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "host network mode is selected explicitly at launch, not by the guest", void, h_nifm_LocalMode, unsigned on) { (void)on; }
HLE_FORCE_WAIT("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "offline by default; explicit LAN waits for the actual request state", void, h_nifm_SubmitWait, void) { dm_nifm_submit_wait(); }
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "actual host IPv4 configuration in explicit LAN mode; unavailable otherwise", Result, h_nifm_IpCfg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    return dm_net_ipconfig(a, b, c, d, e) ? RES_FAIL : RES_OK;
}
// SDK 10's functions of the same name take socket::InAddr / in_addr, exactly the other way round from the generation above and in hle_net.c (Super Mario 3D World + Bowser's Fury)
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", "actual host IPv4 configuration in explicit LAN mode (SDK 10 overload)", Result, h_nifm_IpCfg10, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    return dm_net_ipconfig(a, b, c, d, e) ? RES_FAIL : RES_OK;
}
HLE_FORCE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", "actual LAN IPv4 address, offline by default", Result, h_nifm_GetIp10, uint32_t *a) { if (!a) return RES_FAIL; *a = 0; return dm_net_ipconfig(a, NULL, NULL, NULL, NULL) ? RES_FAIL : RES_OK; }
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "server environment name: retail lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }

// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "friends library allocator; no session is created, ignored", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }




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
HLE_FORCE("_ZN2nn3err39ShowUnacceptableApplicationVersionErrorEv", "game version mismatch error applet; the host only logs it", void, h_err_AppVer, void) { DM_APPLET("error"); LOG("game asks to show an error: game version mismatch"); }
// Pokémon Legends: Z-A (SDK 19): the application's own error screen (an error code number and two messages in the argument) goes through the error applet as well
HLE_FORCE("_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", "application error applet; the host only logs it", void, h_err_ShowApp, const void *arg) { DM_APPLET("error"); (void)arg; LOG("game asks to show an application error (ApplicationErrorArg)"); }
// Pokémon Sword (SDK 7.3.2): the error applet's network service form, after a failed online call
HLE_FORCE("_ZN2nn3err23ShowNetworkServiceErrorENS0_9ErrorCodeE", "network service error applet; the host only logs it", void, h_err_ShowNet, uint64_t code) { DM_APPLET("error"); LOG("game asks to show a network service error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }

// ---------- pdm: the system's play log ----------
// Pokémon Sword asks at boot how often this user has launched two other games (main+0xc88074, one ID each call, 0x18 bytes {application ID, play time, launch count} per entry; the result is
// not read, a launch count of at least 1 is): the host keeps no play log, so every one was never played. The SDK's own call opens the pdm service (null read at sdk+0x336ccc)
HLE_FORCE("_ZN2nn3pdm30QueryApplicationPlayStatisticsEPNS0_25ApplicationPlayStatisticsERKNS_7account3UidEPKNS_13ApplicationIdEi", "play statistics of other games: never played (the host keeps no play log)",
          Result, h_pdm_QueryStats, uint64_t *out, const void *uid, const uint64_t *ids, int n) {
    (void)uid; for (int i = 0; out && ids && i < n; i++) { out[3 * i] = ids[i]; out[3 * i + 1] = 0; out[3 * i + 2] = 0; }
    return RES_OK;
}

// ---------- web: web page applet (online lobby). Unavailable ----------
HLE_FORCE("_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE", "online lobby web applet (cut)", Result, h_web_Lobby, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowLobbyPage (cut)"); return RES_FAIL; }
// Tears of the Kingdom imports the general web page applet too (the game constructs the return value itself, so it stays as constructed)
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web page applet (cut)", Result, h_web_Page, void *rv, const void *arg) { (void)rv; (void)arg; LOG("web::ShowWebPage (cut)"); return RES_FAIL; }
// Octopath Traveler 0 imports the offline HTML page applet too (pages shipped in the game's HTML document NCA, which the host does not extract)
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "offline HTML page applet (cut)", Result, h_web_Offline, void *rv, const void *arg) {
    (void)rv; (void)arg; LOG("web::ShowOfflineHtmlPage (cut)"); return RES_FAIL;
}

// ---------- C socket interface; transport stays offline unless a network mode is selected ----------
HLE_FORCE_WAIT("nnsocketAccept", "socket transport, offline by default", int, h_c_sk_Accept, int s, void *a, unsigned *n) { return dm_net_guest_errno(dm_net_accept(s, a, n)); }
HLE_FORCE("nnsocketListen", "socket transport, offline by default", int, h_c_sk_Listen, int s, int n) { return dm_net_guest_errno(dm_net_listen(s, n)); }
// RecvMsg / SendMsg (gRPC's TCP endpoint does all its reads and writes with them: nn::npln, Super Mario Bros. Wonder). Horizon's msghdr is FreeBSD's, 0x30 bytes: name +0x00,
// namelen +0x08, iov +0x10, iovlen (int) +0x18, control +0x20, controllen +0x28, flags +0x2c; SDK 16's detail::RecvMsg (Wonder 1.2.1 sdk+0x14cf70) copies those 0x30 bytes into an
// mmsghdr and calls RecvMMsg with one message. An iovec is a base and a length. The data goes through one host buffer: gathered before a send, scattered after a receive. No ancillary
// data is carried: a receive reports none (controllen 0), and a send that has some fails with EOPNOTSUPP rather than drop it. The buffer holds at most SK_MSG_MAX bytes: on a
// stream socket a longer message moves in part, as a real socket may move less than asked (the caller sends or reads the rest); no datagram is that long, so one fails with
// EMSGSIZE. A datagram longer than the iovecs is cut to them and the receive reports MSG_TRUNC (0x10), as FreeBSD's does: the host asks for one byte more to see it
typedef struct { void *name; uint32_t namelen, pad0; struct { void *base; uint64_t len; } *iov; int32_t iovlen, pad1; void *control; uint32_t controllen; int32_t flags; } sk_msghdr;
_Static_assert(sizeof(sk_msghdr) == 0x30 && offsetof(sk_msghdr, iov) == 0x10 && offsetof(sk_msghdr, controllen) == 0x28 && offsetof(sk_msghdr, flags) == 0x2c, "Horizon msghdr");
enum { SK_MSG_IOV_MAX = 1024, SK_MSG_MAX = 16 << 20 };
enum { SK_MSG_TRUNC = 0x10 };
static long sk_msg_size(const sk_msghdr *m, int datagram) {
    if (!m || m->iovlen < 0 || m->iovlen > SK_MSG_IOV_MAX || (m->iovlen && !m->iov)) { dm_net_set_errno(22); return -1; }
    uint64_t total = 0;   // stops at SK_MSG_MAX + 1, whatever the lengths add up to
    for (int i = 0; i < m->iovlen; i++) {
        if (m->iov[i].len && !m->iov[i].base) { dm_net_set_errno(14); return -1; }
        if (total <= SK_MSG_MAX) total = m->iov[i].len > SK_MSG_MAX - total ? SK_MSG_MAX + 1ull : total + m->iov[i].len;
    }
    if (total > SK_MSG_MAX) { if (datagram) { dm_net_set_errno(40); return -1; } total = SK_MSG_MAX; }
    return (long)total;
}
static long sk_recvmsg(int s, sk_msghdr *m, int f) {
    int datagram = dm_net_socket_type(s) == 2; long total = sk_msg_size(m, datagram); if (total < 0) return -1;
    uint8_t *buffer = malloc((size_t)total + 1); if (!buffer) { dm_net_set_errno(12); return -1; }
    unsigned namelen = m->name ? m->namelen : 0;
    long got = dm_net_recv(s, buffer, (unsigned long)total + (unsigned long)datagram, f, m->name, m->name ? &namelen : NULL);
    if (got >= 0) {
        int cut = got > total; if (cut) got = total;
        long left = got; for (int i = 0; i < m->iovlen && left > 0; i++) { uint64_t n = m->iov[i].len < (uint64_t)left ? m->iov[i].len : (uint64_t)left; memcpy(m->iov[i].base, buffer + (got - left), n); left -= (long)n; }
        if (m->name) m->namelen = namelen; m->controllen = 0; m->flags = cut ? SK_MSG_TRUNC : 0;
    }
    free(buffer); return got;
}
HLE_FORCE_WAIT("nnsocketRecvMsg", "scatter receive through one host buffer; no ancillary data", long, h_c_sk_RecvMsg, int s, sk_msghdr *m, int f) { return dm_net_guest_errno(sk_recvmsg(s, m, f)); }
static long sk_sendmsg(int s, const sk_msghdr *m, int f) {
    long total = sk_msg_size(m, dm_net_socket_type(s) == 2); if (total < 0) return -1;
    if (m->control && m->controllen) { dm_net_set_errno(95); static int told; if (!told++) LOG("[RED] socket: SendMsg with ancillary data (%u bytes): not carried, the send fails", m->controllen); return -1; }
    uint8_t *buffer = malloc(total ? (size_t)total : 1); if (!buffer) { dm_net_set_errno(12); return -1; }
    long at = 0; for (int i = 0; i < m->iovlen && at < total; i++) { long n = m->iov[i].len < (uint64_t)(total - at) ? (long)m->iov[i].len : total - at; memcpy(buffer + at, m->iov[i].base, (size_t)n); at += n; }
    long sent = dm_net_send(s, buffer, (unsigned long)total, f, m->name, m->name ? m->namelen : 0);
    free(buffer); return sent;
}
HLE_FORCE_WAIT("nnsocketSendMsg", "gather send through one host buffer; ancillary data is refused", long, h_c_sk_SendMsg, int s, const sk_msghdr *m, int f) { return dm_net_guest_errno(sk_sendmsg(s, m, f)); }
// The C resolver takes the C struct addrinfo, the type of the plain-addrinfo C++ overload: in Wonder 1.2.1's SDK nnsocketGetAddrInfo (sdk+0x1481c0) branches straight to
// GetAddrInfo(const char *, const char *, const addrinfo *, addrinfo **, const ResolverOption *, size_t), and that SDK's decoders put the address at +0x20 for addrinfo
// (DNSSerializer::FromBufferInternal<addrinfo>, sdk+0x1493f0) and at +0x18 for nn::socket::AddrInfo (sdk+0x148f60). So it gets that overload's layout whenever the SDK has
// it, as h_sk_Gai2 serves it, and the AddrInfo layout only in an SDK without it. An SDK with neither fails. Resolver options are not applied (the host resolver has none)
static int sk_c_gai(const char *h, const char *s, const void *hint, void **res) {
    if (guest_sym("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_")) return dm_net_getaddrinfo_legacy(h, s, hint, res);
    if (guest_sym("_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_")) return dm_net_getaddrinfo(h, s, hint, res);
    if (res) *res = NULL; return SK_EAI_FAIL;
}
HLE_FORCE_WAIT("nnsocketGetAddrInfoWithOptions", "name resolution in the C struct addrinfo layout; resolver options not applied", int, h_c_sk_GaiOpt, const char *h, const char *s, const void *hint, void **res, const void *o, unsigned long n) {
    (void)o; (void)n; return sk_c_gai(h, s, hint, res);
}
HLE_FORCE_WAIT("nnsocketGetAddrInfo", "name resolution in the C struct addrinfo layout", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { return sk_c_gai(h, s, hint, res); }
HLE_FORCE("nnsocketShutdown", "socket transport, offline by default", int, h_c_sk_Shutdown, int s, int h) { return dm_net_guest_errno(dm_net_shutdown(s, h)); }
HLE_FORCE("nnsocketFreeAddrInfo", "release only resolver allocations owned by the host", void, h_c_sk_FreeAi, void *ai) { dm_net_freeaddrinfo(ai); }
HLE_FORCE("nnsocketGAIStrError", "resolver error text", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }

// BCAT initialization retains the existing offline contract. Validated optional
// delivery snapshots are mounted by hle_fs.c; missing data still fails there.
HLE_FORCE("_ZN2nn4bcat10InitializeEv", "initialize optional data delivery, with no implied network/cache success", void, h_bcat_Init, void) {}

// ---------- album: the capture album (Monster Hunter Rise saves its own photo-mode pictures there). Initialize connects to the capture service (capsrv, an abort in sm::Initialize at the start of
// nnMain, main+0x6bc5478); the host has no album, so saving fails and showing it does nothing. GetImageWidth / GetImageHeight only compute and keep running in the SDK ----------
HLE_FORCE("_ZN2nn5album10InitializeEv", "capture album init (capsrv); the host has no album, no-op", void, h_album_Init, void) {}
HLE_FORCE("_ZN2nn5album8FinalizeEv", "capture album", void, h_album_Fini, void) {}
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save,
          const void *p, size_t n, int size, int orient, int report) { (void)p; (void)n; (void)size; (void)orient; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
// The SDK 7 overload, without an orientation (DRAGON QUEST XI S)
HLE_FORCE("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", "save a picture to the album: no album, fails", Result, h_album_Save7,
          const void *p, size_t n, int size, int report) { (void)p; (void)n; (void)size; (void)report; LOG("album::SaveScreenshot (no album)"); return RES_FAIL; }
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_Share,
          const void *p, size_t n, int size, int orient, int report, const void *uid, const char *text) { (void)p; (void)n; (void)size; (void)orient; (void)report; (void)uid; (void)text; return RES_FAIL; }
// The SDK 20 overload that hands back a share ID, and the URL of the last post (Animal Crossing: New Horizons 3.0.3)
HLE_FORCE("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc", "save and share a picture: no album, fails", Result, h_album_ShareId,
          void *id, const void *p, size_t n, int size, int report, const void *uid, const char *text) { (void)id; (void)p; (void)n; (void)size; (void)report; (void)uid; (void)text; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE", "URL of the last shared post: nothing was shared, fails", Result, h_album_ShareUrl, char *url, size_t n, const void *id) {
    (void)id; if (url && n) url[0] = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn5album14ShowAlbumFilesEv", "album applet: not shown", Result, h_album_Show, void) { return RES_FAIL; }
// Pokémon Legends: Z-A (SDK 19): a screenshot of the current frame, taken by the capture service
HLE_FORCE("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE", "screenshot of the current frame (capsrv): no album, fails", Result, h_album_SaveCur, int report) { (void)report; LOG("album::SaveCurrentScreenshot (no album)"); return RES_FAIL; }
