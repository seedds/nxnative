// ldn: local wireless, taken over for Link Play. Cut as in hle_offline.c (initialization fails, state is always "none") unless co-op is on: DM_COOP=auto (the launcher's Link Play switch)
// or join:<IPv4>[:<port>] (env.txt). Then every call goes to the coop module (hle_coop_*.c, MIT: windows-native src/coop, docs/coop-step1c.md), Link Play "Local connection" between our
// builds on the LAN. The state-change event is bound as before and signalled by the module. Its own file so hle_offline.c stays standalone for dm/host/tests (t_server_environment includes it)
#include "hle.h"
#include "coop.h"
#include <stdatomic.h>
static void *_Atomic g_ldn_attached;   // the game's own state-change event (AttachStateChangeEvent; Pokopia attaches none and polls)
static void coop_hlog(void *u, const char *l) { (void)u; LOG("[coop] %s", l); }
static void coop_hsleep(void *u, int64_t ns) { (void)u; void dm_guest_sleep_ns(int64_t ns); dm_guest_sleep_ns(ns); }
static void coop_hstate(void *u, int st) { (void)u; (void)st; void *e = atomic_load(&g_ldn_attached); if (e) hle_signal_bound_system_event(e); }
static const char *coop_hbuild(void *u) { (void)u; return dm_main_build(); }   // peers compare builds before a join
// The cheats that are on (hle_cheats.c), "Interact everywhere" (DM_COOP_INTERACT, hle_coop_interact.c) and Story co-op (DM_COOP_STORY) as one fingerprint:
// players meet only with the same set, since each changes what happens in the host's town. Without cheats it is the Windows build's fingerprint
int dm_cheats_master(void); int dm_cheats_ncheats(void); int dm_cheat_on_at(int i); const char *dm_cheat_name_at(int i);
static uint64_t coop_hmods(void *u, char *what, size_t n) {
    (void)u; uint64_t h = 0; int k = 0; const coop_site *s;
    for (int i = 0; dm_cheats_master() && i < dm_cheats_ncheats(); i++) {
        if (!dm_cheat_on_at(i)) continue;
        h = coop_mods_add(h, dm_cheat_name_at(i)); k++;
    }
    int ie = coop_interact_on() && coop_interact_sites(dm_main_build(), &s), st = ie && coop_story_on() && coop_story_sites(dm_main_build(), &s);
    if (ie) h = coop_mods_add(h, COOP_INTERACT_MOD);
    if (st) h = coop_mods_add(h, COOP_STORY_MOD);
    const char *mods = st ? COOP_INTERACT_MOD ", " COOP_STORY_MOD : ie ? COOP_INTERACT_MOD : "";
    if (k) snprintf(what, n, "%d cheat%s on%s%s", k, k == 1 ? "" : "s", ie ? ", " : "", mods);
    else if (ie) snprintf(what, n, "%s", mods);
    return h;
}
// "Interact everywhere" and Story co-op: the profile's observation gates (patches.py, OBS_COOP_INTERACT with the site's id in the gate number's upper bits)
// call the shared callback while the switch is on, read when a gate fires (the iPhone's launcher sets it after the host's constructors have run)
static void coop_interact_gate(uint64_t *regs, int param) {
    static _Atomic int said, said_talk;
    if (!coop_interact_on()) return;
    uint64_t was13 = regs[13], was0 = regs[0]; coop_interact_site(param, regs);
    if (regs[13] != was13 && !atomic_exchange(&said, 1)) LOG("[coop] Interact everywhere: this story area's Link Play session has Palette Town's permissions");
    if (param == COOP_SITE_TALK && regs[0] != was0 && !atomic_exchange(&said_talk, 1)) LOG("[coop] Story co-op: a visitor's talk takes the host's path");
}
__attribute__((constructor)) static void coop_interact_attach(void) { gate_obs_set(OBS_COOP_INTERACT, coop_interact_gate); }
coop_node *dm_coop(void) {   // the process's coop node when DM_COOP is set (and not off), else NULL (hle_net.c and hle_misc.c ask too)
    static coop_node *_Atomic node; static _Atomic int done;
    if (atomic_load(&done)) return atomic_load(&node);
    static const coop_hooks h = {NULL, coop_hlog, coop_hsleep, coop_hstate, coop_hbuild, coop_hmods};
    coop_node *n = coop_env_node(&h);
    atomic_store(&node, n); atomic_store(&done, 1);
    return n;
}
#define COOP(call) do { coop_node *c_ = dm_coop(); if (c_) return call; } while (0)
HLE_FORCE("_ZN2nn3ldn10InitializeEv", "local wireless: the coop module (DM_COOP), else cut: initialization fails", Result, h_ldn_Init, void) { COOP(coop_ldn_initialize(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "local wireless (coop / cut)", void, h_ldn_Fini, void) { coop_node *c = dm_coop(); if (c) coop_ldn_finalize(c); }
HLE_FORCE("_ZN2nn3ldn8GetStateEv", "local wireless (coop / cut): state 0 (none)", int, h_ldn_State, void) { COOP(coop_ldn_state(c_)); return 0; }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "local wireless state event: bound; the coop module fires it on every state or network change, else never",
          void, h_ldn_Attach, void *ev) { hle_bind_system_event(ev, 0); atomic_store(&g_ldn_attached, ev); }
HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "local wireless (coop / cut)", Result, h_ldn_OpenSta, void) { COOP(coop_ldn_open_station(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "local wireless (coop / cut)", Result, h_ldn_CloseSta, void) { COOP(coop_ldn_close_station(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "local wireless (coop / cut)", Result, h_ldn_OpenAp, void) { COOP(coop_ldn_open_ap(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "local wireless (coop / cut)", Result, h_ldn_CloseAp, void) { COOP(coop_ldn_close_ap(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "local wireless (coop / cut)", Result, h_ldn_Disc, void) { COOP(coop_ldn_disconnect(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "local wireless (coop / cut)", Result, h_ldn_Destroy, void) { COOP(coop_ldn_destroy_network(c_)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan (coop: asks the hosts it knows; cut: 0 found)", Result, h_ldn_Scan, void *out, int *n, int max, const void *f, int ch) {
    COOP(coop_ldn_scan(c_, out, n, max, f, ch));
    (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "local wireless scan (coop: asks the hosts it knows; cut: 0 found)", Result, h_ldn_ScanP, void *out, int *n, int max, const void *f, int ch) {
    COOP(coop_ldn_scan(c_, out, n, max, f, ch));
    (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "local wireless (coop / cut)", Result, h_ldn_Create,
          const void *a, const void *b, const void *c) { COOP(coop_ldn_create_network(c_, a, b, c)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "local wireless (cut)",
          Result, h_ldn_CreateP, const void *a, const void *b, const void *c, const void *d, int n, const void *e) { (void)a; (void)b; (void)c; (void)d; (void)n; (void)e; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless (coop / cut)", Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) { COOP(coop_ldn_connect(c_, a, b, c, v, o)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "local wireless (cut)",
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) { (void)a; (void)b; (void)c; (void)d; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "local wireless (coop / cut)", Result, h_ldn_NetInfo, void *o) { COOP(coop_ldn_network_info(c_, o)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "local wireless (coop / cut)", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) {
    COOP(coop_ldn_ipv4(c_, a, m));
    if (a) *a = 0;
    if (m) *m = 0;
    return RES_FAIL;
}
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "local wireless (coop / cut)", Result, h_ldn_SecPrm, void *o) { COOP(coop_ldn_security_parameter(c_, o)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "local wireless (coop / cut)", Result, h_ldn_Adv, const void *d, uint64_t n) { COOP(coop_ldn_set_advertise(c_, d, n)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "local wireless (coop / cut): 0", int, h_ldn_DiscReason, void) { COOP(coop_ldn_disconnect_reason(c_)); return 0; }
HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "local wireless (cut)", Result, h_ldn_Filter, uint64_t mac) { (void)mac; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "local wireless (coop / cut)", Result, h_ldn_Policy, int p) { COOP(coop_ldn_set_accept_policy(c_, p & 0xFF)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "local wireless (coop / cut)", Result, h_ldn_Reject, uint32_t a) { COOP(coop_ldn_reject(c_, a)); return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", "local wireless (coop / cut)", Result, h_ldn_NetCfg, void *o) { COOP(coop_ldn_network_config(c_, o)); return RES_FAIL; }

