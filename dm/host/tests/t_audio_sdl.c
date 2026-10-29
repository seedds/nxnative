// SDL3 audio output of hle_audio.c (Linux and Windows) on SDL's dummy audio driver, whose device thread consumes in real time like a sound card (one callback per device buffer, about
// 5 ms here): an output opened through OpenAudioOut gets an SDL stream in the guest's format, its device calls back while started, the game's buffers come back in order with their
// release events and dm_audio_render_calls() advances; Stop and the background suspend stop the callbacks (the silent sink takes over while suspended), Close frees it, and a second
// open / play / close cycle leaves no thread behind (Linux, where /proc counts them). Also a six-channel output (stereo to the device) and DM_AUDIO=0 (the silent sink, no device)
#include "../hle_audio.c"
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
#include <time.h>
#if defined(__linux__)
#include <dirent.h>
#endif

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }
static _Atomic int g_events;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add(&g_events, 1); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }   // call sites in the log: no guest modules in the unit test
void dm_host_helper_qos(plat_qos_t q) { (void)q; }

static int threads(void) {   // this process's threads; -1 where they cannot be counted here
#if defined(__linux__)
    DIR *d = opendir("/proc/self/task"); if (!d) return -1;
    int n = 0; struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d); return n;
#else
    return -1;
#endif
}
static void sleep_ms(int ms) { plat_sleep_ns((uint64_t)ms * 1000000); }

enum { NBUF = 40, FRAMES = 240 };   // 40 blocks of 5 ms: 200 ms of sound
static int16_t pcm[NBUF][FRAMES * 6];
// Open an output of ch channels, play NBUF blocks, stop and close it; real: an SDL device is expected (else the silent sink)
static void cycle(int ch, int real, const char *what) {
    uint8_t out[64] = {0}, ev[64], prm[16]; gbuf bufs[NBUF];
    h_au_InitParam(prm); *(uint16_t *)(prm + 4) = (uint16_t)ch;
    CHECK(h_au_Open(out, ev, NULL, prm) == RES_OK, what);
    aout *a = ao(out);
    if (real) {
        CHECK(a->au != NULL, "the output has an SDL stream");
        SDL_AudioSpec src = {0};
        CHECK(a->au && SDL_GetAudioStreamFormat(a->au, &src, NULL) && src.format == SDL_AUDIO_S16 && src.freq == 48000 && src.channels == (ch == 6 ? 2 : ch),
              "the stream takes S16 at 48 kHz in the device's channels (six are downmixed to two first)");
    } else CHECK(a->au == NULL, "DM_AUDIO=0: no device, the silent sink");
    for (int i = 0; i < NBUF; i++) {
        for (int j = 0; j < FRAMES * ch; j++) pcm[i][j] = (int16_t)(i * 100 + j);
        bufs[i] = (gbuf){pcm[i], (uint64_t)FRAMES * ch * 2, (uint64_t)FRAMES * ch * 2};
        h_au_Append(out, &bufs[i]);
    }
    uint64_t calls = dm_audio_render_calls(); int ev0 = g_events;
    CHECK(h_au_Start(out) == RES_OK && a->backend == (real ? BK_AU : BK_NULL), real ? "Start resumes the SDL device" : "Start runs the silent sink");
    int got = 0, order = 1;
    for (int t = 0; t < 3000 && got < NBUF; ) {
        gbuf *b = h_au_Released(out);
        if (b) { order &= b == &bufs[got]; got++; continue; }
        sleep_ms(2); t += 2;
    }
    CHECK(got == NBUF && order, "every buffer comes back, in order, at the device's pace");
    CHECK(g_events > ev0, "and each release signals the game's event");
    if (real) {
        CHECK(dm_audio_render_calls() > calls && a->cb_n > 0, "the device called back: dm_audio_render_calls advanced");
        CHECK(!a->stalled && a->backend == BK_AU, "the watchdog never took over");
        dm_audio_suspend(1);
        CHECK(a->backend == BK_NULL, "suspended (background): the silent sink takes over");
        uint64_t n = a->cb_n; sleep_ms(60);
        CHECK(a->cb_n == n, "and the device calls back no more");
        dm_audio_suspend(0);
        sleep_ms(60);
        CHECK(a->backend == BK_AU && a->cb_n > n, "resumed: the device plays again");
    }
    h_au_Stop(out);
    uint64_t n = a->cb_n; sleep_ms(60);
    CHECK(a->cb_n == n && a->backend == BK_NONE, "after Stop nothing calls back");
    h_au_Close(out);
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");   // before hle_audio.c starts SDL's audio
    setenv("DM_AUDIO", "1", 1); unsetenv("DM_DBG_AU_FRAMES"); unsetenv("DM_AUDIO_MUTE");
    int t0 = threads();
    cycle(2, 1, "a stereo output opens on the default device");
    CHECK(g_sdl_audio && !strcmp(SDL_GetCurrentAudioDriver(), "dummy"), "SDL's audio runs on the dummy driver");
    { SDL_AudioSpec dev; int fr = 0; SDL_AudioDeviceID id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
      CHECK(id && SDL_GetAudioDeviceFormat(id, &dev, &fr) && fr == RATE / 200, "the device buffer is 5 ms of frames, not SDL's 1024-frame default");
      if (id) SDL_CloseAudioDevice(id); }
    int t1 = threads();
    cycle(6, 1, "a six-channel output opens");
    cycle(2, 1, "and a stereo one again");
    // A thread that is exiting still has its /proc/self/task entry for a moment after the join (or after SDL's device thread was told to stop): the count is read until it settles,
    // up to a second. A thread that is still there after that is a leak (CI run 8 on f8ac96d read 3 right after Close and failed on one exiting thread)
    int t2 = threads();
    for (int ms = 0; t2 != t1 && ms < 1000; ms += 10) { sleep_ms(10); t2 = threads(); }
    if (t0 >= 0) CHECK(t2 == t1, "a second and third open / play / close cycle leave no thread behind");
    else printf("note: thread counting is Linux-only; the leak check did not run here\n");
    printf("threads: %d at start, %d after the first output (its watchdog and SDL's own), %d after two more\n", t0, t1, t2);
    setenv("DM_AUDIO", "0", 1);
    cycle(2, 0, "DM_AUDIO=0 opens an output too");
    printf("== audio_sdl: %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
