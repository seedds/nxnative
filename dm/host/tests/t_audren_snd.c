// Sound categories (hle_audren.c): a voice's category comes from the BWAV its wave buffer lies in (the header's CRC-32, looked up in the table) or from its ADPCM
// coefficients, and its volume is the game's times the category's gain.
// - the table is refused when it is cut off or names a category that does not exist;
// - a buffer inside a channel of a BWAV is found from any address in the channel, a buffer before the header or past the samples is in no BWAV;
// - a span is checked against its header again: another archive loaded at the same address gives the new sound's category;
// - nothing is read outside the memory pool the buffer lies in;
// - with the game's hooks (Mario Kart World): a new voice takes its coefficients' category, SetVoiceVolume hands on volume x gain, a changed gain reaches a playing voice.
#include "../hle.h"
#include <assert.h>
#include "../hle_audren.c"

static float set_vol[8]; static void *set_voice[8]; static int sets;
static void f_set(void *v, float x) { set_voice[sets & 7] = v; set_vol[sets & 7] = x; sets++; }
static unsigned f_true4(void *a, void *b, void *c, size_t d) { (void)a; (void)b; (void)c; (void)d; return 0xffffff01u; }   // only the low byte is the bool
static unsigned f_acq(void *a, void *b, int c, int d, int e, int f, const void *g, size_t h, const void *i) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; return 1; }
static unsigned f_append(void *v, const void *wb) { (void)v; (void)wb; return 1; }
static unsigned f_valid(const void *v) { (void)v; return 1; }
static void f_void2(void *a, void *b) { (void)a; (void)b; }
void *guest_sym(const char *name) {
    if (strstr(name, "SetVoiceVolume")) return (void *)f_set;
    if (strstr(name, "AcquireMemoryPool")) return (void *)f_true4;
    if (strstr(name, "AcquireVoiceSlot")) return (void *)f_acq;
    if (strstr(name, "AppendWaveBuffer")) return (void *)f_append;
    if (strstr(name, "IsVoiceValid")) return (void *)f_valid;
    if (strstr(name, "ReleaseVoiceSlot") || strstr(name, "ReleaseMemoryPool")) return (void *)f_void2;
    return NULL;
}
void *objmap_get(const void *key) { (void)key; return NULL; }
void objmap_put(const void *key, void *value) { (void)key; (void)value; }
void *objmap_del(const void *key) { (void)key; return NULL; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack) { (void)fn; (void)arg; (void)prio; (void)stack; return NULL; }
void dm_guest_thread_join(void *tt) { (void)tt; }
uint64_t now_ns(void) { return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void *dm_pcmout_open(int ch) { (void)ch; return NULL; }
void dm_pcmout_close(void *k) { (void)k; }
void dm_pcmout_start(void *k) { (void)k; }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; return 0; }
void *dm_pcmout_released(void *k) { (void)k; return NULL; }
void dm_pcmout_wait(void *k, int ms) { (void)k; (void)ms; }
void hle_bind_system_event(void *ev, int ac) { (void)ev; (void)ac; }
uint32_t hle_dup_readable_handle(uint32_t h) { return h; }
uint64_t hle_system_event_binding(void *ev) { return (uint64_t)(uintptr_t)ev; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 0; }
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
int dm_audio_route(void) { return 2; }

