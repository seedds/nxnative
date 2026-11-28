// Hardware Opus forwarding (hle_codec.c) without a guest:
// C-initialized decoders keep host state in objmap; mixed C++ and C decoders carry the software class vtable at hw - 8 and forward in place.
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

static struct { const void *k; void *v; } g_om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (g_om[i].k == k) return g_om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!g_om[i].k) { g_om[i].k = k; g_om[i].v = v; return; } abort(); }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (g_om[i].k == k) { void *v = g_om[i].v; g_om[i].k = g_om[i].v = NULL; return v; } return NULL; }
void hle_bind_system_event(void *e, int a) { (void)e; (void)a; }
void hle_signal_system_event(void *e) { (void)e; }

static void *g_vtbl[4], *g_ms_vtbl[4], *g_last;
static int g_inits, g_finis, g_decodes;
static int fake_init(void *s, int rate, int ch, void *buf, size_t size) {
    (void)rate; (void)ch; (void)buf; (void)size;
    g_last = s;
    g_inits++;
    return 0;
}
static int fake_ms_init(void *s, int rate, int ch, int streams, int stereo, const uint8_t *mapping, void *buf, size_t size) {
    (void)streams; (void)stereo; (void)mapping;
    return fake_init(s, rate, ch, buf, size);
}
static void fake_fini(void *s) { g_last = s; g_finis++; }
static int fake_decode(void *s, size_t *c, int *smp, int16_t *o, size_t osz, const void *i, size_t isz) {
    (void)o; (void)osz; (void)i; (void)isz; g_last = s; *c = 12; *smp = 960; g_decodes++; return 0;
}
void *guest_sym(const char *n) {
    static const struct { const char *sym; void *fn; } tab[] = {
        {OPUS_VTBL, g_vtbl}, {OPUS_MS_VTBL, g_ms_vtbl},
        {"_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm", fake_init},
        {"_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE", fake_fini},
        {"_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm", fake_decode},
        {"_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm", fake_ms_init},
        {"_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE", fake_fini},
        {"_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm", fake_decode},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(n, tab[i].sym)) return tab[i].fn;
    return NULL;
}

typedef struct { void *vptr; uint64_t body[7]; } fake_cpp;
static int must_die(int (*fn)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t), void *hw) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};
    g_died = 0; g_die_catch = 1;
    if (!setjmp(g_die_jmp)) (void)fn(hw, &c, &smp, out, sizeof out, in, sizeof in);
    g_die_catch = 0;
    return g_died;
}

int main(void) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};
    uint64_t c_obj[8] = {0}; uint8_t map[2] = {0, 1};

    assert(h_codec_HwOpusInit(c_obj, 48000, 2, in, sizeof in, 0) == 0 && g_inits == 1 && g_last != c_obj);
    assert(h_codec_HwOpusDecode(c_obj, &c, &smp, out, sizeof out, in, sizeof in) == 0 && g_decodes == 1 && g_last == objmap_get(c_obj));
    h_codec_HwOpusFini(c_obj); assert(g_finis == 1 && objmap_get(c_obj) == NULL);

    fake_cpp cpp = {.vptr = &g_vtbl[2]}, ms_cpp = {.vptr = &g_ms_vtbl[2]};
    assert(h_codec_HwOpusDecode(cpp.body, &c, &smp, out, sizeof out, in, sizeof in) == 0 && g_decodes == 2 && g_last == cpp.body);
    h_codec_HwOpusFini(cpp.body); assert(g_finis == 2 && g_last == cpp.body);

    assert(h_codec_HwOpusMsInit(c_obj, 48000, 6, 4, 2, map, in, sizeof in, 0) == 0 && g_inits == 2);
    assert(h_codec_HwOpusMsDecode(c_obj, &c, &smp, out, sizeof out, in, sizeof in) == 0 && g_decodes == 3 && g_last == objmap_get(c_obj));
    h_codec_HwOpusMsFini(c_obj); assert(g_finis == 3 && objmap_get(c_obj) == NULL);

    assert(h_codec_HwOpusMsDecode(ms_cpp.body, &c, &smp, out, sizeof out, in, sizeof in) == 0 && g_decodes == 4 && g_last == ms_cpp.body);
    h_codec_HwOpusMsFini(ms_cpp.body); assert(g_finis == 4 && g_last == ms_cpp.body);

    assert(!opus_class_member(NULL, OPUS_VTBL) && !opus_class_member((const void *)0x100, OPUS_VTBL));
    size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    char *pages = mmap(NULL, 2 * ps, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(pages != MAP_FAILED && mprotect(pages, ps, PROT_NONE) == 0);
    assert(must_die(h_codec_HwOpusDecode, pages + ps) && must_die(h_codec_HwOpusDecode, ms_cpp.body));
    assert(must_die(h_codec_HwOpusMsDecode, cpp.body) && must_die(h_codec_HwOpusDecode, c_obj));
    munmap(pages, 2 * ps);

    puts("t_codec_opus: all checks passed");
    return 0;
}
