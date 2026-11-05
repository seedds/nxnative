// Domain IPC requests and consumer ring fields, with deterministic
// microphone samples. The real Apple device is tested separately by capture-device.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"
#include "../hle_audsmx.c"
#include "../hle_auddmg.c"
static int checks, fails, starts, stops, live_capture, allowed = 1;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
struct dm_capture { dm_capture_samples samples; void *context; int running; };
dm_capture *dm_capture_open(uint32_t rate, dm_capture_samples cb, void *ctx) {
    CHECK(rate == 16000 || rate == 32000 || rate == 44100 || rate == 48000); live_capture++;
    dm_capture *c = calloc(1, sizeof *c); c->samples = cb; c->context = ctx; return c;
}
int dm_capture_start(dm_capture *c) { if (!allowed) return 0; c->running = 1; starts++; return 1; }
void dm_capture_stop(dm_capture *c) { if (c && c->running) { c->running = 0; stops++; } }
void dm_capture_close(dm_capture *c) { if (c) { dm_capture_stop(c); free(c); live_capture--; } }
void dm_guest_write_prepare(void *addr, size_t size) { (void)addr; (void)size; }
uint64_t guest_ns(void) { return 123456789; }
static unsigned next_handle = 1, handles[128], signs[128];
uint32_t hle_new_event(uint32_t *w) { if (next_handle > 125) abort(); uint32_t r = next_handle++; *w = next_handle++; handles[r] = r; handles[*w] = r; return r; }
uint32_t hle_dup_readable_handle(uint32_t h) { if (h >= 128 || next_handle >= 128) abort(); uint32_t r = next_handle++; handles[r] = handles[h]; return r; }
int svc_thread(int num, uint64_t *regs) {
    uint32_t h = (uint32_t)regs[0]; if (h >= 128) abort(); CHECK(handles[h]);
    if (num == 0x16) handles[h] = 0; else { CHECK(num == 0x11); signs[handles[h]]++; }
    regs[0] = 0; return 1;
}
static _Alignas(16) uint32_t wire[64];
static Result request(uint32_t h, uint32_t obj, uint32_t cmd, const void *data, unsigned bytes, int pid, uint32_t copy, const void *in, size_t len) {
    memset(wire, 0, sizeof wire); wire[0] = 6 | (in ? 1u << 20 : 0);
    unsigned p = 2;
    if (pid || copy) {
        wire[1] = 0x80000000u; wire[p++] = (pid ? 1 : 0) | (copy ? 2 : 0);
        if (pid) { wire[p++] = 0; wire[p++] = 0; }
        if (copy) wire[p++] = copy;
    }
    if (in) {
        uintptr_t a = (uintptr_t)in;
        wire[p++] = (uint32_t)len; wire[p++] = (uint32_t)a;
        wire[p++] = (uint32_t)(a >> 32 & 15) << 28 | (uint32_t)(a >> 36) << 2;
    }
    unsigned cm = (p + 3) & ~3u;
    wire[1] |= cm - p + 8 + (bytes + 3) / 4;
    wire[cm] = 1 | (16 + bytes) << 16; wire[cm + 1] = obj;
    wire[cm + 4] = 0x49434653; wire[cm + 5] = 1; wire[cm + 6] = cmd;
    if (bytes) memcpy(wire + cm + 8, data, bytes);
    CHECK(h_sf_Send(h, wire, sizeof wire) == 0);
    CHECK(wire[8] == 0x4f434653);
    return wire[10];
}
static void close_object(uint32_t h, uint32_t obj) {
    memset(wire, 0, sizeof wire); wire[0] = 4; wire[1] = 8; wire[4] = 2; wire[5] = obj;
    CHECK(h_sf_Send(h, wire, sizeof wire) == 0);
}
int main(void) {
    uint32_t h = 0, root = 0;
    CHECK(h_sf_Connect(&h, "audsmx:u") == 0);
    CHECK(h_sf_ToDomain(&root, h) == 0 && root == 1);
    uint32_t factory[4] = {1, 0, 0, 0};
    CHECK(request(h, 1, 0, factory, 16, 1, 7, NULL, 0) != 0);
    CHECK(request(h, 1, 0, factory, 16, 1, 0xffff8001u, NULL, 0) == 0);
    uint32_t user = wire[12]; CHECK(user > 1);
    CHECK(request(h, 1, 0, factory, 16, 1, 0xffff8001u, NULL, 0) == 0xa99);
    uint8_t *work = aligned_alloc(4096, 8192); memset(work, 0, 8192);
    uint64_t create[] = {UINT64_C(2) << 32 | 1, 0x80, (uintptr_t)work, 8192};
    create[2] = 4096;
    CHECK(request(h, user, 1, create, 32, 1, 0, "mic", 3) == 0xc8a99);
    create[2] = (uintptr_t)work;
    CHECK(request(h, user, 1, create, 32, 1, 0, "mic", 3) == 0);
    uint32_t id = wire[12], stream = wire[13]; CHECK(id && stream > user);
    uint32_t open[] = {2, 2, 96000, 480, 0, 0};
    CHECK(request(h, stream, 0, open, 24, 0, 0, NULL, 0) == 0x699);
    open[2] = 48000; open[3] = 0;
    CHECK(request(h, stream, 0, open, 24, 0, 0, NULL, 0) == 0xc8899);
    open[3] = 480;
    CHECK(request(h, stream, 0, open, 24, 0, 0, NULL, 0) == 0 && live_capture == 1 && starts == 0);
    CHECK(request(h, stream, 0, open, 24, 0, 0, NULL, 0) == 0xc8099);
    CHECK(request(h, stream, 2, NULL, 0, 0, 0, NULL, 0) == 0 && wire[12] == 2);
    CHECK(request(h, stream, 5, NULL, 0, 0, 0, NULL, 0) == 0 && wire[2] == 2);
    uint32_t state_copy = wire[3], state_event = handles[state_copy];
    uint32_t event[] = {2, 4};
    CHECK(request(h, stream, 7, event, 8, 0, 0, NULL, 0) == 0 && wire[2] == 2);
    uint32_t data_copy = wire[3], data_event = handles[data_copy];
    CHECK(request(h, stream, 7, event, 8, 0, 0, NULL, 0) == 0xc9699);
    CHECK(sf_audsmx_attach(0x81, id, 4) == 0xcb099);
    CHECK(sf_audsmx_attach(0x80, id, 1) == 0x40299);
    allowed = 0; CHECK(sf_audsmx_attach(0x80, id, 4) != 0 && starts == 0);
    allowed = 1;
    // auddmg's real AttachStream handler selects the capture producer.
    ad_process process = {.refs = 1, .selector = 1, .key = 0x80}; ad_session device = {&process};
    uint32_t attach[] = {id, 2}; sf_reply reply = {0};
    sf_request q = {.command = 700, .data = (uint8_t *)attach, .size = 8, .na = 1, .a = {{"MainAudioIn", 11, 0}}};
    CHECK(ad_user_request(&device, &q, &reply) == 0 && starts == 1 && signs[state_event] == 1);
    CHECK(ad_user_request(&device, &q, &reply) == 0 && starts == 1);
    sx_stream *s = sx_find(0x80, id); CHECK(s != NULL);
    float pcm[] = {-1, -0.5f, 0, 0.5f, 1};
    s->capture->samples(s->capture->context, pcm, 5);
    int16_t samples[10]; memcpy(samples, work + 0x1040, sizeof samples);
    CHECK(samples[0] == -32768 && samples[1] == -32768 && samples[2] == -16384 && samples[6] == 16384 && samples[8] == 32767);
    CHECK(*(uint32_t *)(work + 0x1004) == 5 && signs[data_event] == 1);
    struct { float gain; uint32_t pad; uint64_t duration; } gain = {0, 0, 0};
    CHECK(request(h, stream, 4, &gain, 16, 0, 0, NULL, 0) == 0);
    s->capture->samples(s->capture->context, pcm, 5);
    CHECK(*(uint32_t *)(work + 0x1040 + 20) == 0);
    gain.gain = NAN;
    CHECK(request(h, stream, 4, &gain, 16, 0, 0, NULL, 0) == 0xc8899);
    q = (sf_request){.command = 800, .data = (uint8_t *)&id, .size = 4};
    CHECK(ad_user_request(&device, &q, &reply) == 0 && stops == 1 && s->state == 2);
    CHECK(request(h, stream, 8, NULL, 0, 0, 0, NULL, 0) == 0 && handles[data_copy]);
    sf_close_handle(data_copy); sf_close_handle(state_copy);
    close_object(h, user); // the stream retains its client when the user object goes away first
    CHECK(sf_audsmx_attach(0x80, id, 4) == 0 && starts == 2);
    close_object(h, stream);
    CHECK(!sx_find(0x80, id) && !live_capture && stops == 2 && !sx_process);
    for (unsigned i = 1; i < next_handle; i++) CHECK(!handles[i]);
    CHECK(request(h, 1, 0, factory, 16, 1, 0xffff8001u, NULL, 0) == 0);
    user = wire[12];
    // sysmix stores Q23 in a 32-bit word, like Q31 and float (unlike packed
    // 3-byte legacy AudioIn PCM24). Verify all supported representations/rates.
    uint32_t rates[] = {16000, 32000, 44100};
    for (uint32_t format = 3; format <= 5; format++) {
        memset(work, 0, 8192);
        CHECK(request(h, user, 1, create, 32, 1, 0, "mic", 3) == 0);
        id = wire[12]; stream = wire[13];
        open[0] = format; open[1] = 1; open[2] = rates[format - 3]; open[5] = 1;
        CHECK(request(h, stream, 0, open, 24, 0, 0, NULL, 0) == 0);
        s = sx_find(0x80, id);
        s->capture->samples(s->capture->context, pcm, 5);
        uint32_t first, last; memcpy(&first, work + 0x10c0, 4); memcpy(&last, work + 0x10c0 + 16, 4);
        CHECK(first == (format == 3 ? 0xff800000u : format == 4 ? 0x80000000u : 0xbf800000u));
        CHECK(last == (format == 3 ? 0x007fffffu : format == 4 ? 0x7fffffffu : 0x3f800000u));
        CHECK(*(uint64_t *)(work + 0x1040) == 123456789);
        close_object(h, stream);
    }
    h_sf_Close(h);
    CHECK(!sx_process);
    free(work);
    printf("t_audsmx: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
