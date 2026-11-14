// Services whose client a game links into its own main (nn::sf::hipc), and sessions the SDK gets from another service (the Switch 2's camera, below): Animal Crossing: New Horizons 3.0.3 carries the clients of olsc:u (cloud save data backup settings,
// main+0x404bc6c) and npns (push notifications). The SDK's client code connects through sm (an abort in sm::Initialize under InitializeHipcServiceResolution, from main+0x5b8198) and talks
// HIPC through svcSendSyncRequest; the host has neither. It hands out sessions of its own instead and answers each request in the message buffer, for the services listed in sf_served:
// success, the output data zeroed, no objects or handles, unless the service has an answer of its own (nsd, below). For olsc that reads as "backup off for every user", so the game leaves the settings alone (main+0x2809c8c asks per user and
// switches backup off where it is on). Any other service name is refused with a red line, as sm refuses a service that is not registered.
// The SDK's own client functions are taken over where they reach the kernel (their calls inside sdk go through its PLT): the session manager's CreateNewSession runs natively on
// DuplicateSession, and ConvertToDomainImpl on QueryPointerBufferSize and ConvertSessionToDomain. DM_SF_TRACE=1 dumps every request.
// Message layout (Atmosphère / libnx): HIPC header {u16 type, counts of X / A / B / W descriptors; u32 data words, receive list, special header flag}, the special header and descriptors,
// then the data words, whose CMIF part starts 16-byte aligned to the buffer: in a domain session the domain header {u8 type, u8 objects, u16 size, u32 object, u32, u32 token} first,
// then {"SFCI", version, command, token} and the arguments. The response is the same with {u32 objects, 3 × u32} and {"SFCO", version, result, token}
#include "hle.h"
#include <pthread.h>

#define SF_H0 0xd0000u   // session handles: outside the kernel handle table (0x1000 + slot, 4096 slots, hle_thread.c)
#define SF_MAX 64
// A request as a service answers it: the raw input data after the SFCI header, the buffers as the client described them (X: in, pointer; A: in, mapped; B: out, mapped; C: out, the
// receive list), and the raw output data after the SFCO header. An AutoSelect buffer (types 0x15 / 0x16) goes mapped whenever the server's pointer buffer is too small for it, and the host's is
// 0 (QueryPointerBufferSize below), so the SDK sends A / B with an empty X / C beside it: sf_out takes the one with a size
typedef struct { void *addr; uint64_t size; } sf_buf;
#define SF_NBUF 4
typedef struct {
    const char *name;
    const uint8_t *in; size_t nin;
    sf_buf x[SF_NBUF], a[SF_NBUF], b[SF_NBUF], c[SF_NBUF];
    uint8_t out[0x20]; size_t nout;   // zeroed; the service sets nout to the bytes it wrote
} sf_call;
static sf_buf sf_out(const sf_call *c, unsigned i) { return i >= SF_NBUF ? (sf_buf){0} : c->b[i].size ? c->b[i] : c->c[i]; }
// A service's own answer to a command: its result, a kernel handle to copy to the client in *copy (0: none), its output data in call->out and its output buffers.
// NULL: success, zeroed output (olsc:u)
typedef Result (*sf_answer)(unsigned cmd, uint32_t *copy, sf_call *call);
static struct { char name[16]; int used, domain; unsigned seen; sf_answer answer; } g_sf[SF_MAX];
static pthread_mutex_t g_sf_mu = PTHREAD_MUTEX_INITIALIZER;

static int sf_slot(uint32_t h) {
    unsigned i = h - SF_H0;
    return i < SF_MAX && g_sf[i].used ? (int)i : -1;
}
static uint32_t sf_new(const char *name, int domain, sf_answer answer) {
    pthread_mutex_lock(&g_sf_mu);
    unsigned i = 0; while (i < SF_MAX && g_sf[i].used) i++;
    if (i == SF_MAX) die("sf: more than %d host service sessions", SF_MAX);
    strlcpy(g_sf[i].name, name, sizeof g_sf[i].name); g_sf[i].used = 1; g_sf[i].domain = domain; g_sf[i].seen = 0; g_sf[i].answer = answer;
    pthread_mutex_unlock(&g_sf_mu);
    return SF_H0 + i;
}

