// Audio-device service fixtures, driven through the real HIPC/CMIF transport. No game files or audio device required.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_sf.c"
#include "../hle_auddmg.c"
sf_object sf_audsmx_root(void) { abort(); }
Result sf_audsmx_attach(uint64_t p, uint32_t s, uint32_t d) { (void)p; (void)s; (void)d; abort(); }
Result sf_audsmx_detach(uint64_t p, uint32_t s) { (void)p; (void)s; abort(); }

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
uint32_t hle_new_event(uint32_t *write_end) { (void)write_end; abort(); }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; abort(); }

static void map_alias(uint32_t *p, void *addr, size_t size) {
    uintptr_t a = (uintptr_t)addr;
    p[0] = (uint32_t)size; p[1] = (uint32_t)a;
    p[2] = (uint32_t)(a >> 32 & 15) << 28 | (uint32_t)(a >> 36) << 2 | (uint32_t)(size >> 32 & 15) << 24;
}
static Result factory(uint32_t root, unsigned selector, uint64_t key, uint32_t *child) {
    // Factory request: 14 data words, SendPid, 24 inline bytes.
    _Alignas(16) uint32_t m[64] = {6, 0x8000000e, 1, 0xdeadbeef, 0xdeadbeef};
    m[8] = 0x49434653; m[9] = 1; m[11] = 0xabc;
    m[12] = selector; memcpy(m + 14, &key, 8);
    CHECK(h_sf_Send(root, m, sizeof m) == 0, "factory transport");
    if (!m[6]) {
        CHECK(m[1] >> 31 && m[2] == 32 && m[4] == 0x4f434653 && m[7] == 0xabc, "factory returns one moved child session");
        *child = m[3];
    }
    return m[6];
}
static Result query(uint32_t session, uint32_t cmd, const void *args, size_t argsize, void *in, size_t insize, void *out, size_t outsize, uint32_t *reply, size_t replysize) {
    _Alignas(16) uint32_t m[64] = {0};
    unsigned na = in != NULL || insize != 0, nb = out != NULL || outsize != 0;
    m[0] = 6 | na << 20 | nb << 24;
    unsigned pos = 2;
    if (na) { map_alias(m + pos, in, insize); pos += 3; }
    if (nb) { map_alias(m + pos, out, outsize); pos += 3; }
    unsigned cm = (pos + 3) & ~3u;
    m[1] = (uint32_t)((cm - pos) + 4 + (argsize + 3) / 4);
    m[cm] = 0x49434653; m[cm + 1] = 1; m[cm + 2] = cmd; m[cm + 3] = 0xdef;
    if (argsize) memcpy(m + cm + 4, args, argsize);
    CHECK(h_sf_Send(session, m, sizeof m) == 0 && m[4] == 0x4f434653 && m[7] == 0xdef, "query CMIF reply and context");
    if (!m[6] && replysize) memcpy(reply, m + 8, replysize);
    return m[6];
}
static unsigned list(uint32_t session, unsigned functionality, unsigned flag, uint8_t *names, size_t size, Result expected) {
    uint32_t args[2] = {flag, functionality}, n = 0xfeedface;
    CHECK(query(session, 0, args, sizeof args, NULL, 0, names, size, &n, 4) == expected, "list result");
    return n;
}
int main(void) {
    uint32_t root = 0, session = 0;
    CHECK(h_sf_Connect(&root, "auddmg:u") == 0, "device manager service connects");
    CHECK(factory(root, 2, 0x81, &session) == 0x60a && !session, "factory rejects a mismatched asserted process identity");
    CHECK(factory(root, 2, 0x80, &session) == 0 && session, "factory accepts the guest application identity");
    uint8_t names[4 * 256 + 1]; memset(names, 0xcc, sizeof names);
    CHECK(list(session, 2, 0, names, 4 * 256, 0) == 3, "selector 2 exposes three input routes");
    CHECK(!strcmp((char *)names, "MainAudioIn") && !strcmp((char *)names + 256, "RawAudioIn") && !strcmp((char *)names + 512, "VoiceChatIn"), "virtual route names and order");
    CHECK(names[11] == 0 && names[255] == 0 && names[767] == 0 && names[768] == 0xcc && names[1024] == 0xcc, "returned records padded; unused records and guard untouched");
    CHECK(list(session, 2, 1, names, 256, 0) == 1 && !strcmp((char *)names, "MainAudioIn"), "capacity limits enumeration; bool does not expose hidden routes");
    CHECK(list(session, 1, 0, names, 256, 0) == 1 && !strcmp((char *)names, "MainAudioOut"), "output functionality catalogue");
    list(session, 0, 0, names, 256, 0xc8899);
    list(session, 2, 0, names, 255, 0x499);
    list(session, 2, 0, names, 0, 0x499);
    list(session, 2, 0, NULL, 256, 0x499);
    uint32_t f = 2, fmt[4] = {2, 2, 48000, 2}, output[3] = {0};
    CHECK(query(session, 100, &f, 4, NULL, 0, names, 256, NULL, 0) == 0 && !strcmp((char *)names, "MainAudioIn"), "default input route");
    CHECK(query(session, 100, &f, 4, NULL, 0, names, 512, NULL, 0) == 0xc8e99, "default output must be exactly 256 bytes");
    char input[] = "MainAudioIn", hidden[] = "TrueRawAudioIn", unknown[] = "Microphone";
    CHECK(query(session, 200, &f, 4, input, 11, NULL, 0, NULL, 0) == 0, "known logical input is available");
    CHECK(query(session, 200, &f, 4, hidden, 14, NULL, 0, NULL, 0) == 0, "hidden route is queryable by exact name");
    CHECK(query(session, 200, &f, 4, input, 12, NULL, 0, NULL, 0) == 0x299, "name descriptor excludes terminator; no prefix match");
    CHECK(query(session, 200, &f, 4, unknown, 10, NULL, 0, NULL, 0) == 0x299, "unknown route fails");
    CHECK(query(session, 200, &f, 4, names, 256, NULL, 0, NULL, 0) == 0x299, "oversized input name rejected before lookup");
    CHECK(query(session, 1000, &f, 4, input, 11, NULL, 0, output, sizeof output) == 0 && output[0] == 2 && output[1] == 2 && output[2] == 48000, "input stream format is PCM16 stereo 48 kHz");
    f = 1;
    CHECK(query(session, 1000, &f, 4, names, 0, NULL, 0, output, sizeof output) == 0x299, "zero length name rejected");
    char speaker[] = "MainAudioOut";
    CHECK(query(session, 1000, &f, 4, speaker, 12, NULL, 0, output, sizeof output) == 0 && output[1] == 6, "logical output stream has six channels");
    f = 3;
    CHECK(query(session, 200, &f, 4, input, 11, NULL, 0, NULL, 0) == 0xc8899, "availability validates functionality");
    CHECK(query(session, 1000, &f, 4, input, 11, NULL, 0, NULL, 0) == 0x299, "format query uses exact name/functionality lookup");
    CHECK(query(session, 300, fmt, sizeof fmt, input, 11, NULL, 0, NULL, 0) == 0, "supported input format");
    fmt[0] = 1;
    CHECK(query(session, 300, fmt, sizeof fmt, input, 11, NULL, 0, NULL, 0) == 0x40699, "unsupported sample representation");
    fmt[0] = 5; fmt[1] = 3;
    CHECK(query(session, 300, fmt, sizeof fmt, input, 11, NULL, 0, NULL, 0) == 0x1499, "unsupported channel count");
    fmt[1] = 6; fmt[2] = 96000;
    CHECK(query(session, 300, fmt, sizeof fmt, input, 11, NULL, 0, NULL, 0) == 0x699, "unsupported sample rate");
    fmt[2] = 16000;
    CHECK(query(session, 300, fmt, sizeof fmt, input, 11, NULL, 0, NULL, 0) == 0, "format capability range");
    CHECK(query(session, 0, &f, 4, NULL, 0, names, 256, NULL, 0) == 0x1ce0a, "truncated inline request rejected");
    CHECK(query(session, 401, &f, 4, NULL, 0, NULL, 0, NULL, 0) == 0x1ce0a, "profile needs input and output descriptors");
    uint8_t profile[0x229]; memset(profile, 0xcc, sizeof profile); f = 2;
    CHECK(query(session, 401, &f, 4, input, 11, profile, 0x228, NULL, 0) == 0, "input profile succeeds");
    CHECK(!strcmp((char *)profile, input) && ad_u32(profile + 0x100) == 3 && ad_u32(profile + 0x104) == 2 && ad_u32(profile + 0x108) == 2 && ad_u32(profile + 0x10c) == 48000, "profile names and stream format wire offsets");
    CHECK(ad_u32(profile + 0x210) == 4 && ad_u64(profile + 0x218) == 10000000 && ad_u32(profile + 0x220) == 1 && profile[0x228] == 0xcc, "profile input node, period, mixer ID and output bounds");
    CHECK(query(session, 401, &f, 4, input, 11, profile, 0x227, NULL, 0) == 0xc8e99, "profile requires exact output size");
    uint32_t second = 0;
    CHECK(factory(root, 1, 0x80, &second) == 0 && list(session, 2, 0, names, 1024, 0) == 2, "same identity shares the lowest selector");
    h_sf_Close(second); h_sf_Close(session);
    CHECK(factory(root, 2, 0x80, &session) == 0 && list(session, 2, 0, names, 1024, 0) == 3, "last close releases process selector");
    h_sf_Close(session);
    CHECK(factory(root, 1, 0, &session) == 0 && factory(root, 2, 0, &second) == 0, "anonymous factory records are distinct");
    h_sf_Close(root);
    CHECK(list(session, 2, 0, names, 1024, 0) == 2 && list(second, 2, 0, names, 1024, 0) == 3, "independent anonymous selectors survive root close");
    h_sf_Close(session); h_sf_Close(second);
    printf("t_auddmg: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
