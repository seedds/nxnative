// SDK 1's host renderer (issue #960, Rayman Legends: Definitive Edition): an sdk without ExecuteAudioRendererRendering gets the service's work buffer size for a revision at +0x30,
// RequestUpdateAudioRenderer answers the memory pools, effects and wave buffers in the game's AudioRendererConfig, and a frame mixes a PCM16 voice to stereo with its mix volumes,
// advancing its played samples and consuming its wave buffer at the end
#include "../hle.h"
#include <assert.h>
#include "../hle_audren.c"

void *guest_sym(const char *name) { (void)name; return NULL; }   // SDK 1: no ExecuteAudioRendererRendering, no nn::os::LockMutex wanted
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
static int binds, signals;
void hle_bind_system_event(void *ev, int ac) { (void)ev; (void)ac; binds++; }
void hle_signal_system_event(void *ev) { (void)ev; signals++; }
uint32_t hle_dup_readable_handle(uint32_t h) { return h; }

enum { WAVE = 480 };
_Alignas(16) static uint8_t voice[0x210], pool[2][0x18], effect[0xe0], res0[0x80];
static struct { int32_t count, pad; uint8_t *voices; uint8_t rest[0x48]; } vm;   // VoiceInfoManager: +0 count, +8 voices, +0x50 its mutex
static struct { int32_t count, pad; uint8_t *effects; } em;
static struct { uint8_t pad[0x20]; int32_t count, pad2; uint8_t *pools; } mpm;
static struct { void *vm, *pad, *em, *pad2[2], *mpm; } cfg;   // +0 voices, +0x10 effects, +0x28 memory pools
static int16_t wave[WAVE];

static void test_work_size(void) {
    uint8_t p[P_SIZE] = {0};
    int32_t fields[] = {48000, 240, 8, 0, 64, 1, 2, 0};   // rate, samples per frame, mix buffers, submixes, voices, sinks, effects, performance frames
    memcpy(p, fields, sizeof fields);
    *(uint32_t *)(p + 0x30) = 0x31564552;   // 'REV1' where SDK 1 keeps it
    size_t n = h_ar_WorkSize(p);
    assert(n == ar_service_work_size(p) && n && !(n & (WORK_PAGE - 1)));
    memset(p + 0x30, 0, 4);
    assert(h_ar_WorkSize(p) == 0x40000);   // no revision found: the fallback size
}

static void test_update_and_mix(void) {
    void *handle = NULL; uint8_t param[P_SIZE] = {0}, ev;
    *(int32_t *)param = 48000;
    assert(ar_open(&handle, &ev, param, NULL, 0, NULL) == RES_OK && handle == &g_ar[0] && binds == 1);
    assert(h_ar_Start(handle, NULL) == RES_OK);
    vm.count = 1; vm.voices = voice; em.count = 1; em.effects = effect; mpm.count = 2; mpm.pools = pool[0];
    cfg.vm = &vm; cfg.em = &em; cfg.mpm = &mpm;
    *(uint32_t *)(pool[0] + 0x10) = 2; *(uint32_t *)(pool[1] + 0x10) = 4;   // attach and detach requested
    effect[0] = 3; effect[2] = 1; effect[1] = 1;                            // an enabled Aux effect, new
    for (int i = 0; i < WAVE; i++) wave[i] = 1000;
    voice[0x48] = 1; voice[0x49] = 1; voice[0x4a] = 0; voice[0x4b] = 2;     // new, in use, playing, PCM16
    *(int32_t *)(voice + 0x4c) = 48000; *(int32_t *)(voice + 0x58) = 1; *(float *)(voice + 0x5c) = 1.0f; *(float *)(voice + 0x60) = 1.0f;
    *(int16_t **)(voice + 0xa0) = wave; *(uint64_t *)(voice + 0xa8) = sizeof wave; *(int32_t *)(voice + 0xb0) = 0; *(int32_t *)(voice + 0xb4) = WAVE;
    *(uint32_t *)(voice + 0x1c8) = 1;                                       // one wave buffer appended
    *(uint8_t **)(voice + 0x1d0) = res0; ((float *)(res0 + 0x14))[0] = 1.0f; ((float *)(res0 + 0x14))[1] = 0.5f;   // mono to mix buffers 0 (left) and 1 (right)
    assert(h_ar_RequestUpdate(handle, (const uint8_t *)&cfg, NULL) == RES_OK);
    assert(*(uint32_t *)(pool[0] + 0x10) == 3 && *(uint32_t *)(pool[1] + 0x10) == 5);
    assert(effect[1] == 0 && effect[0xc0] == 3);
    assert(voice[0xba] == 1 && voice[0x198] == 0);

    int16_t out[2 * 240]; int n = 0;
    sdk1_render_frame(&g_ar[0], out, &n);
    assert(n == 240);
    for (int s = 0; s < 240; s++) assert(out[2 * s] == 1000 && out[2 * s + 1] == 500);
    assert(*(int64_t *)(voice + 0x1b0) == 240 && *(uint32_t *)(voice + 0x1b8) == 0);
    sdk1_render_frame(&g_ar[0], out, &n);
    assert(out[2 * 239] == 1000 && *(uint32_t *)(voice + 0x1b8) == 1);   // the wave buffer played to its end and is consumed
    sdk1_render_frame(&g_ar[0], out, &n);
    for (int s = 0; s < 2 * 240; s++) assert(out[s] == 0);                 // nothing more appended: silence
    assert(atomic_load(&g_ar[0].frames) == 3 && atomic_load(&g_ar[0].nonsilent) == 2);

    h_ar_Stop(handle, NULL);
    h_ar_Close(handle, NULL);
    assert(!g_ar[0].handle[0] && !g_sdk1_cfg && !g_sdk1_ev);
}

int main(void) {
    assert(!ar_has_local_cpu());
    test_work_size();
    test_update_and_mix();
    puts("audren SDK 1: service work size from the revision at +0x30, pool / effect / wave buffer answers, PCM16 voice mixed to stereo and consumed passed");
    return 0;
}