// The Switch 2's camera (a USB camera, for GameChat's video). nn::camera::sdk::CreateCameraContext asks the graphics system's user session (gsv) for a session to the camera service and
// gives it to the caller as a UniqueNativeHandle (a 32-bit handle; SDK 22.2.0 sdk+0x2c42c0 writes it), with 0x3af3 when the SDK has no gsv session, which the host never opens. The game then
// talks to the camera through that session with the SDK's HIPC client. The host has no camera, so the session answers as a console with none: by Mario Kart World 1.8.0 (pull request #1116,
// not checked here: the build is not on this Mac) the game opens the camera with command 3 and takes 0xdc4 as "no camera" (any other error aborts), then asks for the camera's event with
// command 21 (a copied handle it attaches to a SystemEvent; an error aborts). So 0xdc4 for every command, and an event that never fires for 21
// Star Fox (SDK 21.4.4, its own camera module subsdk3): nn::camera::Initialize sends command 3 with the device name "Default" and takes success or 0xdc4 (any other result aborts, subsdk3+0x1ef4).
// With 0xdc4 it returns that to the game, which reads it as "This platform does not support the camera function." (vwebcam.switch2.cpp:997) and then dereferences a null on a path a console
// never takes (main+0xba54e4c): a Switch 2 supports the camera whether or not one is plugged in. So command 3 succeeds here, and so does 4 from Finalize (the module aborts on any failure there,
// subsdk3+0x200c); QueryDeviceConnection's 29 and any other command answer 0x7c4, which the module and the game read as "No camera device detected.", and the first unknown command logs its number.
// Mario Kart World 1.8.0 is the game that took 0xdc4 for "no camera" above; that build is not on this Mac, so it has not been run with these answers
static Result camera_answer(unsigned cmd, uint32_t *copy, sf_call *call) {
    (void)call;
    if (cmd == 3 || cmd == 4) return RES_OK;
    if (cmd == 21) { *copy = hle_new_event(NULL); return RES_OK; }
    if (cmd != 29) { static int once; if (!once++) LOG("sf: camera command %u answered 0x7c4 (no camera device)", cmd); }
    return 0x7c4u;
}
// The camera module takes the applet's camera resource before it opens a device (nn::applet::BeginUsingCameraResource read a null service object at sdk+0x1dfeb4): the host runs the only
// application, so the resource is always this game's. The context's end: the SDK's DestroyCameraContext asks the gsv session the host never opened and fails, which aborts Finalize
// (subsdk3+0x2010); the module closes the context's handle itself, so there is nothing left to do here
HLE_FORCE("_ZN2nn6camera3sdk20DestroyCameraContextEv", "camera context destroyed: nothing to release on the host", Result, h_cam_ContextEnd, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6applet24BeginUsingCameraResourceEv", "applet camera resource: always available to this game", Result, h_applet_CamBegin, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6applet22EndUsingCameraResourceEv", "applet camera resource released", Result, h_applet_CamEnd, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6camera3sdk19CreateCameraContextEPNS_5gsdrv18UniqueNativeHandleE", "camera context (a camera service session through gsv) → a host session with no camera", Result, h_cam_Context,
          uint32_t *out) {
    *out = sf_new("camera", 0, camera_answer); LOG("sf: camera context → host session %#x (no camera)", *out);
    return RES_OK;
}

