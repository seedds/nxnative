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
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define SF_H0 0xd0000u   // session handles: outside the kernel handle table (0x1000 + slot, 4096 slots, hle_thread.c)
#define SF_MAX 64
// A service's own answer to a command: its result, and a kernel handle to copy to the client in *copy (0: none). NULL: success, zeroed output (olsc:u)
typedef Result (*sf_answer)(unsigned cmd, uint32_t *copy);
// A service that answers with data: it reads the request's payload and buffers and writes its own output data, buffers, one copied handle or one moved handle (a new session, how a command returns
// an object outside a domain). out is a scratch area the host copies behind the response's SFCO header, out_len the bytes the service used of it
typedef struct { unsigned cmd; intptr_t user; const uint8_t *in; size_t in_len; struct { uint8_t *p; uint64_t n; } a[4], b[4]; unsigned na, nb; uint8_t out[0x400]; size_t out_len; uint32_t copy, move; } sf_req;
typedef Result (*sf_answer2)(sf_req *r);
static struct { char name[16]; int used, domain; unsigned seen; sf_answer answer; sf_answer2 answer2; intptr_t user; } g_sf[SF_MAX];
static pthread_mutex_t g_sf_mu = PTHREAD_MUTEX_INITIALIZER;
static const char *const sf_served[] = {"olsc:u", "caps:su", "caps:u", "caps:a", "caps:c", "caps:sc", "caps:ss"};

