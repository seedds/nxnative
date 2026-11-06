// Switch 2 audio device manager. The devmgr client runs natively through hle_sf's CMIF transport.
// These are logical routes; enumeration/availability do not probe a physical microphone.
// Input attachment connects an audsmx stream to the host's default microphone.
#include "sf_service.h"

enum { AD_NAME = 0x100, AD_PROCESSES = 18 };
static const struct { const char *name; unsigned functionality, minimum, hidden, node; } ad_devices[] = {
    {"MainAudioOut", 1, 1, 0, 1}, {"MainAudioIn", 2, 1, 0, 4}, {"RawAudioIn", 2, 1, 0, 2}, {"VoiceChatIn", 2, 2, 0, 3},
    {"KaraokeIn0", 2, 1, 1, 5}, {"KaraokeIn1", 2, 1, 1, 6}, {"TrueRawAudioIn", 2, 1, 1, 7},
};
typedef struct { unsigned refs, selector; uint64_t key; } ad_process;
typedef struct { ad_process *process; } ad_session;
// hle_sf serializes every dispatch and final close, including across root sessions.
static ad_process ad_processes[AD_PROCESSES];

static uint32_t ad_u32(const void *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint64_t ad_u64(const void *p) { uint64_t v; memcpy(&v, p, sizeof v); return v; }
static int ad_shape(const sf_request *q, size_t bytes, unsigned na, unsigned nb, int pid) {
    return q->size >= bytes && q->na == na && q->nb == nb && q->send_pid == pid &&
           !q->nx && !q->nw && !q->nc && !q->ncopy && !q->nmove && !q->nobjects &&
           (!na || q->a[0].mode == 0) && (!nb || q->b[0].mode == 0);
}
static int ad_functionality(unsigned f) { return f == 1 || f == 2; }
static int ad_name_buffer(const sf_buffer *b) { return b->data && b->size > 0 && b->size < AD_NAME; }
static int ad_device(const sf_buffer *b, unsigned f) {
    // Compare the supplied length and bytes, not a zero-terminated or prefix match.
    for (unsigned k = 0; k < sizeof ad_devices / sizeof *ad_devices; k++)
        if (ad_devices[k].functionality == f && b->size == strlen(ad_devices[k].name) && !memcmp(b->data, ad_devices[k].name, b->size)) return (int)k;
    return -1;
}
static void ad_write_name(void *out, const char *name) { memset(out, 0, AD_NAME); memcpy(out, name, strlen(name)); }
static Result ad_user_request(void *state, const sf_request *q, sf_reply *r) {
    ad_session *s = state;
    unsigned f;
    switch (q->command) {
    case 0: { // ListVirtualDeviceNames: bool at +0 (unused), functionality at +4; Out<MapAlias> names[256].
        if (!ad_shape(q, 8, 0, 1, 0)) return 0x1ce0a;
        const sf_buffer *b = &q->b[0];
        if (!b->data || !b->size || (b->size & UINT64_C(0xffffff00000000ff))) return 0x499;
        f = ad_u32(q->data + 4);
        if (!ad_functionality(f)) return 0xc8899;
        uint32_t n = 0;
        for (unsigned k = 0; k < sizeof ad_devices / sizeof *ad_devices && n < b->size / AD_NAME; k++) {
            if (ad_devices[k].functionality != f || ad_devices[k].hidden || ad_devices[k].minimum > s->process->selector) continue;
            ad_write_name((uint8_t *)b->data + (size_t)n++ * AD_NAME, ad_devices[k].name);
        }
        memcpy(r->data, &n, 4); r->size = 4;
        return RES_OK;
    }
    case 100: // GetDefaultVirtualDeviceName: exactly one output record.
        if (!ad_shape(q, 4, 0, 1, 0)) return 0x1ce0a;
        if (!q->b[0].data || q->b[0].size != AD_NAME) return 0xc8e99;
        f = ad_u32(q->data);
        if (!ad_functionality(f)) return 0xc8899;
        ad_write_name(q->b[0].data, f == 1 ? "MainAudioOut" : "MainAudioIn");
        return RES_OK;
    case 200: // IsVirtualDeviceAvailable returns a Result, not a bool. Hidden routes can still be queried by name.
    case 300: // IsVirtualDeviceFormatSupported: {sample format, channels, rate}, functionality at +12.
    case 1000: { // GetVirtualDeviceStreamFormat: {PCM16=2, channels, 48000}.
        if (!ad_shape(q, q->command == 300 ? 16 : 4, 1, 0, 0)) return 0x1ce0a;
        if (!ad_name_buffer(&q->a[0])) return 0x299;
        f = ad_u32(q->data + (q->command == 300 ? 12 : 0));
        // Unlike 200/300, 1000 looks up the pair without separately validating functionality.
        if (q->command != 1000 && !ad_functionality(f)) return 0xc8899;
        if (ad_device(&q->a[0], f) < 0) return 0x299;
        if (q->command == 300) {
            unsigned format = ad_u32(q->data), channels = ad_u32(q->data + 4), rate = ad_u32(q->data + 8);
            if (format < 2 || format > 5) return 0x40699;
            if (channels != 1 && channels != 2 && channels != 6) return 0x1499;
            if (rate != 16000 && rate != 32000 && rate != 44100 && rate != 48000) return 0x699;
        } else if (q->command == 1000) {
            uint32_t format[3] = {2, f == 1 ? 6u : 2u, 48000};
            memcpy(r->data, format, sizeof format); r->size = sizeof format;
        }
        return RES_OK;
    }
    case 401: { // GetVirtualDeviceProfile; exact 0x228-byte output.
        if (!ad_shape(q, 4, 1, 1, 0)) return 0x1ce0a;
        if (!ad_name_buffer(&q->a[0])) return 0x299;
        if (!q->b[0].data || q->b[0].size != 0x228) return 0xc8e99;
        f = ad_u32(q->data);
        if (!ad_functionality(f)) return 0xc8899;
        int k = ad_device(&q->a[0], f); if (k < 0) return 0x299;
        uint8_t *out = q->b[0].data; memset(out, 0, 0x228);
        ad_write_name(out, ad_devices[k].name);
        uint32_t format[] = {3, 2, f == 1 ? 6u : 2u, 48000};
        memcpy(out + 0x100, format, sizeof format);
        // The profile contains separate node and mixer IDs. CreateStream takes
        // the mixer instance, not the route's node ID.
        ad_write_name(out + 0x110, ad_devices[k].name);
        memcpy(out + 0x210, &ad_devices[k].node, 4);
        uint64_t period = 10000000; memcpy(out + 0x218, &period, 8);
        uint32_t mixer = 1; memcpy(out + 0x220, &mixer, 4);
        return 0;
    }
    case 700: { // AttachStream: stream ID, functionality; input MapAlias name.
        if (!ad_shape(q, 8, 1, 0, 0)) return 0x1ce0a;
        if (!ad_name_buffer(&q->a[0])) return 0x299;
        f = ad_u32(q->data + 4);
        if (!ad_functionality(f)) return 0xc8899;
        int k = ad_device(&q->a[0], f); if (k < 0) return 0x299;
        if (f != 2) return 0x40299;
        return sf_audsmx_attach(s->process->key, ad_u32(q->data), ad_devices[k].node);
    }
    case 800: // DetachStream: the stream remains open and can be attached again.
        if (!ad_shape(q, 4, 0, 0, 0)) return 0x1ce0a;
        return sf_audsmx_detach(s->process->key, ad_u32(q->data));
    default:
        LOG("[RED] auddmg: user command %u is not implemented", q->command);
        return 0x1ba0a;
    }
}
static void ad_user_close(void *state) {
    ad_session *s = state;
    if (!--s->process->refs) memset(s->process, 0, sizeof *s->process);
    free(s);
}
static Result ad_root_request(void *state, const sf_request *q, sf_reply *r) {
    (void)state;
    if (q->command != 0) { LOG("[RED] auddmg: manager command %u is not implemented", q->command); return 0x1ba0a; }
    if (!ad_shape(q, 24, 0, 0, 1)) return 0x1ce0a;
    // Authenticate the optional sender identity; the process selector is a separate input.
    unsigned selector = ad_u32(q->data);
    uint64_t key = ad_u64(q->data + 8);
    if (key && key != q->pid) return 0x60a;
    ad_session *s = calloc(1, sizeof *s);
    if (!s) return 0xa99;
    ad_process *p = NULL;
    for (unsigned k = 0; k < AD_PROCESSES; k++) if (key && ad_processes[k].refs && ad_processes[k].key == key) { p = &ad_processes[k]; break; }
    if (!p) for (unsigned k = 0; k < AD_PROCESSES; k++) if (!ad_processes[k].refs) { p = &ad_processes[k]; *p = (ad_process){0, selector, key}; break; }
    if (!p) { free(s); return 0xa99; }
    // Shared process selectors only move down for a new selector of 1 or 2. Key zero creates a separate record.
    if ((selector == 1 || selector == 2) && selector < p->selector) p->selector = selector;
    p->refs++; s->process = p;
    r->child = (sf_object){ad_user_request, ad_user_close, s};
    return RES_OK;
}
sf_object sf_auddmg_root(void) { return (sf_object){ad_root_request, NULL, NULL}; }
