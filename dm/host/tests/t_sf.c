// Host service sessions (hle_sf.c) without a guest: a CMIF request on a served service (olsc:u; mnpp:app with its PID / Uid and input buffer; notif:a as a domain) succeeds with zeroed output and no special header; the camera context of the Switch 2
// (nn::camera::sdk::CreateCameraContext) is a host session with no camera: Initialize's command 3 and Finalize's 4 succeed, 21 copies an event's read end to the client in the special header,
// and the device commands (QueryDeviceConnection's 29, any other) fail with 0x7c4, "no camera device" (Star Fox's camera module and its game code read it so; the earlier 0xdc4 read as "no camera support")
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"
#include "../hle_irs.c"

static int checks, fails, events;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
uint32_t hle_new_event(uint32_t *write_end) { events++; if (write_end) *write_end = 0x2000 + events; return 0x1000 + events; }
static const hle_readonly_shmem *shared;
uint32_t hle_new_readonly_shmem(const hle_readonly_shmem *memory) { shared = memory; return 0x2001; }
static uint8_t irs_work[2][0x13000];
int hle_map_transfer_memory(hle_transfer_mapping *mapping, uint32_t handle, uint64_t size, uint32_t owner_perm) {
    if (handle < 0x2002 || handle > 0x2003 || size != sizeof irs_work[0] || owner_perm) return 0;
    *mapping = (hle_transfer_mapping){ .address = (uintptr_t)irs_work[handle - 0x2002], .size = size };
    return 1;
}
void hle_unmap_transfer_memory(hle_transfer_mapping *mapping) { *mapping = (hle_transfer_mapping){0}; }

// A CMIF request for command cmd on a non-domain session (HIPC type 4, no special header, no descriptors): the data words from +8, "SFCI" at +0x10
static Result request(uint32_t h, uint32_t *m, unsigned cmd) {
    memset(m, 0xcc, 0x100);
    m[0] = 4; m[1] = 8;
    m[4] = 0x49434653; m[5] = 0; m[6] = cmd; m[7] = 0;
    return h_sf_Send(h, m, 0x100);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int calls, closes, alive;
    int gate, entered, release;
} count_svc;

static Result count_answer(sf_req *q) {
    count_svc *s = (count_svc *)q->ctx;
    pthread_mutex_lock(&s->mu);
    int ok = s->alive;
    s->calls++;
    uint32_t seq = (uint32_t)s->calls;
    if (s->gate) {
        s->entered = 1;
        pthread_cond_broadcast(&s->cv);
        while (!s->release) pthread_cond_wait(&s->cv, &s->mu);
        ok &= s->alive;
    }
    pthread_mutex_unlock(&s->mu);
    memcpy(q->out, &seq, 4);
    q->out_len = 4;
    return ok ? RES_OK : RES_FAIL;
}

