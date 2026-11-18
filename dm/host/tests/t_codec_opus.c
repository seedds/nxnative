// Hardware Opus decoder forwarding (hle_codec.c) without a guest:
// - C-initialized decoders (InitializeHardwareOpusDecoder / MultiStream) keep host state in objmap.
// - Mixed C++/C decoders (constructed/initialized via HardwareOpusDecoder aliased to OpusDecoder, then passed to the C functions via GetBase() at this + 0x08) carry the
//   OpusDecoder / OpusMultiStreamDecoder vtable at hw - 8 and forward directly to the guest software object in place.
// - Null, low, unmapped-page-boundary, wrong-vtable, and uninitialized pointers fail cleanly instead of faulting or passing.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#undef HLE_FORCE_ALIAS
#define HLE_FORCE_ALIAS(mangled, target, why) typedef int hle_alias_unused_##__LINE__
#include <assert.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

static jmp_buf g_die_jmp;
static int g_die_catch, g_died;
#undef die
#define die(...) do { if (g_die_catch) { g_died = 1; longjmp(g_die_jmp, 1); } abort(); } while (0)

#include "../hle_codec.c"

static struct { const void *k; void *v; } g_om[16];
void *objmap_get(const void *k) { for (int i = 0; i < 16; i++) if (g_om[i].k == k) return g_om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 16; i++) if (!g_om[i].k) { g_om[i].k = k; g_om[i].v = v; return; } abort(); }
void *objmap_del(const void *k) { for (int i = 0; i < 16; i++) if (g_om[i].k == k) { void *v = g_om[i].v; g_om[i].k = g_om[i].v = NULL; return v; } return NULL; }
void hle_bind_system_event(void *e, int autoclear) { (void)e; (void)autoclear; }
void hle_signal_system_event(void *e) { (void)e; }

static void *g_vtbl_single[4];
static void *g_vtbl_multi[4];
static void *g_last_arg;
static int g_init_calls, g_fini_calls, g_decode_calls, g_reset_calls;
static int g_ms_init_calls, g_ms_fini_calls, g_ms_decode_calls, g_ms_reset_calls;

static int fake_init(void *s, int rate, int ch, void *buf, size_t size) {
    (void)rate; (void)ch; (void)buf; (void)size;
    g_last_arg = s; g_init_calls++; return 0;
}
static void fake_fini(void *s) { g_last_arg = s; g_fini_calls++; }
static int fake_rate(const void *s) { g_last_arg = (void *)s; return 48000; }
static int fake_ch(const void *s) { g_last_arg = (void *)s; return 2; }
static void fake_reset(void *s) { g_last_arg = s; g_reset_calls++; }
static int fake_decode(void *s, size_t *c, int *smp, int16_t *out, size_t osz, const void *in, size_t isz) {
    (void)out; (void)osz; (void)in; (void)isz;
    g_last_arg = s; *c = 12; *smp = 960; g_decode_calls++; return 0;
}

static int fake_ms_init(void *s, int r, int ch, int st, int str, const uint8_t *m, void *b, size_t sz) {
    (void)r; (void)ch; (void)st; (void)str; (void)m; (void)b; (void)sz;
    g_last_arg = s; g_ms_init_calls++; return 0;
}
static void fake_ms_fini(void *s) { g_last_arg = s; g_ms_fini_calls++; }
static int fake_ms_rate(const void *s) { g_last_arg = (void *)s; return 48000; }
static int fake_ms_ch(const void *s) { g_last_arg = (void *)s; return 6; }
static int fake_ms_total(const void *s) { g_last_arg = (void *)s; return 4; }
static int fake_ms_stereo(const void *s) { g_last_arg = (void *)s; return 2; }
static void fake_ms_reset(void *s) { g_last_arg = s; g_ms_reset_calls++; }
static int fake_ms_decode(void *s, size_t *c, int *smp, int16_t *out, size_t osz, const void *in, size_t isz) {
    (void)out; (void)osz; (void)in; (void)isz;
    g_last_arg = s; *c = 24; *smp = 960; g_ms_decode_calls++; return 0;
}

void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZTVN2nn5codec11OpusDecoderE")) return g_vtbl_single;
    if (!strcmp(name, "_ZTVN2nn5codec22OpusMultiStreamDecoderE")) return g_vtbl_multi;
    if (!strcmp(name, "_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm")) return (void *)fake_init;
    if (!strcmp(name, "_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE")) return (void *)fake_fini;
    if (!strcmp(name, "_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE")) return (void *)fake_rate;
    if (!strcmp(name, "_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE")) return (void *)fake_ch;
    if (!strcmp(name, "_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE")) return (void *)fake_reset;
    if (!strcmp(name, "_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm")) return (void *)fake_decode;
    if (!strcmp(name, "_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm")) return (void *)fake_ms_init;
    if (!strcmp(name, "_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE")) return (void *)fake_ms_fini;
    if (!strcmp(name, "_ZN2nn5codec35GetOpusMultiStreamDecoderSampleRateEPKNS0_26OpusMultiStreamDecoderTypeE")) return (void *)fake_ms_rate;
    if (!strcmp(name, "_ZN2nn5codec37GetOpusMultiStreamDecoderChannelCountEPKNS0_26OpusMultiStreamDecoderTypeE")) return (void *)fake_ms_ch;
    if (!strcmp(name, "_ZN2nn5codec41GetOpusMultiStreamDecoderTotalStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE")) return (void *)fake_ms_total;
    if (!strcmp(name, "_ZN2nn5codec42GetOpusMultiStreamDecoderStereoStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE")) return (void *)fake_ms_stereo;
    if (!strcmp(name, "_ZN2nn5codec27ResetOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE")) return (void *)fake_ms_reset;
    if (!strcmp(name, "_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm")) return (void *)fake_ms_decode;
    return NULL;
}

