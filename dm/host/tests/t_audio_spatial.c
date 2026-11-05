// hle_audio.c spatial audio (DM_AUDIO_SPATIAL, Mac): a six-channel output goes to macOS as 5.1 through the system renderer (audio_sys_mac.m) instead of an AudioUnit.
// Through the real renderer on the default output, fed silence (nothing audible): the output gets a renderer and no AudioUnit; once started the feeder pulls the queue in 240-frame blocks,
// whole blocks in order (the queue position is exactly the blocks pulled), at real time (about 200 blocks a second once the clock runs), with no underrun while the queue has data and no feed
// late against the device's delay; while the clock is held (as the system does at some starts and after it flushed the renderer) it still pulls at real time, dropping the blocks, and
// stops pulling once the clock has been held over 2 s (a dead renderer, for the watchdog); after the system's "flushed automatically" notification it restarts the clock and
// pulls at real time again, whole blocks, without a late feed; after Stop it pulls nothing more, and it starts again; a stereo output gets an AudioUnit and no renderer
#include "../hle_audio.c"
#include <time.h>
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
void *objmap_get(const void *k) { (void)k; return NULL; }
void objmap_put(const void *k, void *v) { (void)k; (void)v; }
void *objmap_del(const void *k) { (void)k; return NULL; }
void hle_signal_system_event(void *g) { (void)g; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }

enum { SECONDS = 10, BLOCK_BYTES = 240 * 6 * 2 };
static int16_t g_silence[48000 * 6 * SECONDS];   // more than the test plays: the queue never runs dry

static uint64_t blocks(aout *a) { return atomic_load_explicit(&a->cb_n, memory_order_relaxed); }

int main(void) {
    setenv("DM_AUDIO_SPATIAL", "1", 1);
    gbuf b = {g_silence, sizeof g_silence, sizeof g_silence};
    aout a = {.ch = 6, .id = 1};
    atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
    a.q[0] = &b; atomic_init(&a.q_tail, 1);
    if (au_create(&a) || !a.sys || a.au) { printf("FAIL spatial: a six-channel output got %s\n", a.sys ? "an AudioUnit too" : "no system renderer"); return 1; }
    if (dev_start(&a)) { puts("FAIL spatial: the system renderer did not start"); return 1; }
    usleep(1000000);   // the synchronizer's clock takes a moment to run
    uint64_t n0 = blocks(&a), t0 = now_ns();
    usleep(1000000);
    uint64_t n1 = blocks(&a), t1 = now_ns();
    double rate = (n1 - n0) / ((t1 - t0) / 1e9);
    printf("spatial: %llu blocks pulled in the first second, %.1f blocks/s after (200 = real time), %llu late feeds\n", (unsigned long long)n0, rate,
           (unsigned long long)dm_sys_audio_late(a.sys));
    if (!n0 || rate < 190 || rate > 210) { puts("FAIL spatial: the feeder does not pull at real time"); return 1; }
    dm_sys_audio_post_flush(a.sys);
    usleep(1500000);   // the restart: the first block, then the clock (held a while at a start)
    uint64_t f0 = blocks(&a), ft0 = now_ns();
    usleep(1000000);
    uint64_t f1 = blocks(&a), ft1 = now_ns();
    double flush_rate = (f1 - f0) / ((ft1 - ft0) / 1e9);
    printf("spatial: after the system's flush: %.1f blocks/s, %llu late feeds\n", flush_rate, (unsigned long long)dm_sys_audio_late(a.sys));
    if (flush_rate < 190 || flush_rate > 210) { puts("FAIL spatial: no real-time pace after the system flushed the renderer"); return 1; }
    if (dm_sys_audio_late(a.sys)) { puts("FAIL spatial: a late feed after the system flushed the renderer"); return 1; }
    dm_sys_audio_hold_clock(a.sys);
    usleep(100000);
    uint64_t h0 = blocks(&a), ht0 = now_ns();
    usleep(1000000);
    uint64_t h1 = blocks(&a), ht1 = now_ns();
    double held_rate = (h1 - h0) / ((ht1 - ht0) / 1e9);
    usleep(1200000);   // held over 2 s now
    uint64_t h2 = blocks(&a);
    usleep(300000);
    uint64_t h3 = blocks(&a);
    printf("spatial: clock held: %.1f blocks/s pulled and dropped, %llu pulled after 2.3 s held\n", held_rate, (unsigned long long)(h3 - h2));
    if (held_rate < 190 || held_rate > 210) { puts("FAIL spatial: the feeder does not keep the real-time pace while the clock is held"); return 1; }
    if (h3 != h2) { puts("FAIL spatial: the feeder still pulls with the clock held over 2 s (the watchdog would never see a dead renderer)"); return 1; }
    dev_stop(&a);
    uint64_t stopped = blocks(&a);
    if (a.pos != stopped * BLOCK_BYTES || atomic_load(&a.q_head) != 0 || atomic_load(&a.under_n)) {
        printf("FAIL spatial: %llu blocks pulled but the queue is at byte %llu (head %u, underruns %llu)\n", (unsigned long long)stopped, (unsigned long long)a.pos,
               atomic_load(&a.q_head), (unsigned long long)atomic_load(&a.under_n));
        return 1;
    }
    if (dm_sys_audio_late(a.sys)) { puts("FAIL spatial: the feed fell behind the device's delay"); return 1; }
    usleep(200000);
    if (blocks(&a) != stopped) { puts("FAIL spatial: blocks were pulled after Stop"); return 1; }
    if (dev_start(&a)) { puts("FAIL spatial: the system renderer did not start again"); return 1; }
    usleep(1500000);
    if (blocks(&a) == stopped) { puts("FAIL spatial: nothing pulled after starting again"); return 1; }
    dev_stop(&a);
    dev_dispose(&a);
    aout st = {.ch = 2, .id = 2};
    atomic_init(&st.state, STATE_STOPPED); atomic_init(&st.vol, 0x3f800000u);
    if (au_create(&st) || st.sys || !st.au) { puts("FAIL spatial: a stereo output did not get a plain AudioUnit"); return 1; }
    dev_dispose(&st);
    puts("spatial audio: six channels to the system renderer, pulled whole blocks in order at real time with no underrun or late feed, restarted after the system's flush, real-time pace kept while the clock is held up to 2 s, idle after Stop, restarts; stereo outputs untouched");
    return 0;
}