static void count_close(void *ctx) {
    count_svc *s = (count_svc *)ctx;
    pthread_mutex_lock(&s->mu);
    s->alive = 0;
    s->closes++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

typedef struct {
    uint32_t h;
    uint32_t msg[64];
    Result res;
} inflight_req;

static void *inflight_worker(void *arg) {
    inflight_req *r = (inflight_req *)arg;
    r->res = request(r->h, r->msg, 7);
    return NULL;
}
// IRS commands send a PID and packed arguments. Image-transfer starts copy a handle and the state query receives a B buffer.
static Result irs_request(uint32_t h, unsigned cmd, uint32_t *m, const uint32_t *body, size_t size, uint32_t copy, void *buf, uint32_t capacity) {
    memset(m, 0, 0x100);
    m[0] = 4 | (buf ? 1u << 24 : 0);
    uint32_t *p = m + 2;
    *p++ = copy ? 3 : 1;
    *p++ = DM_APPLET_RESOURCE_USER_ID;
    *p++ = 0;
    if (copy) *p++ = copy;
    if (buf) {
        uint64_t address = (uintptr_t)buf;
        *p++ = capacity;
        *p++ = (uint32_t)address;
        *p++ = (uint32_t)(address >> 32 & 0xf) << 28 | (uint32_t)(address >> 36) << 2;
    }
    unsigned align = (unsigned)(-(p - m)) & 3;
    p += align;
    m[1] = (align + 4 + (unsigned)(size / 4)) | 1u << 31;
    p[0] = 0x49434653; p[2] = cmd;
    memcpy(p + 4, body, size);
    if (h_sf_Send(h, m, 0x100) || m[4] != 0x4f434653) return RES_FAIL;
    return m[6];
}
static Result irs_run_request(uint32_t h, unsigned cmd, uint32_t *m, const uint32_t *body, size_t size, uint32_t copy) {
    return irs_request(h, cmd, m, body, size, copy, NULL, 0);
}

static Result irs_cluster_request(uint32_t h, uint32_t *m, uint32_t camera, uint8_t gain) {
    uint32_t body[14] = { camera, 0, DM_APPLET_RESOURCE_USER_ID };
    uint32_t *config = body + 4;
    config[0] = 300000;                          // exposure time
    ((uint8_t *)config)[9] = gain;
    config[5] = 320 | 240u << 16;                // window width and height
    config[7] = 3; config[8] = 76800;           // minimum and maximum pixel count
    return irs_run_request(h, 307, m, body, sizeof body, 0);
}
static void irs_run_checks(uint32_t h, uint32_t *m) {
    const struct { unsigned cmd, size; } cases[] = {
        {306, 0x30}, {308, 0x30}, {310, 0x18}, {312, 0x18}, {316, 0x38}, {317, 0x18}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        unsigned cmd = cases[i].cmd, size = cases[i].size;
        unsigned transfer = cmd == 308 || cmd == 316;
        unsigned aruid = size == 0x18 ? 4 : 2;
        uint32_t body[14] = {0};
        body[aruid] = DM_APPLET_RESOURCE_USER_ID;
        uint8_t *config = (uint8_t *)(body + 4);
        if (size != 0x18) {
            body[4] = 300000; config[9] = 8;
        }
        if (cmd == 306) {
            body[9] = 320 | 240u << 16; config[0x1c] = 1;
        } else if (cmd == 312) {
            body[2] = 320 | 240u << 16;
        } else if (cmd == 310 || cmd == 317) {
            body[1] = body[2] = UINT32_MAX;   // opaque until the worker runs
        }
        if (transfer) body[cmd == 308 ? 10 : 12] = sizeof irs_work[0];
        uint32_t copy = transfer ? 0x2002 : 0;
        CHECK(irs_run_request(h, cmd, m, body, size, copy) == RES_OK, "valid IRS start succeeds with no camera");
        CHECK(irs_run_request(h, cmd, m, body, size - 4, copy) == 0x1ce0au, "short IRS start body rejected");
        body[0] = 9;
        CHECK(irs_run_request(h, cmd, m, body, size, copy) == 0x198cdu, "invalid camera rejected on each start");
        body[0] = 0;
        body[aruid] = DM_APPLET_RESOURCE_USER_ID + 1;
        CHECK(irs_run_request(h, cmd, m, body, size, copy) == 0x60au, "IRS rejects another applet's ID");
        body[aruid] = 0;
        CHECK(irs_run_request(h, cmd, m, body, size, copy) == RES_OK, "IRS accepts ARUID zero for the caller");
        if (transfer) {
            CHECK(irs_run_request(h, cmd, m, body, size, 0x2001) == 0x19acdu, "wrong copied handle fails transfer setup");
            CHECK(irs_run_request(h, cmd, m, body, size, 0) == 0x1ce0au, "missing copied handle is malformed");
        }
        if (cmd == 306) {
            body[9] = 7 | 6u << 16;
        } else if (cmd == 308) {
            config[0x14] = 5;
        } else if (cmd == 312) {
            body[2] = 240u << 16;
        } else if (cmd == 316) {
            config[0x14] = 0x80;
            CHECK(irs_run_request(h, cmd, m, body, size, copy) == RES_OK, "IRS decodes the original image format like firmware");
            config[0x14] = 0;
            config[0x15] = 4;
            config[0x16] = 301 & 0xff; config[0x17] = 301 >> 8;   // 20 px crop does not fit at x=301
        }
        if (cmd != 310 && cmd != 317)
            CHECK(irs_run_request(h, cmd, m, body, size, copy) == 0x19ccdu, "invalid processor configuration rejected");
    }
}

static void irs_control_checks(uint32_t h, uint32_t *m) {
    const unsigned cmds[] = {309, 313, 314, 315};
    uint32_t player[4] = {0, 0, DM_APPLET_RESOURCE_USER_ID}, handheld[4] = {8, 0, DM_APPLET_RESOURCE_USER_ID};
    uint8_t pixels[16], before[sizeof pixels];
    memset(pixels, 0xa5, sizeof pixels);
    memcpy(before, pixels, sizeof before);
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; ++i) {
        unsigned cmd = cmds[i];
        void *buf = cmd == 309 ? pixels : NULL;
        CHECK(irs_request(h, cmd, m, player, 12, 0, buf, sizeof pixels) == 0x1ce0au, "short IRS control body rejected");
        player[0] = 0x100;
        CHECK(irs_request(h, cmd, m, player, sizeof player, 0, buf, sizeof pixels) == 0x198cdu, "bad camera type rejected");
        player[0] = 0;
        player[2]++;
        CHECK(irs_request(h, cmd, m, player, sizeof player, 0, buf, sizeof pixels) == 0x60au, "other applet rejected");
        player[2]--;
    }
    CHECK(irs_run_request(h, 309, m, player, sizeof player, 0) == 0x1ce0au, "image query requires an output descriptor");
    CHECK(irs_request(h, 309, m, player, sizeof player, 0, pixels, sizeof pixels) == 0xf0cdu, "idle image query returns no image");
    CHECK(irs_run_request(h, 313, m, player, sizeof player, 0) == RES_OK, "idle suspend succeeds");

    player[1] = 3 | 11u << 16;
    CHECK(irs_run_request(h, 314, m, player, sizeof player, 0) == 0xdecdu, "first firmware check reports missing MCU");
    CHECK(irs_run_request(h, 314, m, player, sizeof player, 0) == RES_OK, "pending firmware check returns success");
    CHECK(irs_run_request(h, 314, m, handheld, sizeof handheld, 0) == 0xdecdu, "firmware check is per camera");
    player[1] = 3;
    CHECK(irs_run_request(h, 315, m, player, sizeof player, 0) == RES_OK && irs_cameras[0].function_level == 3,
          "IRS stores the requested function level for the camera");

    uint32_t image[14] = {0, 0, DM_APPLET_RESOURCE_USER_ID, 0, 300000, 0, 8u << 8};
    image[10] = sizeof irs_work[0];
    CHECK(irs_run_request(h, 308, m, image, 0x30, 0x2002) == RES_OK, "start image transfer for player one");
    image[0] = 8; image[10] = 0; image[12] = sizeof irs_work[0];
    CHECK(irs_run_request(h, 316, m, image, sizeof image, 0x2003) == RES_OK, "start extended transfer for handheld");
    CHECK(irs_request(h, 309, m, player, sizeof player, 0, pixels, sizeof pixels) == 0xf0cdu,
          "image query after start still returns no image");
    CHECK(irs_run_request(h, 313, m, player, sizeof player, 0) == RES_OK &&
          !irs_cameras[0].transfer.size && irs_cameras[8].transfer.size, "suspend releases only its camera's mapping");
    CHECK(irs_request(h, 309, m, player, sizeof player, 0, pixels, sizeof pixels) == 0xf0cdu &&
          !memcmp(pixels, before, sizeof pixels), "image queries leave the output buffer untouched");
    CHECK(irs_run_request(h, 313, m, handheld, sizeof handheld, 0) == RES_OK && !irs_cameras[8].transfer.size,
          "suspend also releases an extended transfer mapping");
    CHECK(irs_run_request(h, 316, m, image, sizeof image, 0x2003) == RES_OK, "restart after suspend");
    CHECK(irs_run_request(h, 318, m, handheld, sizeof handheld, 0) == RES_OK && !irs_cameras[8].transfer.size,
          "stop releases the extended transfer mapping");
    CHECK(irs_run_request(h, 316, m, image, sizeof image, 0x2003) == RES_OK, "restart after stop");
    CHECK(irs_run_request(h, 303, m, player + 2, 8, 0) == RES_OK && !irs_cameras[8].transfer.size,
          "deactivation releases remaining mappings");
    CHECK(irs_run_request(h, 314, m, player, sizeof player, 0) == 0xdecdu, "deactivation clears pending firmware checks");
}
// The same on a domain session as the SDK's notif:a client sends it: the process ID in a special header when pid, one B descriptor (the buffer buf) when buf, the domain header for
// object 1 at the first 16-byte boundary after them, then "SFCI"
static Result domain_request(uint32_t h, uint32_t *m, unsigned cmd, int pid, void *buf, uint32_t size) {
    memset(m, 0xcc, 0x100);
    m[0] = 4 | (buf ? 1u << 24 : 0); m[1] = 16 | (pid ? 1u << 31 : 0);
    uint32_t *p = m + 2;
    if (pid) { *p++ = 1; *p++ = 0; *p++ = 0; }
    if (buf) { uint64_t a = (uint64_t)(uintptr_t)buf; *p++ = size; *p++ = (uint32_t)a; *p++ = (uint32_t)(a >> 32 & 0xf) << 28 | (uint32_t)(a >> 36 & 7) << 2; }
    size_t cm = ((size_t)((uint8_t *)p - (uint8_t *)m) + 15) & ~(size_t)15;
    uint32_t *d = (uint32_t *)((uint8_t *)m + cm);
    d[0] = 1 | 16u << 16; d[1] = 1; d[2] = d[3] = 0;   // domain header: send a message to object 1, 16 bytes of data
    d[4] = 0x49434653; d[5] = 0; d[6] = cmd; d[7] = 0;
    return h_sf_Send(h, m, 0x100);
}
int main(void) {
    uint32_t m[0x40], h = 0;
    CHECK(h_sf_Connect(&h, "olsc:u") == RES_OK && h, "olsc:u is served");
    CHECK(request(h, m, 0) == RES_OK, "olsc:u request");
    CHECK(!(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "olsc:u: success, no special header, SFCO at +0x10");
    CHECK(h_sf_Connect(&h, "chat:u") == 0xe15u, "a service the host does not serve is not registered");

    uint32_t irs = 0;
    CHECK(h_sf_Connect(&irs, "irs") == RES_OK && irs, "IRS has a direct service session");
    if (irs) {
        memset(m, 0, sizeof m);
        m[0] = 4; m[1] = 9; m[4] = 0x49434653; m[6] = 311; m[8] = 0x20;
        CHECK(h_sf_Send(irs, m, sizeof m) == RES_OK && m[6] == RES_OK && m[8] == 8, "IRS handheld camera handle selects device eight");
        CHECK(request(irs, m, 302) == RES_OK && m[6] == RES_OK, "IRS activation succeeds");
        CHECK(request(irs, m, 304) == RES_OK && m[6] == RES_OK && m[3] == 0x2001 && m[1] >> 31 && m[2] == 2,
              "IRS returns one copied shared memory handle");
        CHECK(shared && shared->size == 0x8000, "IRS status manager has its ABI size");
        irs_control_checks(irs, m);
        irs_run_checks(irs, m);
        if (shared) {
            const uint32_t *words = shared->bytes;
            for (size_t camera = 0; camera < 9; ++camera) {
                size_t at = camera * 0xe30 / sizeof *words;
                CHECK(words[at] == 2 && words[at + 1] == 0 && words[at + 2] == 0 && words[at + 6] == 0,
                      "each camera is disconnected, stopped and has no samples");
            }
        }
        CHECK(irs_cluster_request(irs, m, 0, 1) == RES_OK, "IRS accepts minimum clustering gain without a camera");
        CHECK(irs_cluster_request(irs, m, 0xffff0008, 16) == RES_OK,
              "IRS accepts handheld, ignores reserved handle bytes and accepts maximum clustering gain");
        CHECK(irs_cluster_request(irs, m, 9, 8) == 0x198cdu, "IRS rejects an out-of-range clustering player index");
        CHECK(irs_cluster_request(irs, m, 0x100, 8) == 0x198cdu, "IRS rejects a nonzero clustering camera type");
        CHECK(irs_cluster_request(irs, m, 0, 0) == 0x19ccdu, "IRS rejects clustering gain below one");
        CHECK(irs_cluster_request(irs, m, 0, 17) == 0x19ccdu, "IRS rejects clustering gain above sixteen");
        CHECK(request(irs, m, 307) == RES_OK && m[6] == 0x1ce0au, "IRS rejects a truncated clustering configuration");
        h_sf_Close(irs);
    }

    uint32_t mn = 0, unserved = 0;
    CHECK(h_sf_Connect(&mn, "mnpp:app") == RES_OK && mn && mn != h, "mnpp:app is served");
    CHECK(h_sf_Connect(&unserved, "mnpp:sys") == 0xe15u && h_sf_Connect(&unserved, "mnpp:web") == 0xe15u, "other mnpp service names remain unserved");
    if (mn) {
        uint8_t empty[sizeof m - 0x20] = {0};
        // Initialize: PID in a special header, reserved u64 after SFCI; 3 alignment words + 4 CMIF words + 2 input words.
        memset(m, 0xcc, sizeof m);
        m[0] = 4; m[1] = 9 | 1u << 31;
        m[2] = 1; m[3] = 0x1234; m[4] = 0;   // PID descriptor, no copied or moved handles
        m[8] = 0x49434653; m[9] = 0; m[10] = 0; m[11] = 0;
        m[12] = m[13] = 0;
        CHECK(h_sf_Send(mn, m, sizeof m) == RES_OK, "mnpp:app Initialize with a PID and reserved u64");
        CHECK(m[0] == 0 && !(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "Initialize: success, no descriptors or handles");
        CHECK(!memcmp(m + 8, empty, sizeof empty), "Initialize returns no output data");

        // SubmitEvent: Uid after SFCI and a type-0x5 input buffer (one A descriptor); no output or retained event.
        uint8_t event[32], before[sizeof event];
        memset(event, 0xa5, sizeof event); memcpy(before, event, sizeof event);
        memset(m, 0xcc, sizeof m);
        uint64_t address = (uint64_t)(uintptr_t)event;
        m[0] = 4 | 1u << 20; m[1] = 11;   // 3 alignment words + 4 CMIF words + 4 Uid words
        m[2] = sizeof event; m[3] = (uint32_t)address;
        m[4] = (uint32_t)(address >> 32 & 0xf) << 28 | (uint32_t)(address >> 36 & 0x3fffff) << 2;
        m[8] = 0x49434653; m[9] = 0; m[10] = 1; m[11] = 0;
        m[12] = 1; m[13] = 2; m[14] = 3; m[15] = 4;
        CHECK(h_sf_Send(mn, m, sizeof m) == RES_OK, "mnpp:app SubmitEvent with a Uid and input buffer");
        CHECK(m[0] == 0 && !(m[1] >> 31) && m[4] == 0x4f434653 && m[6] == RES_OK, "SubmitEvent: success, no descriptors or handles");
        CHECK(!memcmp(m + 8, empty, sizeof empty), "SubmitEvent returns no output data");
        CHECK(!memcmp(event, before, sizeof event), "SubmitEvent leaves the input buffer untouched");
        h_sf_Close(mn);
    }

    // notif:a as nn::notification::ListAlarmSettings reaches it: connect, a domain, Initialize with the process ID, then List into the caller's array, which stays untouched
    uint32_t notif = 0, obj = 0; uint8_t alarms[8 * 0x40], before[sizeof alarms];
    memset(alarms, 0xa5, sizeof alarms); memcpy(before, alarms, sizeof alarms);
    CHECK(h_sf_Connect(&notif, "notif:a") == RES_OK && notif, "notif:a is served");
    CHECK(h_sf_ToDomain(&obj, notif) == RES_OK && obj == 1, "notif:a converts to a domain");
    CHECK(domain_request(notif, m, 1000, 1, NULL, 0) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK, "notif:a Initialize with the process ID: success");
    CHECK(domain_request(notif, m, 520, 0, alarms, sizeof alarms) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK, "notif:a ListAlarmSettings: success");
    CHECK(m[12] == 0, "no alarms listed");
    CHECK(!memcmp(alarms, before, sizeof alarms), "the caller's alarm array is not written");
    CHECK(domain_request(notif, m, 500, 0, alarms, 0x40) == RES_OK && m[10] == RES_OK && m[12] == 0, "notif:a RegisterAlarmSetting: success, ID 0");

    uint32_t cam = 0;
    CHECK(h_cam_Context(&cam) == RES_OK && cam, "the camera context is a host session");
    CHECK(request(cam, m, 3) == RES_OK && m[4] == 0x4f434653 && m[6] == RES_OK && !(m[1] >> 31), "initializing the camera library: success");
    CHECK(request(cam, m, 29) == RES_OK && m[6] == 0x7c4u && !(m[1] >> 31), "querying the device: 0x7c4, no camera device");
    CHECK(events == 0, "no event made for a failing command");
    CHECK(request(cam, m, 21) == RES_OK, "the camera's event");
    CHECK(m[1] >> 31 && (m[2] >> 1 & 15) == 1 && (m[2] & 1) == 0 && (m[2] >> 5 & 15) == 0, "special header: one copy handle, no process ID, no move handle");
    CHECK(m[3] == 0x1001 && events == 1, "the copied handle is the new event's read end");
    CHECK(m[4] == 0x4f434653 && m[6] == RES_OK, "SFCO stays at +0x10 after the special header and handle");
    CHECK((m[1] & 0x3ff) == (0x100 - 0x10) / 4, "the data words start after the handle");
    uint32_t dup = 0;
    CHECK(h_sf_Dup(&dup, cam) == RES_OK && dup && dup != cam, "a duplicate of the camera session");
    CHECK(request(dup, m, 5) == RES_OK && m[6] == 0x7c4u, "the duplicate answers as the camera");
    CHECK(request(dup, m, 4) == RES_OK && m[6] == RES_OK, "finalizing the camera library: success");
    h_sf_Close(dup); h_sf_Close(cam); h_sf_Close(h);

    // Shared ownership across duplicate handles (closing in both orders), per-slot domain flags, and concurrent close during an in-flight request
    {
        // Order 1: duplicate then close original first; duplicate still responds and destructor runs once on final close
        count_svc s1 = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .alive = 1};
        uint32_t orig = sf_session("count1", count_answer, &s1, count_close), d1 = 0, d2 = 0, dom_obj = 0;
        CHECK(orig && h_sf_Dup(&d1, orig) == RES_OK && d1 && d1 != orig, "duplicate counting service (order 1)");
        CHECK(h_sf_DupEx(&d2, d1, 3) == RES_OK && d2 && d2 != orig && d2 != d1, "DuplicateSessionEx clones duplicate");
        CHECK(h_sf_ToDomain(&dom_obj, d2) == RES_OK && dom_obj == 1, "convert one duplicate to domain");
        h_sf_Close(orig);
        CHECK(s1.closes == 0 && s1.alive == 1, "closing original leaves shared owner alive");
        CHECK(request(orig, m, 1) == 0xe401u, "closed original handle is rejected");
        CHECK(request(d1, m, 1) == RES_OK && m[4] == 0x4f434653 && m[6] == RES_OK && m[8] == 1, "non-domain duplicate still responds after original closes");
        CHECK(domain_request(d2, m, 2, 0, NULL, 0) == RES_OK && m[8] == 0x4f434653 && m[10] == RES_OK && m[12] == 2, "domain duplicate keeps independent domain flag");
        h_sf_Close(d1);
        CHECK(s1.closes == 0 && s1.alive == 1, "closing second handle still keeps shared owner alive");
        CHECK(domain_request(d2, m, 3, 0, NULL, 0) == RES_OK && m[10] == RES_OK && m[12] == 3, "last remaining handle still responds");
        h_sf_Close(d2);
        CHECK(s1.closes == 1 && s1.alive == 0 && s1.calls == 3, "destructor runs exactly once after last handle closes (original-first)");

        // Order 2: duplicate then close duplicate first; original still responds and destructor runs once on final close
        count_svc s2 = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .alive = 1};
        orig = sf_session("count2", count_answer, &s2, count_close); d1 = 0;
        CHECK(orig && h_sf_Dup(&d1, orig) == RES_OK && d1 != orig, "duplicate counting service (order 2)");
        h_sf_Close(d1);
        CHECK(s2.closes == 0 && s2.alive == 1, "closing duplicate first leaves original alive");
        CHECK(request(orig, m, 1) == RES_OK && m[6] == RES_OK && m[8] == 1, "original responds after duplicate closes");
        h_sf_Close(orig);
        CHECK(s2.closes == 1 && s2.alive == 0 && s2.calls == 1, "destructor runs exactly once after last handle closes (duplicate-first)");

        // In-flight request coordinated with concurrent handle close and slot reuse
        count_svc s3 = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .alive = 1, .gate = 1};
        uint32_t live = sf_session("count3", count_answer, &s3, count_close);
        inflight_req req = {.h = live};
        pthread_t th;
        CHECK(!pthread_create(&th, NULL, inflight_worker, &req), "start in-flight request thread");
        pthread_mutex_lock(&s3.mu);
        while (!s3.entered) pthread_cond_wait(&s3.cv, &s3.mu);
        pthread_mutex_unlock(&s3.mu);

        h_sf_Close(live);
        CHECK(sf_slot(live) < 0, "handle slot closes while request is in flight");
        uint32_t reused = sf_session("reused", NULL, NULL, NULL);
        CHECK(reused == live, "closed slot can be reused while earlier request is still running");
        pthread_mutex_lock(&s3.mu);
        CHECK(s3.closes == 0 && s3.alive == 1, "destructor waits while request holds temporary owner reference");
        s3.release = 1;
        pthread_cond_broadcast(&s3.cv);
        pthread_mutex_unlock(&s3.mu);

        pthread_join(th, NULL);
        CHECK(req.res == RES_OK && req.msg[4] == 0x4f434653 && req.msg[6] == RES_OK && req.msg[8] == 1,
              "in-flight request completes reply after handle closed and slot was reused");
        CHECK(s3.closes == 1 && s3.alive == 0 && s3.calls == 1, "destructor runs exactly once when last in-flight request finishes");
        h_sf_Close(reused);
    }
    printf("t_sf: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
