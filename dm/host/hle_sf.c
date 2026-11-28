// Services whose client a game links into its own main (nn::sf::hipc), and sessions the SDK gets from another service (the Switch 2's camera, below): Animal Crossing: New Horizons 3.0.3 carries the clients of olsc:u (cloud save data backup settings,
// main+0x404bc6c) and npns (push notifications). The SDK's client code connects through sm (an abort in sm::Initialize under InitializeHipcServiceResolution, from main+0x5b8198) and talks
// HIPC through svcSendSyncRequest; the host has neither. It hands out sessions of its own instead and answers each request in the message buffer, for the services listed in sf_served:
// success, the output data zeroed, no objects or handles. For olsc that reads as "backup off for every user", so the game leaves the settings alone (main+0x2809c8c asks per user and
// switches backup off where it is on). Any other service name is refused with a red line, as sm refuses a service that is not registered.
// The SDK's own client functions are taken over where they reach the kernel (their calls inside sdk go through its PLT): the session manager's CreateNewSession runs natively on
// DuplicateSession, and ConvertToDomainImpl on QueryPointerBufferSize and ConvertSessionToDomain. DM_SF_TRACE=1 dumps every request.
// Message layout (Atmosphère / libnx): HIPC header {u16 type, counts of X / A / B / W descriptors; u32 data words, receive list, special header flag}, the special header and descriptors,
// then the data words, whose CMIF part starts 16-byte aligned to the buffer: in a domain session the domain header {u8 type, u8 objects, u16 size, u32 object, u32, u32 token} first,
// then {"SFCI", version, command, token} and the arguments. The response is the same with {u32 objects, 3 × u32} and {"SFCO", version, result, token}
#include "hle.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <pthread.h>
#include <stdatomic.h>