typedef struct { void *vptr; uint64_t body[7]; } fake_cpp_decoder;

static int must_die(void (*fn)(void *), void *arg) {
    g_died = 0; g_die_catch = 1;
    if (!setjmp(g_die_jmp)) fn(arg);
    g_die_catch = 0;
    return g_died;
}
static void call_decode(void *hw) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};
    (void)h_codec_HwOpusDecode(hw, &c, &smp, out, sizeof out, in, sizeof in);
}
static void call_ms_decode(void *hw) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};
    (void)h_codec_HwOpusMsDecode(hw, &c, &smp, out, sizeof out, in, sizeof in);
}

int main(void) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};

    // 1. C API: objmap takes precedence over any surrounding memory.
    uint64_t c_obj[8] = {0};
    assert(h_codec_HwOpusInit(c_obj, 48000, 2, in, sizeof in, 0) == 0);
    assert(g_init_calls == 1 && g_last_arg != c_obj && objmap_get(c_obj) == g_last_arg);
    assert(h_codec_HwOpusDecode(c_obj, &c, &smp, out, sizeof out, in, sizeof in) == 0);
    assert(g_decode_calls == 1 && g_last_arg == objmap_get(c_obj) && c == 12 && smp == 960);
    assert(h_codec_HwOpusRate(c_obj) == 48000 && h_codec_HwOpusCh(c_obj) == 2);
    h_codec_HwOpusReset(c_obj); assert(g_reset_calls == 1);
    h_codec_HwOpusFini(c_obj); assert(g_fini_calls == 1 && objmap_get(c_obj) == NULL);

    // 2. Mixed C++/C single-stream: OpusDecoder vtable at obj+0, GetBase() = obj+8 passed to C API.
    fake_cpp_decoder cpp = {.vptr = &g_vtbl_single[2]};
    void *base = (void *)cpp.body;
    assert(opus_class_member(base, "_ZTVN2nn5codec11OpusDecoderE") == 1);
    assert(opus_class_member(base, "_ZTVN2nn5codec22OpusMultiStreamDecoderE") == 0);
    assert(h_codec_HwOpusDecode(base, &c, &smp, out, sizeof out, in, sizeof in) == 0);
    assert(g_decode_calls == 2 && g_last_arg == base);
    assert(h_codec_HwOpusRate(base) == 48000 && g_last_arg == base);
    assert(h_codec_HwOpusCh(base) == 2 && g_last_arg == base);
    h_codec_HwOpusReset(base); assert(g_reset_calls == 2 && g_last_arg == base);
    h_codec_HwOpusFini(base); assert(g_fini_calls == 2 && g_last_arg == base);

    // 3. Multi-stream C API and mixed C++/C multi-stream (previously routed to opus_soft instead of opus_ms_soft).
    uint64_t ms_obj[8] = {0}; uint8_t map[2] = {0, 1};
    assert(h_codec_HwOpusMsInit(ms_obj, 48000, 6, 4, 2, map, in, sizeof in, 0) == 0);
    assert(g_ms_init_calls == 1 && objmap_get(ms_obj) == g_last_arg);
    assert(h_codec_HwOpusMsDecode(ms_obj, &c, &smp, out, sizeof out, in, sizeof in) == 0);
    assert(g_ms_decode_calls == 1 && g_last_arg == objmap_get(ms_obj) && c == 24);
    assert(h_codec_HwOpusMsRate(ms_obj) == 48000 && h_codec_HwOpusMsCh(ms_obj) == 6);
    assert(h_codec_HwOpusMsTotal(ms_obj) == 4 && h_codec_HwOpusMsStereo(ms_obj) == 2);
    h_codec_HwOpusMsReset(ms_obj); assert(g_ms_reset_calls == 1);
    h_codec_HwOpusMsFini(ms_obj); assert(g_ms_fini_calls == 1 && objmap_get(ms_obj) == NULL);

    fake_cpp_decoder ms_cpp = {.vptr = &g_vtbl_multi[2]};
    void *ms_base = (void *)ms_cpp.body;
    assert(h_codec_HwOpusMsDecode(ms_base, &c, &smp, out, sizeof out, in, sizeof in) == 0);
    assert(g_ms_decode_calls == 2 && g_last_arg == ms_base);
    assert(h_codec_HwOpusMsCh(ms_base) == 6 && h_codec_HwOpusMsTotal(ms_base) == 4 && h_codec_HwOpusMsStereo(ms_base) == 2);
    h_codec_HwOpusMsReset(ms_base); assert(g_ms_reset_calls == 2 && g_last_arg == ms_base);
    h_codec_HwOpusMsFini(ms_base); assert(g_ms_fini_calls == 2 && g_last_arg == ms_base);

    // 4. Null, low, unmapped-page-boundary, wrong-vtable, and uninitialized pointers fail cleanly without faulting.
    assert(opus_class_member(NULL, "_ZTVN2nn5codec11OpusDecoderE") == 0);
    assert(opus_class_member((const void *)0x100, "_ZTVN2nn5codec11OpusDecoderE") == 0);
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    char *pages = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(pages != MAP_FAILED && mprotect(pages, ps, PROT_NONE) == 0);
    assert(opus_class_member(pages + ps, "_ZTVN2nn5codec11OpusDecoderE") == 0);
    assert(must_die(call_decode, pages + ps));
    assert(must_die(call_decode, ms_base));
    assert(must_die(call_ms_decode, base));
    assert(must_die(call_decode, c_obj));
    munmap(pages, 2 * ps);

    puts("t_codec_opus: all checks passed");
    return 0;
}