// nsd:a / nsd:u, network service discovery (nn::nsd::detail::IManager): the server environment and the names of Nintendo's servers. PATAPON 1+2 REPLAY (call site sdk+0x3aefa8,
// LNPANDY/NX2Apple#426) and Hogwarts Legacy 1.0.5 (sdk+0x3a1748, LNPANDY/NX2Apple#475) connect to nsd:a, then to nsd:u, at boot, and abort with Result 0xe8d (module 141, nsd) when sm refuses
// both. Hogwarts Legacy asks nn::oe::GetServerEnvironmentType, which SDK 17 sends to nsd as GetApplicationServerEnvironmentType (seedds/nxnative#890 traced it). Served here as a retail
// console that is offline, whichever SDK function asks; the commands are switchbrew's (Sockets services, "nsd:u, nsd:a"):
//   11 GetEnvironmentIdentifier: "lp1" in the 8-byte output buffer, as hle_offline.c answers nn::nsd::GetEnvironmentIdentifier
//   12 GetDeviceId: 16 zero bytes (the console's own is a stub that returns 0); 63 IsChangeEnvironmentIdentifierDisabled: false
//   100 GetApplicationServerEnvironmentType: a u8, 1 for lp1 (0 dd1, 1 lp1, 2 sd1, 3 sp1, 4 dp1), or the type 101 SetApplicationServerEnvironmentType set until 102 deletes it
//   20 Resolve, 30 GetNasServiceSetting, 40 GetNasRequestFqdn, 42 GetNasApiFqdn: RES_FAIL, as hle_misc.c answers nn::nsd::Resolve (networking is out of scope). Their Ex forms (21, 31, 41, 43)
//   succeed with RES_FAIL as the u32 InnerResult output, which the SDK's wrapper returns as the lookup's result
// Any other command (the setting name and URL, nsd:a's settings import, the test commands) logs a red line once and fails with sf's ResultUnknownCommandId (0x1ba0a), as a server without it would
enum { NSD_ENV_LP1 = 1 };
static int g_nsd_env = -1;   // 101's type, -1: none (lp1)
static Result nsd_answer(unsigned cmd, uint32_t *copy, sf_call *call) {
    (void)copy;
    switch (cmd) {
    case 11: {
        sf_buf o = sf_out(call, 0);
        if (!o.addr || o.size < 8) { LOG("[RED] sf: %s GetEnvironmentIdentifier without an 8-byte output buffer (%llu bytes)", call->name, (unsigned long long)o.size); return RES_FAIL; }
        memset(o.addr, 0, 8); memcpy(o.addr, "lp1", 3); return RES_OK;
    }
    case 12: call->nout = 16; return RES_OK;
    case 63: call->nout = 1; return RES_OK;
    case 100: { int t = __atomic_load_n(&g_nsd_env, __ATOMIC_RELAXED); call->out[0] = (uint8_t)(t < 0 ? NSD_ENV_LP1 : t); call->nout = 1; return RES_OK; }
    case 101:
        if (call->nin < 1) { LOG("[RED] sf: %s SetApplicationServerEnvironmentType without its argument", call->name); return RES_FAIL; }
        __atomic_store_n(&g_nsd_env, call->in[0], __ATOMIC_RELAXED); LOG("sf: %s: the application server environment type is set to %u", call->name, call->in[0]); return RES_OK;
    case 102: __atomic_store_n(&g_nsd_env, -1, __ATOMIC_RELAXED); return RES_OK;
    case 20: case 30: case 40: case 42: return RES_FAIL;
    case 21: case 31: case 41: case 43: { uint32_t r = RES_FAIL; memcpy(call->out, &r, 4); call->nout = 4; return RES_OK; }
    }
    static uint64_t seen[4]; unsigned k = cmd < 256 ? cmd : 255;
    if (!(__atomic_fetch_or(&seen[k / 64], 1ull << k % 64, __ATOMIC_RELAXED) >> k % 64 & 1))
        LOG("[RED] sf: %s command %u not answered by the host (fails with 0x1ba0a, unknown command)", call->name, cmd);
    return 0x1ba0au;   // sf ResultUnknownCommandId
}

// The services the host serves to a game's own client (ConnectToHipcService): their answer (NULL: every request succeeds with zeroed output) and what the log says of them
static const struct { const char *name; sf_answer answer; const char *what; } sf_served[] = {
    {"olsc:u", NULL, "every request succeeds with zeroed output"},
    {"caps:su", NULL, "every request succeeds with zeroed output"}, {"caps:u", NULL, "every request succeeds with zeroed output"},
    {"caps:a", NULL, "every request succeeds with zeroed output"}, {"caps:c", NULL, "every request succeeds with zeroed output"},
    {"caps:sc", NULL, "every request succeeds with zeroed output"}, {"caps:ss", NULL, "every request succeeds with zeroed output"},
    {"nsd:a", nsd_answer, "a retail console offline: environment lp1, no server names"}, {"nsd:u", nsd_answer, "a retail console offline: environment lp1, no server names"},
};

HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service name resolution (connects to sm); the host resolves the names it serves itself", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service name resolution (sm)", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect to a service through sm → a host session for the services the host serves", Result, h_sf_Connect,
          uint32_t *out, const char *name) {
    for (size_t k = 0; k < sizeof sf_served / sizeof *sf_served; k++)
        if (!strncmp(name, sf_served[k].name, 8)) {
            *out = sf_new(sf_served[k].name, 0, sf_served[k].answer); LOG("sf: %s → host session %#x (%s)", sf_served[k].name, *out, sf_served[k].what); return RES_OK;
        }
    LOG("[RED] sf: the game connects to service %.8s itself, which the host does not serve", name);
    return 0xe15u;   // sm ResultNotRegistered
}
HLE_FORCE("_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE", "close a session (kernel handle) → the host session", void, h_sf_Close, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) { LOG("[RED] sf: CloseClientSessionHandle on %#x, not a host session", h); return; }
    pthread_mutex_lock(&g_sf_mu); g_sf[i].used = 0; pthread_mutex_unlock(&g_sf_mu);
}
// Control requests: the pointer buffer is the server's receive buffer for X descriptors (none: the host reads them in place), the domain conversion makes the session's object 1, a duplicate
// is a new session to the same object
HLE_FORCE("_ZN2nn2sf4hipc6client22QueryPointerBufferSizeEPtNS_3svc6HandleE", "control request: pointer buffer size of a session → 0 for a host session", Result, h_sf_PtrBuf, uint16_t *out, uint32_t h) {
    if (sf_slot(h) < 0) { LOG("[RED] sf: QueryPointerBufferSize on %#x, not a host session", h); return RES_FAIL; }
    *out = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client22ConvertSessionToDomainEPNS0_4cmif18CmifDomainObjectIdENS_3svc6HandleE", "control request: convert a session to a domain → the host session's object 1", Result, h_sf_ToDomain,
          uint32_t *out, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) { LOG("[RED] sf: ConvertSessionToDomain on %#x, not a host session", h); return RES_FAIL; }
    g_sf[i].domain = 1; *out = 1; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client16DuplicateSessionEPNS_3svc6HandleES4_", "control request: clone a session → a new host session to the same service", Result, h_sf_Dup, uint32_t *out, uint32_t h) {
    int i = sf_slot(h);
    if (i < 0) { LOG("[RED] sf: DuplicateSession on %#x, not a host session", h); return RES_FAIL; }
    *out = sf_new(g_sf[i].name, g_sf[i].domain, g_sf[i].answer); return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "control request: clone a session (with a tag) → a new host session to the same service", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }

// A buffer descriptor's address and size (Atmosphère / libnx layouts): X {u32: index:6, address[36:41]:6, address[32:35]:4, size:16; u32 address[0:31]}; A / B / W {u32 size[0:31];
// u32 address[0:31]; u32: mode:2, address[36:57]:22, size[32:35]:4, address[32:35]:4}; a receive list entry {u32 address[0:31]; u32: address[32:47]:16, size:16}
static sf_buf sf_static(const uint32_t *d) {
    return (sf_buf){(void *)(uintptr_t)(d[1] | (uint64_t)(d[0] >> 12 & 15) << 32 | (uint64_t)(d[0] >> 6 & 63) << 36), d[0] >> 16};
}
static sf_buf sf_mapped(const uint32_t *d) {
    return (sf_buf){(void *)(uintptr_t)(d[1] | (uint64_t)(d[2] >> 28) << 32 | (uint64_t)(d[2] >> 2 & 0x3fffff) << 36), d[0] | (uint64_t)(d[2] >> 24 & 15) << 32};
}
static sf_buf sf_recv(const uint32_t *d) { return (sf_buf){(void *)(uintptr_t)(d[0] | (uint64_t)(d[1] & 0xffff) << 32), d[1] >> 16}; }

HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "send a request on a session (svcSendSyncRequest) → answered by the host for its sessions", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    int i = sf_slot(h);
    if (i < 0 || size < 0x40) { LOG("[RED] sf: SendSyncRequest on %#x (%llu bytes), not a host session", h, (unsigned long long)size); return 0xe401u; }   // kernel ResultInvalidHandle
    uint8_t *base = (uint8_t *)m;
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, na = m[0] >> 20 & 15, nb = m[0] >> 24 & 15, nw = m[0] >> 28 & 15, ndw = m[1] & 0x3ff;
    uint32_t *p = m + 2;
    if (m[1] >> 31) { uint32_t sh = *p++; p += (sh & 1 ? 2 : 0) + (sh >> 1 & 15) + (sh >> 5 & 15); }   // special header: process ID, copy and move handles
    const uint32_t *dx = p, *dab = p + nx * 2;
    p += nx * 2 + (na + nb + nw) * 3;
    size_t data = (size_t)((uint8_t *)p - base), cm = (data + 15) & ~(size_t)15;
    int dom = g_sf[i].domain && (type == 4 || type == 6);
    unsigned dtype = dom ? base[cm] : 1, obj = dom ? *(uint32_t *)(base + cm + 4) : 0;
    size_t hin = cm + (dom ? 16 : 0);
    if (hin + 16 > size || data + ndw * 4 > size) { LOG("[RED] sf: %s: request of %u data words does not fit %llu bytes", g_sf[i].name, ndw, (unsigned long long)size); return 0xe401u; }
    uint32_t magic = *(uint32_t *)(base + hin), ver = *(uint32_t *)(base + hin + 4), cmd = *(uint32_t *)(base + hin + 8);
    static int trace = -1; if (trace < 0) trace = getenv("DM_SF_TRACE") && atoi(getenv("DM_SF_TRACE"));
    if (trace) { char hx[3 * 0x80 + 1] = ""; for (unsigned k = 0; k < 0x80 && k < size; k++) snprintf(hx + 3 * k, 4, "%02x ", base[k]); LOG("sf: %s request: %s", g_sf[i].name, hx); }
    if (type == 2) return 0xf601u;   // HIPC Close: the session ends, and the kernel answers the closed-session result (SDK 21's client sends one for a session it is done with: Star Fox's camera session)
    if (type != 4 && type != 6) { LOG("[RED] sf: %s: HIPC message type %u not served", g_sf[i].name, type); return 0xe401u; }
    if (dtype == 2) { memset(base, 0, 16); return RES_OK; }   // a domain object closed: nothing to answer
    if (magic != 0x49434653) { LOG("[RED] sf: %s: request without SFCI (%#x)", g_sf[i].name, magic); return 0xe401u; }
    unsigned key = (obj & 0xff) << 16 | (cmd & 0xffff);   // log each object / command once
    pthread_mutex_lock(&g_sf_mu); int first = g_sf[i].seen != key; g_sf[i].seen = key; pthread_mutex_unlock(&g_sf_mu);
    sf_call call = {.name = g_sf[i].name, .in = base + hin + 16, .nin = data + ndw * 4 > hin + 16 ? data + ndw * 4 - (hin + 16) : 0};
    for (unsigned k = 0; k < nx && k < SF_NBUF; k++) call.x[k] = sf_static(dx + 2 * k);
    for (unsigned k = 0; k < na && k < SF_NBUF; k++) call.a[k] = sf_mapped(dab + 3 * k);
    for (unsigned k = 0; k < nb && k < SF_NBUF; k++) call.b[k] = sf_mapped(dab + 3 * (na + k));
    // The receive list: mode 0 none, 2 one entry, 2 + n n entries; at its offset in words, or right after the data words
    unsigned rmode = m[1] >> 10 & 15, nc = rmode > 2 ? rmode - 2 : rmode == 2, roff = m[1] >> 20 & 0x7ff;
    const uint32_t *rl = roff ? m + roff : (const uint32_t *)(base + data) + ndw;
    for (unsigned k = 0; k < nc && k < SF_NBUF && (const uint8_t *)(rl + 2 * k + 2) <= base + size; k++) call.c[k] = sf_recv(rl + 2 * k);
    uint32_t copy = 0; Result res = g_sf[i].answer ? g_sf[i].answer(cmd & 0xffff, &copy, &call) : RES_OK;
    if (first || trace) LOG("sf: %s object %u command %u (%u data words in): %s%s", g_sf[i].name, obj, cmd, ndw, res ? "fails" : g_sf[i].answer ? "success" : "success, zeroed output", copy ? ", a copied handle" : "");
    // The response: no descriptors. Without a handle there is no special header, so the data words start at +8; a copied handle takes a special header (its copy count) and the handle,
    // so they start at +0x10. The CMIF part starts at +0x10 either way (16-byte aligned); everything after the headers up to the buffer's end reads as zero
    size_t end = size < 0x1000 ? size : 0x1000, dw = copy ? 0x10 : 8;
    memset(base, 0, end);
    m[1] = (uint32_t)((end - dw) / 4 < 0x3ff ? (end - dw) / 4 : 0x3ff);
    if (copy) { m[1] |= 1u << 31; m[2] = 1u << 1; m[3] = copy; }   // special header: no process ID, 1 copy handle, 0 move handles
    size_t out = 0x10 + (dom ? 16 : 0);
    uint32_t *o = (uint32_t *)(base + out); o[0] = 0x4f434653; o[1] = ver; o[2] = res; o[3] = 0;   // "SFCO"; a domain's out-object count before it stays 0
    if (call.nout > sizeof call.out || out + 16 + call.nout > end) LOG("[RED] sf: %s: %zu bytes of output data do not fit the %zu-byte message", g_sf[i].name, call.nout, end);
    else memcpy(base + out + 16, call.out, call.nout);   // the output data after SFCO
    return RES_OK;
}