static int sf_slot(uint32_t h) {
    unsigned i = h - SF_H0;
    return i < SF_MAX && g_sf[i].used ? (int)i : -1;
}
static uint32_t sf_new(const char *name, int domain, sf_answer answer) {
    pthread_mutex_lock(&g_sf_mu);
    unsigned i = 0; while (i < SF_MAX && g_sf[i].used) i++;
    if (i == SF_MAX) die("sf: more than %d host service sessions", SF_MAX);
    strlcpy(g_sf[i].name, name, sizeof g_sf[i].name); g_sf[i].used = 1; g_sf[i].domain = domain; g_sf[i].seen = 0; g_sf[i].answer = answer; g_sf[i].answer2 = NULL;
    pthread_mutex_unlock(&g_sf_mu);
    return SF_H0 + i;
}
static uint32_t sf_new2(const char *name, sf_answer2 answer2, intptr_t user) {
    uint32_t h = sf_new(name, 0, NULL);
    pthread_mutex_lock(&g_sf_mu); g_sf[h - SF_H0].answer2 = answer2; g_sf[h - SF_H0].user = user; pthread_mutex_unlock(&g_sf_mu);
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

// The Switch 2's audio mixer (audsmx:u, SDK 22): the SDK's output streams. The root's command 0 (16 bytes, the process ID, a copied handle) returns a user session, whose command 1 (two u32 including
// the creation flags, a u64 process ID, the ring buffer's address and size as u64, a name in a send buffer) makes a stream: its ID in the output data and the stream as a session; commands 2 and 3 (a
// send buffer of u32 stream IDs) engage and disengage streams. A stream's commands: 0 open (sample type, channels, rate, frames, prefill and flags, all u32), 1 close, 2 the state (u32: 0 created,
// 1 open, 2 engaged), 3 the gain (f32), 4 set it (f32 and a ramp time), 5 and 7 (a mode, a threshold) the state-change and data events as copied handles, 6 and 8 release them, 9 clock, 10 and 11
// (s32 frames) prime and purge, answered with that number of frames, 12 rename. The host plays no stream yet: it keeps each stream's state, signals the state event when the state changes, and
// drains an engaged stream's ring at its sample rate (below), so the game's audio clock, which its movie player paces the video by, runs; the samples are dropped
#define AUD_MAXSTREAM 32
static struct { int used, open, engaged, mode; int32_t thr; uint32_t id, srh, swh, drh, dwh, rate, frames; float gain; double last, owed; uint8_t *ring; uint64_t ring_size; } g_as[AUD_MAXSTREAM];
static pthread_mutex_t g_as_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_as_next_id = 1;
static double aud_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
// Every 2 ms: an engaged stream's read index moves on by the frames its sample rate owes since the last pass (up to what the ring holds; an empty ring plays silence), and a stream with at least
// its threshold of free frames gets its data event. The ring is the game's own memory: its read index at +0x44 and its write index at +0x1004 (u32 each), frames + 1 entries
#define AUD_RING_READ 0x44
#define AUD_RING_WRITE 0x1004
static void *aud_timer(void *arg) {
    (void)arg;
    for (;;) {
        usleep(2000); double now = aud_now();
        pthread_mutex_lock(&g_as_mu);
        for (int k = 0; k < AUD_MAXSTREAM; k++) {
            if (!g_as[k].used || !g_as[k].open || !g_as[k].frames || !g_as[k].ring || g_as[k].ring_size < AUD_RING_WRITE + 4) continue;
            uint32_t *rd = (uint32_t *)(g_as[k].ring + AUD_RING_READ), *wr = (uint32_t *)(g_as[k].ring + AUD_RING_WRITE);
            uint32_t cap = g_as[k].frames + 1, w = __atomic_load_n(wr, __ATOMIC_ACQUIRE), r = __atomic_load_n(rd, __ATOMIC_ACQUIRE);
            double dt = now - g_as[k].last; g_as[k].last = now;
            if (w >= cap || r >= cap) continue;
            uint32_t filled = (w + cap - r) % cap;
            if (g_as[k].engaged) {
                g_as[k].owed += g_as[k].rate * dt; uint32_t owed = (uint32_t)g_as[k].owed; g_as[k].owed -= owed;
                uint32_t taken = filled < owed ? filled : owed;
                if (taken) { __atomic_store_n(rd, (r + taken) % cap, __ATOMIC_RELEASE); filled -= taken; }
            }
            if (g_as[k].mode && (int32_t)(cap - 1 - filled) >= g_as[k].thr) hle_event_signal(g_as[k].dwh);
        }
        pthread_mutex_unlock(&g_as_mu);
    }
    return NULL;
}
static int aud_stream_new(uint8_t *ring, uint64_t ring_size) {
    static int timer_started;
    pthread_mutex_lock(&g_as_mu);
    int k = 0; while (k < AUD_MAXSTREAM && g_as[k].used) k++;
    if (k == AUD_MAXSTREAM) { pthread_mutex_unlock(&g_as_mu); LOG("[RED] sf: audsmx:u: more than %d streams", AUD_MAXSTREAM); return -1; }
    memset(&g_as[k], 0, sizeof g_as[k]); g_as[k].used = 1; g_as[k].id = g_as_next_id++; g_as[k].gain = 1.0f; g_as[k].ring = ring; g_as[k].ring_size = ring_size; g_as[k].srh = hle_new_event(&g_as[k].swh); g_as[k].drh = hle_new_event(&g_as[k].dwh);
    pthread_mutex_unlock(&g_as_mu);
    if (!timer_started) { timer_started = 1; pthread_t t; pthread_create(&t, NULL, aud_timer, NULL); pthread_detach(t); }
    return k;
}
static int aud_find(uint32_t id) { for (int k = 0; k < AUD_MAXSTREAM; k++) if (g_as[k].used && g_as[k].id == id) return k; return -1; }
static void aud_engage(uint32_t id, int on) {
    pthread_mutex_lock(&g_as_mu);
    int k = aud_find(id);
    if (k >= 0) { g_as[k].engaged = on; g_as[k].owed = 0; g_as[k].last = aud_now(); hle_event_signal(g_as[k].swh); }
    pthread_mutex_unlock(&g_as_mu);
}
static Result aud_stream_answer(sf_req *r) {
    int k = (int)r->user; Result res = RES_OK;
    pthread_mutex_lock(&g_as_mu);
    if (k < 0 || k >= AUD_MAXSTREAM || !g_as[k].used) { pthread_mutex_unlock(&g_as_mu); return 0x1u; }
    uint32_t u[6] = {0}; memcpy(u, r->in, r->in_len < sizeof u ? r->in_len : sizeof u);
    switch (r->cmd) {
    case 0: g_as[k].rate = u[2]; g_as[k].frames = u[3]; g_as[k].open = 1; g_as[k].last = aud_now(); g_as[k].owed = 0; hle_event_signal(g_as[k].swh); LOG("sf: audsmx:u stream %u opened: sample type %u, %u channels, %u Hz, %u frames", g_as[k].id, u[0], u[1], u[2], u[3]); break;
    case 1: g_as[k].open = 0; hle_event_signal(g_as[k].swh); break;
    case 2: { uint32_t st = g_as[k].open ? (g_as[k].engaged ? 2 : 1) : 0; memcpy(r->out, &st, 4); r->out_len = 4; break; }
    case 3: memcpy(r->out, &g_as[k].gain, 4); r->out_len = 4; break;
    case 4: memcpy(&g_as[k].gain, u, 4); break;
    case 5: r->copy = hle_dup_readable_handle(g_as[k].srh); break;
    case 7: g_as[k].mode = (int)u[0]; g_as[k].thr = (int32_t)u[1]; r->copy = hle_dup_readable_handle(g_as[k].drh); break;
    case 10: case 11: { int32_t f = (int32_t)u[0]; uint32_t n = f > 0 ? (uint32_t)f : 0; memcpy(r->out, &n, 4); r->out_len = 4; break; }
    default: break;
    }
    pthread_mutex_unlock(&g_as_mu);
    return res;
}
static Result aud_user_answer(sf_req *r) {
    if (r->cmd == 1) {
        uint64_t ring = 0, ring_size = 0; if (r->in_len >= 32) { memcpy(&ring, r->in + 16, 8); memcpy(&ring_size, r->in + 24, 8); }
        int k = aud_stream_new((uint8_t *)(uintptr_t)ring, ring_size); if (k < 0) return 0x1u;
        uint32_t id = g_as[k].id; memcpy(r->out, &id, 4); r->out_len = 4;
        r->move = sf_new2("audsmx:u stream", aud_stream_answer, k);
    } else if ((r->cmd == 2 || r->cmd == 3) && r->na && r->a[0].p) {
        for (uint64_t j = 0; j + 4 <= r->a[0].n; j += 4) aud_engage(*(uint32_t *)(r->a[0].p + j), r->cmd == 2);
    }
    return RES_OK;
}
static Result aud_mixer_answer(sf_req *r) {
    if (r->cmd == 0) r->move = sf_new2("audsmx:u session", aud_user_answer, 0);
    return RES_OK;
}

// The Switch 2's audio device manager (auddmg:u, SDK 22): nn::audio::ListVirtualOutputDevices and the device property queries the SDK makes while it sets up audio. Command 0 (16 bytes and the
// process ID in) returns the manager as a new session; the manager's commands, by what a game reads of them: 0 (u32 capacity) the number of devices in its output data and their names in a receive
// buffer, 0x100 bytes each; 100 the default device's name in a receive buffer; 300 (a stream format, a name) whether the device takes it, as the result; 401 (a name in a send buffer) the device's
// 0x228-byte property block in a receive buffer; 500 an event for the device's state changes (a copied handle); 700 and 800 (a stream's ID, a name) attach a stream to the device and detach it
// again (it engages the stream the mixer made). The host has one virtual device, stereo at 48 kHz, in front of the output it plays everything through. The property block: the name at +0, a type (3) at +0x100, a flag word (2) at
// +0x104, the channel count at +0x108, the sample rate at +0x10c, the physical output's name at +0x110, its ID at +0x210, the latency in nanoseconds (a u64) at +0x218 and a device ID (not 0: the
// SDK hands it to the mixer when it creates a stream) at +0x220. The streams themselves are created through the mixer service (audsmx:u, below)
#define AUD_DEV_NAME "AudioVirtualOutput0"
#define AUD_PHYS_NAME "AudioTvOutput"
static void aud_put_name(uint8_t *dst, const char *name) { memset(dst, 0, 0x100); strlcpy((char *)dst, name, 0x100); }
static Result aud_mgr_answer(sf_req *r) {
    switch (r->cmd) {
    case 0: {
        uint32_t n = r->nb && r->b[0].n >= 0x100 ? 1 : 0;
        if (n) aud_put_name(r->b[0].p, AUD_DEV_NAME);
        memcpy(r->out, &n, 4); r->out_len = 4; return RES_OK; }
    case 100:
        if (r->nb && r->b[0].n >= 0x100) aud_put_name(r->b[0].p, AUD_DEV_NAME);
        return RES_OK;
    case 401: {
        if (!r->nb || r->b[0].n < 0x228) return 0x1u;
        uint8_t *b = r->b[0].p; memset(b, 0, 0x228);
        if (r->na && r->a[0].n) memcpy(b, r->a[0].p, r->a[0].n < 0xff ? r->a[0].n : 0xff); else strlcpy((char *)b, AUD_DEV_NAME, 0x100);
        uint32_t t = 3, f = 2, ch = 2, hz = 48000, id = 1; uint64_t lat = 10000000;
        memcpy(b + 0x100, &t, 4); memcpy(b + 0x104, &f, 4); memcpy(b + 0x108, &ch, 4); memcpy(b + 0x10c, &hz, 4);
        strlcpy((char *)b + 0x110, AUD_PHYS_NAME, 0x100); memcpy(b + 0x218, &lat, 8); memcpy(b + 0x220, &id, 4);
        return RES_OK; }
    case 500: r->copy = hle_new_event(NULL); return RES_OK;
    case 700: case 800: { uint32_t id = 0; if (r->in_len >= 4) memcpy(&id, r->in, 4); aud_engage(id, r->cmd == 700); return RES_OK; }
    case 300: return RES_OK;
    default: return RES_OK;
    }
}
static Result aud_root_answer(sf_req *r) {
    if (r->cmd == 0) { r->move = sf_new2("auddmg:u mgr", aud_mgr_answer, 0); return RES_OK; }
    return RES_OK;
}

HLE_FORCE("_ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv", "service name resolution (connects to sm); the host resolves the names it serves itself", Result, h_sf_InitRes, void) { return RES_OK; }
HLE_FORCE("_ZN2nn2sf4hipc29FinalizeHipcServiceResolutionEv", "service name resolution (sm)", void, h_sf_FinRes, void) {}
HLE_FORCE("_ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc", "connect to a service through sm → a host session for the services the host serves", Result, h_sf_Connect,
          uint32_t *out, const char *name) {
    if (!strncmp(name, "audsmx:u", 8)) { *out = sf_new2("audsmx:u", aud_mixer_answer, 0); LOG("sf: audsmx:u → host session %#x (the audio mixer; streams keep their state and fire their events, nothing plays)", *out); return RES_OK; }
    if (!strncmp(name, "auddmg:u", 8)) { *out = sf_new2("auddmg:u", aud_root_answer, 0); LOG("sf: auddmg:u → host session %#x (the audio device manager)", *out); return RES_OK; }
    for (size_t k = 0; k < sizeof sf_served / sizeof *sf_served; k++)
        if (!strncmp(name, sf_served[k], 8)) { *out = sf_new(sf_served[k], 0, NULL); LOG("sf: %s → host session %#x (every request succeeds with zeroed output)", sf_served[k], *out); return RES_OK; }
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
    *out = sf_new(g_sf[i].name, g_sf[i].domain, g_sf[i].answer);
    pthread_mutex_lock(&g_sf_mu); g_sf[*out - SF_H0].answer2 = g_sf[i].answer2; g_sf[*out - SF_H0].user = g_sf[i].user; pthread_mutex_unlock(&g_sf_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2sf4hipc6client18DuplicateSessionExEPNS_3svc6HandleES4_j", "control request: clone a session (with a tag) → a new host session to the same service", Result, h_sf_DupEx,
          uint32_t *out, uint32_t h, unsigned tag) { (void)tag; return h_sf_Dup(out, h); }

HLE_FORCE("_ZN2nn2sf4hipc15SendSyncRequestENS_3svc6HandleEPvm", "send a request on a session (svcSendSyncRequest) → answered by the host for its sessions", Result, h_sf_Send,
          uint32_t h, uint32_t *m, uint64_t size) {
    int i = sf_slot(h);
    if (i < 0 || size < 0x40) { LOG("[RED] sf: SendSyncRequest on %#x (%llu bytes), not a host session", h, (unsigned long long)size); return 0xe401u; }   // kernel ResultInvalidHandle
    uint8_t *base = (uint8_t *)m;
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, nabw = (m[0] >> 20 & 15) + (m[0] >> 24 & 15) + (m[0] >> 28 & 15), ndw = m[1] & 0x3ff;
    uint32_t *p = m + 2;
    if (m[1] >> 31) { uint32_t sh = *p++; p += (sh & 1 ? 2 : 0) + (sh >> 1 & 15) + (sh >> 5 & 15); }   // special header: process ID, copy and move handles
    unsigned na = m[0] >> 20 & 15, nbn = m[0] >> 24 & 15; const uint32_t *dsc = p + nx * 2;   // the A (send) descriptors, then the B (receive) ones
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
    pthread_mutex_lock(&g_sf_mu); int first = g_sf[i].seen != key; g_sf[i].seen = key; pthread_mutex_unlock(&g_sf_mu);
    uint32_t copy = 0, move = 0; Result res; static sf_req rq; static pthread_mutex_t rqmu = PTHREAD_MUTEX_INITIALIZER; uint8_t out2[0x400]; size_t out2n = 0;
    if (g_sf[i].answer2) {
        pthread_mutex_lock(&rqmu); memset(&rq, 0, sizeof rq); rq.cmd = cmd & 0xffff; rq.user = g_sf[i].user;
        rq.in = base + hin + 16; rq.in_len = data + ndw * 4 > hin + 16 ? data + ndw * 4 - (hin + 16) : 0;
        for (unsigned k = 0; k < na && k < 4; k++) { const uint32_t *d = dsc + k * 3; rq.a[rq.na].p = (uint8_t *)(uintptr_t)(d[1] | (uint64_t)(d[2] >> 28 & 15) << 32 | (uint64_t)(d[2] >> 2 & 7) << 36); rq.a[rq.na++].n = d[0] | (uint64_t)(d[2] >> 24 & 15) << 32; }
        for (unsigned k = 0; k < nbn && k < 4; k++) { const uint32_t *d = dsc + (na + k) * 3; rq.b[rq.nb].p = (uint8_t *)(uintptr_t)(d[1] | (uint64_t)(d[2] >> 28 & 15) << 32 | (uint64_t)(d[2] >> 2 & 7) << 36); rq.b[rq.nb++].n = d[0] | (uint64_t)(d[2] >> 24 & 15) << 32; }
        res = g_sf[i].answer2(&rq); copy = rq.copy; move = rq.move; out2n = rq.out_len < sizeof out2 ? rq.out_len : sizeof out2; memcpy(out2, rq.out, out2n);
        pthread_mutex_unlock(&rqmu);
    } else res = g_sf[i].answer ? g_sf[i].answer(cmd & 0xffff, &copy) : RES_OK;
    if (first || trace) LOG("sf: %s object %u command %u (%u data words in): %s%s%s", g_sf[i].name, obj, cmd, ndw, res ? "fails" : g_sf[i].answer2 ? "success" : "success, zeroed output", copy ? ", a copied handle" : "", move ? ", a moved session" : "");
    // The response: no descriptors. Without a handle there is no special header, so the data words start at +8; a copied handle takes a special header (its copy count) and the handle,
    // so they start at +0x10. The CMIF part starts at +0x10 either way (16-byte aligned); everything after the headers up to the buffer's end reads as zero
    size_t end = size < 0x1000 ? size : 0x1000, dw = copy || move ? 0x10 : 8;
    memset(base, 0, end);
    m[1] = (uint32_t)((end - dw) / 4 < 0x3ff ? (end - dw) / 4 : 0x3ff);
    if (copy) { m[1] |= 1u << 31; m[2] = 1u << 1; m[3] = copy; }   // special header: no process ID, 1 copy handle, 0 move handles
    else if (move) { m[1] |= 1u << 31; m[2] = 1u << 5; m[3] = move; }   // 0 copy handles, 1 move handle
    size_t out = 0x10 + (dom ? 16 : 0);
    uint32_t *o = (uint32_t *)(base + out); o[0] = 0x4f434653; o[1] = ver; o[2] = res; o[3] = 0;   // "SFCO"; a domain's out-object count before it stays 0
    if (out2n && out + 16 + out2n <= end) memcpy(base + out + 16, out2, out2n);   // the service's output data
    return RES_OK;
}