static void w16(uint8_t *p, unsigned v) { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); }
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
// A BWAV at h: nch ADPCM channels of ns samples, the first channel's samples at 0x40-aligned off, the next right after
static size_t bwav(uint8_t *h, uint32_t crc, unsigned nch, uint32_t ns, const uint8_t *coef) {
    size_t off = (0x10 + 0x4c * nch + 0x3f) & ~(size_t)0x3f, bytes = (ns + 13) / 14 * 8;
    memcpy(h, "BWAV", 4); w16(h + 4, 0xfeff); w16(h + 6, 1); w32(h + 8, crc); w16(h + 0xc, 0); w16(h + 0xe, nch);
    for (unsigned c = 0; c < nch; c++) {
        uint8_t *ch = h + 0x10 + 0x4c * c; memset(ch, 0, 0x4c);
        w16(ch, 1); w32(ch + 4, 48000); w32(ch + 8, ns); w32(ch + 0xc, ns); if (coef) memcpy(ch + 0x10, coef, 32);
        w32(ch + 0x30, (uint32_t)(off + bytes * c)); w32(ch + 0x34, (uint32_t)(off + bytes * c));
    }
    return off + bytes * nch;
}
static uint8_t tab[20 + 8 + 4 + 4 + 4];
static void table(uint32_t crc_a, int cat_a, uint32_t crc_b, int cat_b, uint32_t coef, int cat_c) {
    memcpy(tab, "DMSNDCAT", 8); w32(tab + 8, 1); w32(tab + 12, 2); w32(tab + 16, 1);
    w32(tab + 20, crc_a); w32(tab + 24, crc_b); memset(tab + 28, 0, 4); tab[28] = (uint8_t)cat_a; tab[29] = (uint8_t)cat_b;
    w32(tab + 32, coef); memset(tab + 36, 0, 4); tab[36] = (uint8_t)cat_c;
}
int main(void) {
    static uint8_t mem[0x8000]; uint8_t *pool = mem + 0x1000; size_t pool_n = 0x4000; uint32_t crc = 0;
    uint8_t coef[32]; for (int i = 0; i < 32; i++) coef[i] = (uint8_t)(i * 7 + 1);
    // the table
    table(0x11111111u, DM_SND_MUSIC, 0x22222222u, DM_SND_ENGINE, snd_crc32(coef, 32), DM_SND_VOICE);
    assert(snd_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    assert(!snd_tab_parse(tab, sizeof tab - 1));                     // cut off
    tab[29] = DM_SND_CATS; assert(!snd_tab_parse(tab, sizeof tab));  // a category that does not exist
    tab[29] = DM_SND_ENGINE; assert(snd_tab_parse(tab, sizeof tab)); g_snd_tab.state = 1;
    assert(snd_lookup(g_snd_tab.crc, g_snd_tab.crc_cat, g_snd_tab.ncrc, 0x22222222u) == DM_SND_ENGINE && snd_lookup(g_snd_tab.crc, g_snd_tab.crc_cat, g_snd_tab.ncrc, 0x33333333u) == -1);
    // one pool with two BWAVs: a mono one (music) and a stereo one (engine)
    g_snd_pool[0].pool = (void *)1; g_snd_pool[0].lo = pool; g_snd_pool[0].hi = pool + pool_n; g_snd_npool = 1;
    uint8_t *a = pool + 0x40, *b = pool + 0x1000;
    size_t an = bwav(a, 0x11111111u, 1, 1400, NULL), bn = bwav(b, 0x22222222u, 2, 2800, NULL);
    assert(an == 0x80 + 800 && bn == 0xc0 + 2 * 1600);
    assert(snd_find(a + 0x80, &crc) == DM_SND_MUSIC && crc == 0x11111111u);        // the samples' start
    assert(snd_find(a + 0x80 + 799, &crc) == DM_SND_MUSIC);                          // their last byte (through the span)
    assert(snd_find(a + 0x80 + 800, &crc) == -1);                                    // past them
    assert(snd_find(a + 0x10, &crc) == -1 && snd_find(pool, &crc) == -1);            // the header itself, and before any header
    assert(snd_find(b + 0xc0 + 1600 + 4, &crc) == -1);                               // the second channel is 0x700 past the header: not found from there ...
    assert(snd_find(b + 0xc0, &crc) == DM_SND_ENGINE && crc == 0x22222222u);        // ... until the first channel was played, which adds both spans
    assert(snd_find(b + 0xc0 + 1600 + 4, &crc) == DM_SND_ENGINE);
    assert(snd_find(mem + 0x800, &crc) == -1 && snd_find(pool + pool_n + 0x80, &crc) == -1);   // outside every pool
    // another archive at the same address: the span's header no longer matches
    bwav(a, 0x22222222u, 1, 1400, NULL);
    assert(snd_find(a + 0x80 + 8, &crc) == DM_SND_ENGINE && crc == 0x22222222u);
    // a header whose samples would run past the pool is no span
    g_snd_nspan = 0; uint8_t *e = pool + pool_n - 0x100; bwav(e, 0x11111111u, 1, 1400, NULL);
    assert(snd_find(e + 0x80, &crc) == -1);
    // a sound the table does not know plays as "other"
    g_snd_nspan = 0; bwav(a, 0x44444444u, 1, 1400, NULL); assert(snd_find(a + 0x80, &crc) == DM_SND_OTHER && crc == 0x44444444u);
#ifdef DM_SND_HOOKS
    // the game's calls: a pool, a voice whose coefficients name "voice", its volume, then a buffer in the engine BWAV
    g_snd_npool = 0; g_snd_nspan = 0; bwav(a, 0x11111111u, 1, 1400, NULL);
    assert(h_snd_PoolAcquire(NULL, (void *)1, pool, pool_n) & 0xff); assert(g_snd_npool == 1);
    dm_snd_gain_set(DM_SND_VOICE, 0.5f); dm_snd_gain_set(DM_SND_ENGINE, 0.25f); dm_snd_gain_set(DM_SND_MUSIC, 0.0f);
    static uint8_t v1[0x40], v2[0x40]; sets = 0;
    assert(h_snd_VoiceAcquire(NULL, v1, 48000, 1, 6, 0, coef, 32, NULL) & 0xff);
    assert(sets == 1 && set_voice[0] == v1 && set_vol[0] == 0.5f);                  // volume 1 x the voice category's gain
    h_snd_VoiceVolume(v1, 0.8f); assert(sets == 2 && set_vol[1] == 0.8f * 0.5f);
    struct { void *p; size_t n; } wb = {b + 0xc0, 1600};
    assert(h_snd_Append(v1, &wb) & 0xff); assert(sets == 3 && set_vol[2] == 0.8f * 0.25f);   // the BWAV says engine: the game's 0.8 x 0.25
    assert(h_snd_Append(v1, &wb) & 0xff); assert(sets == 3);                        // the same category again: nothing to set
    // a 16-bit PCM voice with no BWAV is a decoded stream: music
    assert(h_snd_VoiceAcquire(NULL, v2, 48000, 1, 2, 0, NULL, 0, NULL) & 0xff); assert(sets == 3);
    static uint8_t loose[0x100]; struct { void *p; size_t n; } wb2 = {loose, 0x100};
    assert(h_snd_Append(v2, &wb2) & 0xff); assert(sets == 4 && set_voice[3] == v2 && set_vol[3] == 0.0f);
    // a changed gain reaches the playing voices at the next update; a released voice is left alone
    h_snd_VoiceRelease(NULL, v2); g_snd_done = atomic_load(&g_snd_gen); sets = 0;
    snd_apply(); assert(sets == 0);
    dm_snd_gain_set(DM_SND_ENGINE, 1.0f); snd_apply(); assert(sets == 1 && set_voice[0] == v1 && set_vol[0] == 0.8f);
    h_snd_VoiceVolume(v2, 0.3f); assert(sets == 2 && set_vol[1] == 0.3f);            // not the host's voice any more: the game's value as is
    // releasing the pool forgets its sounds
    h_snd_PoolRelease(NULL, (void *)1); assert(g_snd_npool == 0 && g_snd_nspan == 0);
    puts("t_audren_snd: ok (table, BWAV spans, voice volumes)");
#else
    puts("t_audren_snd: ok (table, BWAV spans; the voice hooks are Mario Kart World's)");
#endif
    return 0;
}
