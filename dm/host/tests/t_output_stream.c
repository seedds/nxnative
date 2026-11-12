// hle_audio.c unit test (silent sink): SDK 21's OutputStream on the host's AudioOut. A write of interleaved float frames becomes one 16-bit interleaved block,
// the free frame count is the capacity less the frames queued, played blocks come back (the release event fires) and the count recovers; a purge after a stop
// drops what was queued; the states are 0 not open, 1 stopped, 2 started. The default virtual device's properties name it, and the overload without flags opens too.
#include "../hle_audio.c"
#include <time.h>
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }
static _Atomic int g_sig;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add_explicit(&g_sig, 1, memory_order_seq_cst); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); puts(""); return 1; } while (0)
int main(void) {
    enum { CAP = 4800, FRAMES = 960 };   // capacity 100 ms, writes of 20 ms
    char stream[0x350] = {0}, ev[64], name[0x100];
    h_os_DefaultDev(name);
    if (!name[0]) FAIL("no default virtual device name");
    if (h_os_State(stream) != OS_NONE) FAIL("state of a stream never opened");
    int32_t fmt[3] = {OS_FLOAT, 2, 48000};
    if (h_os_Open(stream, ev, NULL, 0, name, fmt, CAP, CAP - FRAMES, 0)) FAIL("open");
    if (h_os_State(stream) != OS_STOPPED || h_os_Free(stream) != CAP) FAIL("after open: state %d, free %d", h_os_State(stream), h_os_Free(stream));
    // the properties of the host's one virtual output device (Nintendo Switch Sports Resort reads them before each open, main+0x5eda90): its name, the shortest stream
    // as a duration and the unidentified word; any other name is no device of the host's
    vdev_props p; memset(&p, 0xee, sizeof p);
    if (sizeof p != 0x110 || h_os_Props(&p, name) || strcmp(p.name, name) || p.min_ns != OS_MIN_NS || p.word_108 != 0)
        FAIL("properties: size %zu, name %.16s, shortest %lld ns, +0x108 %u", sizeof p, p.name, (long long)p.min_ns, p.word_108);
    if (!h_os_Props(&p, "AudioExternalOutput")) FAIL("properties of a device the host does not have");
    char nf[0x350] = {0};   // the overload without flags (Nintendo Switch Sports Resort, main+0x5edb34)
    if (h_os_OpenNoFlags(nf, ev, NULL, 0, name, fmt, CAP, CAP - FRAMES) || h_os_State(nf) != OS_STOPPED || h_os_Free(nf) != CAP) FAIL("open without flags");   // NOLINT(clang-analyzer-core.StackAddressEscape): the host keeps the stream only while it is open, and it is closed below before this returns
    h_os_Close(nf);
    if (h_os_State(nf) != OS_NONE) FAIL("state after closing the stream opened without flags");
    int32_t bad[3] = {OS_FLOAT, 2, 44100};
    char other[0x350] = {0};
    if (!h_os_Open(other, NULL, NULL, 0, name, bad, CAP, 0, 0) || h_os_State(other) != OS_NONE) FAIL("a 44.1 kHz stream opened");

    // interleaved float, as the game writes it (Xenoblade Chronicles 2's Switch 2 program; read as planar, all its sound came out an octave low): the block keeps the order, scaled to 16 bits and clamped
    static float pcm[2 * FRAMES];
    for (int i = 0; i < FRAMES; i++) { pcm[2 * i] = (float)i / FRAMES; pcm[2 * i + 1] = i == 1 ? 2.0f : -0.5f; }
    if (h_os_Write(stream, pcm, FRAMES) != FRAMES) FAIL("write");
    if (h_os_Free(stream) != CAP - FRAMES) FAIL("free after one write: %d", h_os_Free(stream));
    ostream *o = os_of(stream); aout *a = ao(o->key);
    oblock *k = (oblock *)a->q[0];
    if (k->b.size != FRAMES * 2 * 2 || k->pcm[0] != 0 || k->pcm[1] != -16383 || k->pcm[3] != 32767 || k->pcm[2 * 480] != (int16_t)(0.5f * 32767.0f))
        FAIL("block layout: size %llu, %d %d %d %d", (unsigned long long)k->b.size, k->pcm[0], k->pcm[1], k->pcm[3], k->pcm[960]);
    for (int i = 1; i < CAP / FRAMES; i++) if (h_os_Write(stream, pcm, FRAMES) != FRAMES) FAIL("write %d", i);
    if (h_os_Free(stream) != 0 || h_os_Write(stream, pcm, FRAMES) != 0) FAIL("a full stream took more frames");

    // started, the silent sink plays the 100 ms in real time: the frames come back and the event fired
    if (h_os_Start(stream) || h_os_State(stream) != OS_STARTED) FAIL("start");
    uint64_t t0 = now_ns();
    while (h_os_Free(stream) != CAP && now_ns() - t0 < 5000000000ull) usleep(2000);
    uint64_t ms = (now_ns() - t0) / 1000000;
    if (h_os_Free(stream) != CAP) FAIL("frames not played back in 5 s: free %d", h_os_Free(stream));
    if (ms < 60) FAIL("100 ms of frames played in %llu ms", (unsigned long long)ms);
    if (!atomic_load(&g_sig)) FAIL("no release event");

    // stopped with frames queued, a purge drops them
    for (int i = 0; i < 2; i++) if (h_os_Write(stream, pcm, FRAMES) != FRAMES) FAIL("write after playing");
    h_os_Stop(stream);
    if (h_os_State(stream) != OS_STOPPED) FAIL("stop");
    h_os_Purge(stream);
    if (h_os_Free(stream) != CAP) FAIL("purge left %d frames queued", CAP - h_os_Free(stream));
    h_os_Close(stream);
    if (h_os_State(stream) != OS_NONE) FAIL("state after close");
    printf("OutputStream: 100 ms played in %llu ms, %d release events\n", (unsigned long long)ms, atomic_load(&g_sig));
    return 0;
}