#define SF_H0 0xd0000u   // session handles: outside the kernel handle table (0x1000 + slot, 4096 slots, hle_thread.c)
#define SF_MAX 64
// Shared owner for a host service session: duplicate handles and active requests share one owner, destroyed outside g_sf_mu on the last release.
typedef struct {
    char name[16];
    sf_answer answer;
    void *ctx;
    void (*close)(void *);
    unsigned seen;
    unsigned refs;
} sf_obj;
// Handle table: each slot keeps handle-specific flags (domain) and a reference to its shared owner.
static struct { int used, domain; sf_obj *obj; } g_sf[SF_MAX];
static pthread_mutex_t g_sf_mu = PTHREAD_MUTEX_INITIALIZER;
// notif:a, the reminder alarms (nn::notification, linked by most SDKs; Fit Boxing 3 lists them when its Settings open, PR 1974): the SDK's client sends only 1000 Initialize (with the process
// ID), 500 Register, 510 Update, 520 List, 530 LoadApplicationParameter and 540 Delete (SDK 21's sdk+0x58ac80..0x58ad10), and aborts on any failure; its notification event comes from am.
// The host schedules no alarms, so zeroed output is its answer: no alarms listed, a registration that never fires (ID 0), a parameter of 0 bytes
static const struct { const char *name; sf_answer answer; } sf_served[] = {
    {"olsc:u", NULL}, {"caps:su", NULL}, {"caps:u", NULL}, {"caps:a", NULL}, {"caps:c", NULL}, {"caps:sc", NULL}, {"caps:ss", NULL}, {"notif:a", NULL},
    // mnpp:app (13.0.0+): Initialize takes a PID and reserved u64; SubmitEvent takes a Uid and input buffer. Neither has output. SNES Classics 6.0.0 aborts when sm refuses it
    // (0xe15, main+0x37080). Offline stand-in: acknowledge and discard events; no queue is retained for Nintendo's later upload (services.md, "Mission events through mnpp:app")
    {"mnpp:app", NULL},
};
// The services other files serve (SF_SERVICE, hle.h): audren:u (hle_audren_sv.c)
static const sf_service_ent *sf_registered(unsigned long *n) {
    unsigned long sz = 0; const sf_service_ent *e = (const sf_service_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swsf", &sz);
    *n = e ? sz / sizeof *e : 0; return e;
}

static int sf_slot_locked(uint32_t h) {
    unsigned i = h - SF_H0;
    return i < SF_MAX && g_sf[i].used ? (int)i : -1;
}
static int sf_slot(uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot_locked(h);
    pthread_mutex_unlock(&g_sf_mu);
    return i;
}
static uint32_t sf_alloc_locked(int domain, sf_obj *o) {
    unsigned i = 0; while (i < SF_MAX && g_sf[i].used) i++;
    if (i == SF_MAX) die("sf: more than %d host service sessions", SF_MAX);
    g_sf[i].used = 1; g_sf[i].domain = domain; g_sf[i].obj = o;
    return SF_H0 + i;
}
static void sf_obj_destroy(sf_obj *o) {
    if (o->close) o->close(o->ctx);   // the session's own state ends with its last handle and active request (an audio renderer stops rendering)
    free(o);
}
static void sf_obj_put(sf_obj *o) {
    pthread_mutex_lock(&g_sf_mu);
    unsigned left = --o->refs;
    pthread_mutex_unlock(&g_sf_mu);
    if (!left) sf_obj_destroy(o);
}
static uint32_t sf_new(const char *name, int domain, sf_answer answer, void *ctx, void (*close)(void *)) {
    sf_obj *o = calloc(1, sizeof *o);
    if (!o) die("sf: out of memory allocating host session");
    strlcpy(o->name, name, sizeof o->name); o->answer = answer; o->ctx = ctx; o->close = close; o->refs = 1;
    pthread_mutex_lock(&g_sf_mu);
    uint32_t h = sf_alloc_locked(domain, o);
    pthread_mutex_unlock(&g_sf_mu);
    return h;
}
uint32_t sf_session(const char *name, sf_answer answer, void *ctx, void (*close)(void *)) { return sf_new(name, 0, answer, ctx, close); }

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
static Result camera_answer(sf_req *r) {
    unsigned cmd = r->cmd;
    if (cmd == 3 || cmd == 4) return RES_OK;
    if (cmd == 21) { r->out_copy = hle_new_event(NULL); return RES_OK; }
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
    *out = sf_new("camera", 0, camera_answer, NULL, NULL); LOG("sf: camera context → host session %#x (no camera)", *out);
    return RES_OK;
}

HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service name resolution (connects to sm); the host resolves the names it serves itself", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service name resolution (sm)", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect to a service through sm → a host session for the services the host serves", Result, h_sf_Connect,
          uint32_t *out, const char *name) {
    for (size_t k = 0; k < sizeof sf_served / sizeof *sf_served; k++)
        if (!strncmp(name, sf_served[k].name, 8)) {
            *out = sf_new(sf_served[k].name, 0, sf_served[k].answer, NULL, NULL); LOG("sf: %s → host session %#x (every request succeeds with zeroed output)", sf_served[k].name, *out);
            return RES_OK;
        }
    unsigned long nreg; const sf_service_ent *reg = sf_registered(&nreg);
    for (unsigned long k = 0; k < nreg; k++)
        if (!strncmp(name, reg[k].name, 8)) { *out = sf_new(reg[k].name, 0, reg[k].answer, NULL, NULL); LOG("sf: %s → host session %#x", reg[k].name, *out); return RES_OK; }
    LOG("[RED] sf: the game connects to service %.8s itself, which the host does not serve", name);
    return 0xe15u;   // sm ResultNotRegistered
}
HLE_FORCE("_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE", "close a session (kernel handle) → the host session", void, h_sf_Close, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot_locked(h);
    if (i < 0) { pthread_mutex_unlock(&g_sf_mu); LOG("[RED] sf: CloseClientSessionHandle on %#x, not a host session", h); return; }
    sf_obj *o = g_sf[i].obj;
    g_sf[i].used = 0; g_sf[i].domain = 0; g_sf[i].obj = NULL;
    unsigned left = --o->refs;
    pthread_mutex_unlock(&g_sf_mu);
    if (!left) sf_obj_destroy(o);
}
// Control requests: the pointer buffer is the server's receive buffer for X descriptors (none: the host reads them in place), the domain conversion makes the session's object 1, a duplicate
// is a new session to the same object
HLE_FORCE("_ZN2nn2sf4hipc6client22QueryPointerBufferSizeEPtNS_3svc6HandleE", "control request: pointer buffer size of a session → 0 for a host session", Result, h_sf_PtrBuf, uint16_t *out, uint32_t h) {
    if (sf_slot(h) < 0) { LOG("[RED] sf: QueryPointerBufferSize on %#x, not a host session", h); return RES_FAIL; }
    *out = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client22ConvertSessionToDomainEPNS0_4cmif18CmifDomainObjectIdENS_3svc6HandleE", "control request: convert a session to a domain → the host session's object 1", Result, h_sf_ToDomain,
          uint32_t *out, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot_locked(h);
    if (i < 0) { pthread_mutex_unlock(&g_sf_mu); LOG("[RED] sf: ConvertSessionToDomain on %#x, not a host session", h); return RES_FAIL; }
    g_sf[i].domain = 1;
    pthread_mutex_unlock(&g_sf_mu);
    *out = 1; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client16DuplicateSessionEPNS_3svc6HandleES4_", "control request: clone a session → a new host session to the same service", Result, h_sf_Dup, uint32_t *out, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot_locked(h);
    if (i < 0) { pthread_mutex_unlock(&g_sf_mu); LOG("[RED] sf: DuplicateSession on %#x, not a host session", h); return RES_FAIL; }
    sf_obj *o = g_sf[i].obj;
    int dom = g_sf[i].domain;
    o->refs++;
    uint32_t dup = sf_alloc_locked(dom, o);
    pthread_mutex_unlock(&g_sf_mu);
    *out = dup; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "control request: clone a session (with a tag) → a new host session to the same service", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }

// A request on a host session. The message (Atmosphère / libnx): HIPC header {u16 type, X / A / B / W descriptor counts; data word count, special header flag}, the special header (process ID,
// copied and moved handles) and the descriptors, then the data words, whose CMIF part starts 16-byte aligned to the buffer. X descriptors are 2 words {index:6, address 36..41:6, address
// 32..35:4, size:16; address low}, A / B / W 3 words {size low; address low; mode:2, address 36..57:22, size 32..35:4, address 32..35:4}
static uint64_t sf_desc_addr(uint32_t lo, uint32_t mid4, uint32_t hi) { return lo | (uint64_t)mid4 << 32 | (uint64_t)hi << 36; }
HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "send a request on a session (svcSendSyncRequest) → answered by the host for its sessions", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot_locked(h);
    if (i < 0 || size < 0x40) {
        pthread_mutex_unlock(&g_sf_mu);
        LOG("[RED] sf: SendSyncRequest on %#x (%llu bytes), not a host session", h, (unsigned long long)size);
        return 0xe401u;   // kernel ResultInvalidHandle
    }
    int slot_dom = g_sf[i].domain;
    sf_obj *o = g_sf[i].obj;
    o->refs++;
    pthread_mutex_unlock(&g_sf_mu);

    Result ret = RES_OK;
    uint8_t *base = (uint8_t *)m;
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, na = m[0] >> 20 & 15, nb = m[0] >> 24 & 15, nw = m[0] >> 28 & 15, ndw = m[1] & 0x3ff;
    sf_req q; memset(&q, 0, sizeof q);
    uint32_t *p = m + 2;
    if (m[1] >> 31) {   // special header: process ID, copied and moved handles
        uint32_t sh = *p++; if (sh & 1) p += 2;
        for (unsigned k = 0; k < (sh >> 1 & 15); k++, p++) if (q.ncopy < 4) q.copy[q.ncopy++] = *p;
        for (unsigned k = 0; k < (sh >> 5 & 15); k++, p++) if (q.nmove < 4) q.move[q.nmove++] = *p;
    }
    for (unsigned k = 0; k < nx; k++, p += 2)
        if (q.nx < 4) { q.x[q.nx].p = (uint8_t *)(uintptr_t)sf_desc_addr(p[1], p[0] >> 12 & 15, p[0] >> 6 & 63); q.x[q.nx++].size = p[0] >> 16; }
    for (unsigned k = 0; k < na + nb + nw; k++, p += 3) {
        uint8_t *a = (uint8_t *)(uintptr_t)sf_desc_addr(p[1], p[2] >> 28, p[2] >> 2 & 0x3fffff); uint64_t n = p[0] | (uint64_t)(p[2] >> 24 & 15) << 32;
        if (k < na) { if (q.na < 4) { q.a[q.na].p = a; q.a[q.na++].size = n; } }
        else if (k < na + nb) { if (q.nb < 4) { q.b[q.nb].p = a; q.b[q.nb++].size = n; } }
    }
    size_t data = (size_t)((uint8_t *)p - base), cm = (data + 15) & ~(size_t)15;
    int dom = slot_dom && (type == 4 || type == 6);
    unsigned dtype = dom ? base[cm] : 1, obj = dom ? *(uint32_t *)(base + cm + 4) : 0;
    size_t hin = cm + (dom ? 16 : 0);
    if (hin + 16 > size || data + ndw * 4 > size) { LOG("[RED] sf: %s: request of %u data words does not fit %llu bytes", o->name, ndw, (unsigned long long)size); ret = 0xe401u; goto out; }
    uint32_t magic = *(uint32_t *)(base + hin), ver = *(uint32_t *)(base + hin + 4), cmd = *(uint32_t *)(base + hin + 8);
    static _Atomic int trace_flag = -1;
    int trace = atomic_load(&trace_flag);
    if (trace < 0) { trace = getenv("DM_SF_TRACE") && atoi(getenv("DM_SF_TRACE")); atomic_store(&trace_flag, trace); }
    if (trace) { char hx[3 * 0x80 + 1] = ""; for (unsigned k = 0; k < 0x80 && k < size; k++) snprintf(hx + 3 * k, 4, "%02x ", base[k]); LOG("sf: %s request: %s", o->name, hx); }
    if (type == 2) { ret = 0xf601u; goto out; }   // HIPC Close: the session ends, and the kernel answers the closed-session result (SDK 21's client sends one for a session it is done with: Star Fox's camera session)
    if (type != 4 && type != 6) { LOG("[RED] sf: %s: HIPC message type %u not served", o->name, type); ret = 0xe401u; goto out; }
    if (dtype == 2) { memset(base, 0, 16); ret = RES_OK; goto out; }   // a domain object closed: nothing to answer
    if (magic != 0x49434653) { LOG("[RED] sf: %s: request without SFCI (%#x)", o->name, magic); ret = 0xe401u; goto out; }
    size_t end_in = data + ndw * 4;
    q.cmd = cmd & 0xffff; q.object = obj; q.in = base + hin + 16; q.in_len = end_in > hin + 16 ? end_in - hin - 16 : 0; q.ctx = o->ctx;
    unsigned key = (obj & 0xff) << 16 | (cmd & 0xffff);   // log each object / command once
    pthread_mutex_lock(&g_sf_mu); int first = o->seen != key; o->seen = key; pthread_mutex_unlock(&g_sf_mu);
    Result res = o->answer ? o->answer(&q) : RES_OK;
    if (q.out_move && dom) { LOG("[RED] sf: %s: command %u returns an object in a domain, which the host does not serve", o->name, q.cmd); q.out_move = 0; res = 0xe401u; }
    if (first || trace) LOG("sf: %s object %u command %u (%u data words in): %s%s%s", o->name, obj, cmd, ndw, res ? "fails" : o->answer ? "success" : "success, zeroed output",
                            q.out_copy ? ", a copied handle" : "", q.out_move ? ", a moved session" : "");
    // The response: no descriptors. The special header (its handle counts) and the handles come right after the HIPC header, when there are any; the CMIF part starts at the next
    // 16-byte boundary (+0x10 with none or one handle), the domain's out header first in a domain. Everything after it up to the buffer's end reads as zero
    unsigned nc = q.out_copy ? 1 : 0, nm = q.out_move ? 1 : 0;
    size_t end = size < 0x1000 ? size : 0x1000, dw = 8 + (nc + nm ? 4 + 4 * (nc + nm) : 0);
    memset(base, 0, end);
    m[1] = (uint32_t)((end - dw) / 4 < 0x3ff ? (end - dw) / 4 : 0x3ff);
    if (nc + nm) { m[1] |= 1u << 31; m[2] = nc << 1 | nm << 5; if (nc) m[3] = q.out_copy; if (nm) m[3 + nc] = q.out_move; }
    size_t out = ((dw + 15) & ~(size_t)15) + (dom ? 16 : 0);
    uint32_t *o_hdr = (uint32_t *)(base + out); o_hdr[0] = 0x4f434653; o_hdr[1] = ver; o_hdr[2] = res; o_hdr[3] = 0;   // "SFCO"; a domain's out-object count before it stays 0
    if (!res && q.out_len) memcpy(base + out + 16, q.out, q.out_len <= sizeof q.out && out + 16 + q.out_len <= end ? q.out_len : 0);
out:
    sf_obj_put(o);
    return ret;
}
