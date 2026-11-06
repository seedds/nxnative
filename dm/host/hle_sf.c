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
#include "sf_service.h"
#include <pthread.h>
extern int svc_thread(int num, uint64_t *regs);

#define SF_H0 0xd0000u   // session handles: outside the kernel handle table (0x1000 + slot, 4096 slots, hle_thread.c)
#define SF_MAX 64
// A service's own answer to a command: its result, and a kernel handle to copy to the client in *copy (0: none). NULL: success, zeroed output (olsc:u)
typedef Result (*sf_answer)(unsigned cmd, uint32_t *copy);
typedef struct { uint32_t id; sf_object object; } sf_entry;
typedef struct { unsigned refs, next_id; int domain; sf_entry entries[SF_MAX]; } sf_objects;
static struct { char name[16]; int used, domain; unsigned seen; sf_answer answer; sf_objects *objects; } g_sf[SF_MAX];
static pthread_mutex_t g_sf_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *const sf_served[] = {"olsc:u", "caps:su", "caps:u", "caps:a", "caps:c", "caps:sc", "caps:ss"};

static int sf_slot(uint32_t h) {
    unsigned i = h - SF_H0;
    return i < SF_MAX && g_sf[i].used ? (int)i : -1;
}
static uint32_t sf_new_locked(const char *name, int domain, sf_answer answer, sf_objects *objects) {
    unsigned i = 0; while (i < SF_MAX && g_sf[i].used) i++;
    if (i == SF_MAX) return 0;
    strlcpy(g_sf[i].name, name, sizeof g_sf[i].name); g_sf[i].used = 1; g_sf[i].domain = domain; g_sf[i].seen = 0; g_sf[i].answer = answer;
    g_sf[i].objects = objects;
    return SF_H0 + i;
}
static uint32_t sf_new(const char *name, int domain, sf_answer answer) {
    pthread_mutex_lock(&g_sf_mu);
    uint32_t h = sf_new_locked(name, domain, answer, NULL);
    pthread_mutex_unlock(&g_sf_mu);
    if (!h) die("sf: more than %d host service sessions", SF_MAX);
    return h;
}
static void sf_dispose(sf_object *o) {
    if (o->close) o->close(o->state);
    *o = (sf_object){0};
}
static void sf_release_locked(sf_objects *objects) {
    if (!objects || --objects->refs) return;
    for (unsigned k = 0; k < SF_MAX; k++) sf_dispose(&objects->entries[k].object);
    free(objects);
}
// Takes ownership only on success. Duplicated handles share the domain and its objects, as HIPC sessions do.
static uint32_t sf_object_new_locked(const char *name, sf_object object) {
    sf_objects *objects = calloc(1, sizeof *objects);
    if (!objects) return 0;
    objects->refs = 1; objects->next_id = 2;
    objects->entries[0] = (sf_entry){1, object};
    uint32_t h = sf_new_locked(name, 0, NULL, objects);
    if (!h) free(objects);
    return h;
}
static uint32_t sf_object_new(const char *name, sf_object object) {
    pthread_mutex_lock(&g_sf_mu);
    uint32_t h = sf_object_new_locked(name, object);
    pthread_mutex_unlock(&g_sf_mu);
    return h;
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
static Result camera_answer(unsigned cmd, uint32_t *copy) {
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

HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service name resolution (connects to sm); the host resolves the names it serves itself", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service name resolution (sm)", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect to a service through sm → a host session for the services the host serves", Result, h_sf_Connect,
          uint32_t *out, const char *name) {
    if (!strncmp(name, "auddmg:u", 8)) {
        uint32_t h = sf_object_new("auddmg:u", sf_auddmg_root());
        if (!h) return 0xa99u;
        *out = h; LOG("sf: auddmg:u → host device-manager session %#x", h); return RES_OK;
    }
    if (!strncmp(name, "audsmx:u", 8)) {
        uint32_t h = sf_object_new("audsmx:u", sf_audsmx_root());
        if (!h) return RES_FAIL;
        *out = h; LOG("sf: audsmx:u → host system-mixer session %#x", h); return RES_OK;
    }
    for (size_t k = 0; k < sizeof sf_served / sizeof *sf_served; k++)
        if (!strncmp(name, sf_served[k], 8)) { *out = sf_new(sf_served[k], 0, NULL); LOG("sf: %s → host session %#x (every request succeeds with zeroed output)", sf_served[k], *out); return RES_OK; }
    LOG("[RED] sf: the game connects to service %.8s itself, which the host does not serve", name);
    return 0xe15u;   // sm ResultNotRegistered
}
HLE_FORCE("_ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE", "close a session (kernel handle) → the host session", void, h_sf_Close, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot(h);
    if (i >= 0) { g_sf[i].used = 0; sf_release_locked(g_sf[i].objects); g_sf[i].objects = NULL; }
    pthread_mutex_unlock(&g_sf_mu);
    if (i < 0) LOG("[RED] sf: CloseClientSessionHandle on %#x, not a host session", h);
}
// Control requests: the pointer buffer is the server's receive buffer for X descriptors (none: the host reads them in place), the domain conversion makes the session's object 1, a duplicate
// is a new session to the same object
HLE_FORCE("_ZN2nn2sf4hipc6client22QueryPointerBufferSizeEPtNS_3svc6HandleE", "control request: pointer buffer size of a session → 0 for a host session", Result, h_sf_PtrBuf, uint16_t *out, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu); int valid = sf_slot(h) >= 0; pthread_mutex_unlock(&g_sf_mu);
    if (!valid) { LOG("[RED] sf: QueryPointerBufferSize on %#x, not a host session", h); return RES_FAIL; }
    *out = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client22ConvertSessionToDomainEPNS0_4cmif18CmifDomainObjectIdENS_3svc6HandleE", "control request: convert a session to a domain → the host session's object 1", Result, h_sf_ToDomain,
          uint32_t *out, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot(h);
    if (i >= 0) { g_sf[i].domain = 1; if (g_sf[i].objects) g_sf[i].objects->domain = 1; *out = 1; }
    pthread_mutex_unlock(&g_sf_mu);
    if (i < 0) { LOG("[RED] sf: ConvertSessionToDomain on %#x, not a host session", h); return RES_FAIL; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client16DuplicateSessionEPNS_3svc6HandleES4_", "control request: clone a session → a new host session to the same service", Result, h_sf_Dup, uint32_t *out, uint32_t h) {
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot(h);
    uint32_t copy = i < 0 ? 0 : sf_new_locked(g_sf[i].name, g_sf[i].domain, g_sf[i].answer, g_sf[i].objects);
    if (copy && g_sf[i].objects) g_sf[i].objects->refs++;
    pthread_mutex_unlock(&g_sf_mu);
    if (!copy) { LOG("[RED] sf: cannot duplicate session %#x", h); return RES_FAIL; }
    *out = copy; return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "control request: clone a session (with a tag) → a new host session to the same service", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }

static uint32_t sf_u32(const void *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static void sf_put32(void *p, uint32_t v) { memcpy(p, &v, sizeof v); }
void sf_close_handle(uint32_t handle) {
    if (handle) { uint64_t regs[8] = {handle}; svc_thread(0x16, regs); }
}
static sf_buffer sf_map_buffer(const uint8_t *p) {
    uint32_t bits = sf_u32(p + 8);
    uintptr_t addr = sf_u32(p + 4) | (uint64_t)(bits >> 28) << 32 | (uint64_t)(bits >> 2 & 0x3fffff) << 36;
    size_t size = sf_u32(p) | (uint64_t)(bits >> 24 & 15) << 32;
    return (sf_buffer){(void *)addr, size, bits & 3};
}
// Typed CMIF services need actual payloads, MapAlias buffers and owned child objects. Legacy services below retain their zero-output replies.
// Keep HIPC descriptor bounds separate from the declared CMIF data-word bounds.
static Result sf_object_send_locked(uint32_t h, uint8_t *base, size_t size) {
    Result transport = 0xe401u;
    int i = sf_slot(h);
    if (i < 0 || !g_sf[i].objects || size < 0x40) goto done;
    sf_objects *objects = g_sf[i].objects;
    uint32_t h0 = sf_u32(base), h1 = sf_u32(base + 4), type = h0 & 0xffff;
    if (type != 4 && type != 6) goto done;
    sf_request req = {.nx = h0 >> 16 & 15, .na = h0 >> 20 & 15, .nb = h0 >> 24 & 15, .nw = h0 >> 28 & 15,
                      .nc = h1 >> 10 & 15};
    size_t at = 8;
    if (h1 >> 31) {
        uint32_t sh = sf_u32(base + at); at += 4;
        req.send_pid = sh & 1; req.ncopy = sh >> 1 & 15; req.nmove = sh >> 5 & 15;
        if ((req.send_pid ? 8u : 0u) + 4 * (req.ncopy + req.nmove) > size - at) goto done;
        at += req.send_pid ? 8 : 0;
        for (unsigned k = 0; k < req.ncopy; k++, at += 4) req.copies[k] = sf_u32(base + at);
        for (unsigned k = 0; k < req.nmove; k++, at += 4) req.moves[k] = sf_u32(base + at);
        // One guest process: this is the identity h_applet_Aruid exposes. The HIPC PID slot is kernel-written, not trusted client data.
        if (req.send_pid) req.pid = DM_GUEST_PROCESS_ID;
    }
    size_t descriptors = 8 * req.nx + 12 * (req.na + req.nb + req.nw);
    if (descriptors > size - at) goto done;
    at += 8 * req.nx;
    for (unsigned k = 0; k < req.na; k++, at += 12) req.a[k] = sf_map_buffer(base + at);
    for (unsigned k = 0; k < req.nb; k++, at += 12) req.b[k] = sf_map_buffer(base + at);
    at += 12 * req.nw;
    size_t data_end = at + 4 * (h1 & 0x3ff), cm = (at + 15) & ~(size_t)15;
    if (data_end > size || cm > data_end) goto done;
    uint32_t object_id = 1, token = 0;
    unsigned dtype = 1;
    size_t payload_end = data_end;
    if (objects->domain) {
        if (data_end - cm < 16) goto done;
        dtype = base[cm]; req.nobjects = base[cm + 1];
        unsigned bytes = base[cm + 2] | (unsigned)base[cm + 3] << 8;
        object_id = sf_u32(base + cm + 4); token = sf_u32(base + cm + 12); cm += 16;
        if (dtype != 1 && dtype != 2) goto done;
        if (dtype == 1) {
            if (bytes < 16 || bytes + 4 * req.nobjects > data_end - cm) goto done;
            payload_end = cm + bytes;
        }
    }
    sf_entry *entry = NULL;
    for (unsigned k = 0; k < SF_MAX; k++) if (objects->entries[k].id == object_id && objects->entries[k].object.request) { entry = &objects->entries[k]; break; }
    if (!entry) goto done;
    if (dtype == 2) {
        sf_dispose(&entry->object); entry->id = 0;
        memset(base, 0, 16); transport = RES_OK; goto done;
    }
    if (payload_end - cm < 16 || sf_u32(base + cm) != 0x49434653) goto done;
    uint32_t version = sf_u32(base + cm + 4);
    req.command = sf_u32(base + cm + 8);
    if (!objects->domain) token = sf_u32(base + cm + 12);
    req.data = base + cm + 16; req.size = payload_end - cm - 16;
    sf_reply reply = {0};
    Result res = entry->object.request(entry->object.state, &req, &reply);
    if (reply.size > sizeof reply.data) res = RES_FAIL;
    if (res) { sf_dispose(&reply.child); sf_close_handle(reply.copy); reply.copy = 0; reply.size = 0; }
    unsigned has_child = reply.child.request != NULL;
    size_t dw = 8 + ((reply.copy || (has_child && !objects->domain)) ? 4 + 4 * ((reply.copy != 0) + (has_child && !objects->domain)) : 0);
    size_t out = ((dw + 15) & ~(size_t)15) + (objects->domain ? 16 : 0);
    size_t end = out + 16 + reply.size + (has_child && objects->domain ? 4 : 0);
    end = (end + 3) & ~(size_t)3;
    if (end > size) { sf_dispose(&reply.child); sf_close_handle(reply.copy); goto done; }
    uint32_t child = 0;
    if (has_child) {
        if (objects->domain) {
            for (unsigned k = 0; k < SF_MAX; k++) if (!objects->entries[k].object.request) {
                if (objects->next_id) { child = objects->next_id++; objects->entries[k] = (sf_entry){child, reply.child}; }
                break;
            }
        } else child = sf_object_new_locked(g_sf[i].name, reply.child);
        if (!child) { sf_dispose(&reply.child); sf_close_handle(reply.copy); goto done; }
    }
    static int trace = -1; if (trace < 0) trace = getenv("DM_SF_TRACE") && atoi(getenv("DM_SF_TRACE"));
    unsigned key = object_id * 65537u + req.command;
    if (g_sf[i].seen != key || trace) LOG("sf: %s object %u command %u: result %#x, %zu output bytes%s%s", g_sf[i].name, object_id, req.command, res, reply.size,
                                        has_child ? ", child object" : "", reply.copy ? ", copied handle" : "");
    g_sf[i].seen = key;
    memset(base, 0, end);
    sf_put32(base + 4, (uint32_t)((end - dw) / 4));
    if (dw != 8) {
        sf_put32(base + 4, sf_u32(base + 4) | 0x80000000u);
        sf_put32(base + 8, (reply.copy ? 2u : 0) | (has_child && !objects->domain ? 32u : 0));
        size_t hp = 12;
        if (reply.copy) { sf_put32(base + hp, reply.copy); hp += 4; }
        if (has_child && !objects->domain) sf_put32(base + hp, child);
    }
    if (objects->domain) sf_put32(base + out - 16, has_child);
    sf_put32(base + out, 0x4f434653); sf_put32(base + out + 4, version);
    sf_put32(base + out + 8, res); sf_put32(base + out + 12, token);
    memcpy(base + out + 16, reply.data, reply.size);
    if (has_child && objects->domain) sf_put32(base + out + 16 + reply.size, child);
    transport = RES_OK;
done:
    return transport;
}

static Result sf_legacy_send_locked(int i, uint32_t *m, uint64_t size) {
    uint8_t *base = (uint8_t *)m;
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, nabw = (m[0] >> 20 & 15) + (m[0] >> 24 & 15) + (m[0] >> 28 & 15), ndw = m[1] & 0x3ff;
    uint32_t *p = m + 2;
    if (m[1] >> 31) { uint32_t sh = *p++; p += (sh & 1 ? 2 : 0) + (sh >> 1 & 15) + (sh >> 5 & 15); }   // special header: process ID, copy and move handles
    p += nx * 2 + nabw * 3;
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
    int first = g_sf[i].seen != key; g_sf[i].seen = key;
    uint32_t copy = 0; Result res = g_sf[i].answer ? g_sf[i].answer(cmd & 0xffff, &copy) : RES_OK;
    if (first || trace) LOG("sf: %s object %u command %u (%u data words in): %s%s", g_sf[i].name, obj, cmd, ndw, res ? "fails" : "success, zeroed output", copy ? ", a copied handle" : "");
    // The response: no descriptors. Without a handle there is no special header, so the data words start at +8; a copied handle takes a special header (its copy count) and the handle,
    // so they start at +0x10. The CMIF part starts at +0x10 either way (16-byte aligned); everything after the headers up to the buffer's end reads as zero
    size_t end = size < 0x1000 ? size : 0x1000, dw = copy ? 0x10 : 8;
    memset(base, 0, end);
    m[1] = (uint32_t)((end - dw) / 4 < 0x3ff ? (end - dw) / 4 : 0x3ff);
    if (copy) { m[1] |= 1u << 31; m[2] = 1u << 1; m[3] = copy; }   // special header: no process ID, 1 copy handle, 0 move handles
    size_t out = 0x10 + (dom ? 16 : 0);
    uint32_t *o = (uint32_t *)(base + out); o[0] = 0x4f434653; o[1] = ver; o[2] = res; o[3] = 0;   // "SFCO"; a domain's out-object count before it stays 0
    return RES_OK;
}

HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "send a request on a session (svcSendSyncRequest) → answered by the host for its sessions", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    // Service handlers are bounded host operations. Serialize dispatch with duplication/close so a final close cannot free state during a request.
    pthread_mutex_lock(&g_sf_mu);
    int i = sf_slot(h);
    Result res = 0xe401u;
    if (i >= 0 && size >= 0x40) res = g_sf[i].objects ? sf_object_send_locked(h, (uint8_t *)m, size) : sf_legacy_send_locked(i, m, size);
    else LOG("[RED] sf: SendSyncRequest on %#x (%llu bytes), not a host session", h, (unsigned long long)size);
    pthread_mutex_unlock(&g_sf_mu);
    return res;
}
