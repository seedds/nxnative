// Exercise the real callback while its diagnostic writer is blocked, without a sound device.
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <dispatch/dispatch.h>
#include <string.h>
static _Thread_local int in_callback;
static _Atomic int callback_io, block_report, reports, line_slots;
static dispatch_semaphore_t report_entered, report_allow, report_done, released_event;
static char lines[8][512];
static void checked_flockfile(FILE *f) { (void)f; if (in_callback) atomic_fetch_add(&callback_io, 1); }
static void checked_funlockfile(FILE *f) { (void)f; }
static int checked_fputc(int c, FILE *f) { (void)f; return c; }
static int checked_fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; if (in_callback) atomic_fetch_add(&callback_io, 1); return 0; }
// A log line (dm.h LOG is one call, dm_log): hle_audio.c's lines come here
__attribute__((format(printf, 1, 2))) static void checked_log(const char *fmt, ...) {
    if (in_callback) { atomic_fetch_add(&callback_io, 1); return; }
    if (!strstr(fmt, "device callbacks")) return;
    if (atomic_exchange(&block_report, 0)) {
        dispatch_semaphore_signal(report_entered);
        dispatch_semaphore_wait(report_allow, DISPATCH_TIME_FOREVER);
    }
    int n = atomic_fetch_add(&line_slots, 1);
    va_list ap; va_start(ap, fmt);
    if (n < 8) vsnprintf(lines[n], sizeof lines[n], fmt, ap);
    va_end(ap);
    atomic_fetch_add(&reports, 1); dispatch_semaphore_signal(report_done);
}
#define flockfile checked_flockfile
#define funlockfile checked_funlockfile
#define fputc checked_fputc
#define fprintf checked_fprintf
#define DM_LOG_IMPL   // dm.h declares dm_log, which is checked_log here, and leaves its definition out
#define dm_log checked_log
#include "../hle_audio.c"
#undef dm_log
#undef fprintf
#undef fputc
#undef funlockfile
#undef flockfile
__thread _Atomic int dm_host_work;
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; } // No guest address space: the real big-stack entry keeps the calling host stack.
uint64_t now_ns(void) { return 0; }
static void *key, *value;
void *objmap_get(const void *k) { return k == key ? value : NULL; }
void objmap_put(const void *k, void *v) { key = (void *)k; value = v; }
void *objmap_del(const void *k) { void *v = objmap_get(k); key = value = NULL; return v; }
static _Atomic int signals;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add(&signals, 1); dispatch_semaphore_signal(released_event); }
void hle_bind_system_event(void *g, int c) { (void)g; (void)c; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
static int checks, failures;
#define CHECK(v, why) do { checks++; if (!(v)) { failures++; printf("FAIL: %s\n", why); } } while (0)
static int wait_for(dispatch_semaphore_t s) { return !dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, 2000000000)); }
static int render(void *out, int count, int with_pcm) {
    aout *a = ao(out);
    for (int i = 0; i < count; i++) {
        int frames = 1 + (i & 1); int16_t input[] = {123, -456, 789, -1234}, played[4] = {0};
        aout_buf b = {.ptr = input, .cap = sizeof input, .size = (uint64_t)frames * 4};
        if (with_pcm && !h_au_Append(out, &b)) return 0;
        AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, frames * 4, played}}};
        in_callback = 1; au_render(a, NULL, NULL, 0, frames, &bl); in_callback = 0;
        if (with_pcm) {
            int woke = !a->event || wait_for(released_event);
            int same = !memcmp(input, played, (size_t)frames * 4);
            aout_buf *returned = woke && same ? h_au_Released(out) : NULL;
            if (!woke || !same || returned != &b) {
                printf("release handoff %d/%d: event=%d PCM=%d buffer=%d pending=%d armed=%d\n",
                       i + 1, count, woke, same, returned == &b, atomic_load(&a->notify_pending), atomic_load(&a->notify_armed));
                return 0;
            }
        }
    }
    return 1;
}
static void *drain_stats(void *unused) { (void)unused; stats_drain(); return NULL; }
static void render_frames(aout *a, unsigned frames, int16_t *pcm) {
    unsigned channels = (unsigned)a->dev_ch;
    AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{channels, frames * channels * sizeof *pcm, pcm}}};
    in_callback = 1;
    au_render(a, NULL, NULL, 0, frames, &bl);
    in_callback = 0;
}
// A two-block, 256-frame queue reproduces the timing seen in Need for Speed.
// A 44.1 kHz route requesting 256 device frames pulls 278 or 279 at 48 kHz.
static int16_t replay_sample(uint64_t frame) { return (int16_t)(1 + frame % 30000); }
static void replay_block(aout_buf *buffer, uint64_t start) {
    int16_t *pcm = buffer->ptr;
    for (int i = 0; i < 256; i++) {
        pcm[i * 6] = replay_sample(start + i);
        pcm[i * 6 + 1] = -pcm[i * 6];
    }
}
static void replay_device_callbacks(int bluetooth) {
    uint32_t out[16] = {0};
    uint8_t prm[8] = {0};
    prm[4] = 6;
    CHECK(!h_au_OpenDef0(out, prm), "open six-channel guest audio");
    aout *a = ao(out);
    a->dev_ch = 2;  // The mocked stereo route is normally set by au_create.
    CHECK(a->ch == 6, "use the same guest format as the game");
    atomic_store(&a->state, STATE_STARTED);
    int16_t pcm[2][256 * 6] = {{0}};
    aout_buf buffers[2] = {
        {.ptr = pcm[0], .cap = sizeof pcm[0], .size = sizeof pcm[0]},
        {.ptr = pcm[1], .cap = sizeof pcm[1], .size = sizeof pcm[1]},
    };
    for (int i = 0; i < 2; i++) {
        replay_block(&buffers[i], i * 256);
        CHECK(h_au_Append(out, &buffers[i]), "queue both game buffers");
    }
    uint64_t submitted = 512, consumed = 0, mismatches = 0, count_mismatches = 0;
    for (unsigned i = 0; i < 8000; i++) {
        unsigned frames = bluetooth ? ((i + 1) * 40960 / 147 - i * 40960 / 147) : 240;
        int16_t played[279 * 2];
        render_frames(a, frames, played);
        atomic_store(&a->st_pending, 0);
        for (unsigned j = 0; j < frames; j++) {
            int16_t expected = replay_sample(consumed + j);
            mismatches += played[2 * j] != expected || played[2 * j + 1] != -expected;
        }
        consumed += frames;
        count_mismatches += h_au_BufCount(out) != (submitted / 256) - (consumed / 256);
        aout_buf *released;
        while ((released = h_au_Released(out))) {
            replay_block(released, submitted);
            submitted += 256;
            if (!h_au_Append(out, released)) { CHECK(0, "requeue a returned game buffer"); break; }
        }
    }
    uint64_t underruns = atomic_load(&a->under_n);
    printf("%s: 8,000 callbacks, %llu underruns\n", bluetooth ? "Bluetooth" : "Speaker", (unsigned long long)underruns);
    CHECK(!underruns, "callback requests fit without zero padding");
    CHECK(!mismatches, "all samples are played exactly once and in order");
    CHECK(!count_mismatches, "buffer count includes every unplayed block after each callback");
    if (bluetooth) {
        h_au_Stop(out);
        CHECK(!h_au_BufCount(out), "stop clears the count of both staged and queued audio");
        CHECK(h_au_Released(out) && h_au_Released(out) && !h_au_Released(out), "stop returns the newly queued guest buffers");
        replay_block(&buffers[0], 42);
        CHECK(h_au_Append(out, &buffers[0]), "queue fresh audio before restarting");
        atomic_store(&a->state, STATE_STARTED);
        int16_t played[240 * 2];
        render_frames(a, 240, played);
        CHECK(played[0] == replay_sample(42), "restart begins with fresh PCM");
    }
    h_au_Close(out);
}
static void check_staging_limit(unsigned channels) {
    enum { CALLBACK_FRAMES = 241, TAIL_FRAMES = 480 };
    uint32_t out[16] = {0};
    uint8_t prm[8] = {0};
    prm[4] = (uint8_t)channels;
    CHECK(!h_au_OpenDef0(out, prm), "open a guest output for the staging boundary");
    aout *a = ao(out);
    atomic_store(&a->state, STATE_STARTED);
    atomic_store(&a->q_head, UINT32_MAX);
    atomic_store(&a->q_tail, UINT32_MAX);
    atomic_store(&a->q_played, UINT32_MAX);
    int16_t pcm[(CALLBACK_FRAMES + TAIL_FRAMES + 1) * 2] = {0}, played[CALLBACK_FRAMES * 2];
    aout_buf buffer = {.ptr = pcm, .cap = sizeof pcm, .size = (CALLBACK_FRAMES + TAIL_FRAMES) * channels * sizeof(int16_t)};
    CHECK(h_au_Append(out, &buffer), "queue a block with exactly 10 ms left after one callback");
    render_frames(a, CALLBACK_FRAMES, played);
    CHECK(h_au_Released(out) == &buffer && h_au_BufCount(out) == 1, "480-frame tail is released early and still counts across queue wrap");
    render_frames(a, TAIL_FRAMES / 2, played);
    CHECK(h_au_BufCount(out) == 1, "partly consumed staged audio still counts");
    render_frames(a, TAIL_FRAMES / 2, played);
    CHECK(!h_au_BufCount(out) && !h_au_Released(out), "draining a staged tail clears its count without releasing it twice");

    buffer.size += channels * sizeof(int16_t);
    CHECK(h_au_Append(out, &buffer), "queue a block with one frame more than 10 ms left");
    render_frames(a, CALLBACK_FRAMES, played);
    CHECK(!h_au_Released(out) && h_au_BufCount(out) == 1, "481-frame tail stays in the guest queue");
    h_au_Stop(out);
    CHECK(h_au_Released(out) == &buffer && !h_au_BufCount(out), "stop returns the unstaged block and clears the count");
    h_au_Close(out);
}
int main(void) {
    setenv("DM_AUDIO", "0", 1); setenv("DM_AUDIO_MUTE", "0", 1);
    check_staging_limit(1);
    check_staging_limit(2);
    replay_device_callbacks(0);
    replay_device_callbacks(1);
    report_entered = dispatch_semaphore_create(0); report_allow = dispatch_semaphore_create(0);
    report_done = dispatch_semaphore_create(0); released_event = dispatch_semaphore_create(0);
    uint32_t out[16] = {0}; uint8_t prm[8] = {0}, ev[64] = {0};
    CHECK(!h_au_OpenDef(out, ev, prm), "open output with a guest release event");
    atomic_store(&ao(out)->state, STATE_STARTED); atomic_store(&block_report, 1);
    CHECK(render(out, 2000, 1), "PCM and release order at first statistics boundary");
    CHECK(!atomic_load(&callback_io), "callback never enters stdio or its stream lock");
    if (atomic_load(&callback_io)) { h_au_Close(out); return 1; } // old code fails here, without waiting for a helper that never logs
    pthread_t logger; CHECK(!pthread_create(&logger, NULL, drain_stats, NULL), "run the watchdog's actual statistics drain");
    CHECK(wait_for(report_entered), "hold the diagnostic writer outside the callback");
    CHECK(render(out, 6000, 1), "a blocked diagnostic writer never stops PCM or guest release events");
    CHECK(atomic_load(&signals) == 8000, "every event-driven PCM buffer is released while diagnostics are blocked");
    CHECK(!atomic_load(&callback_io), "backlogged diagnostics still use no callback stdio");
    dispatch_semaphore_signal(report_allow);
    pthread_join(logger, NULL); stats_drain();
    CHECK(wait_for(report_done) && wait_for(report_done), "helper drains two complete snapshots");
    CHECK(render(out, 2000, 1), "callbacks continue after the diagnostic backlog");
    stats_drain();
    CHECK(wait_for(report_done), "accumulated callback statistics are eventually reported");
    CHECK(strstr(lines[0], "device callbacks 2000: 1\xe2\x80\x93" "2 frames") && strstr(lines[0], "1.0 blocks queued") && strstr(lines[0], "in these 2000, 0 underruns, 0.0 ms"), "first report preserves frame range, queue depth and underrun fields");
    CHECK(strstr(lines[1], "device callbacks 4000:") && strstr(lines[1], "in these 2000,"), "next pending snapshot is preserved");
    CHECK(strstr(lines[2], "device callbacks 10000:") && strstr(lines[2], "in these 6000,"), "a full mailbox merges later windows without losing their counts");
    h_au_Close(out);

    // The watchdog's local report may outlive the output; Close owns any newer pending snapshot.
    atomic_store(&signals, 0);
    CHECK(!h_au_OpenDef(out, ev, prm), "open another output with a guest event");
    atomic_store(&ao(out)->state, STATE_STARTED); atomic_store(&block_report, 1);
    CHECK(render(out, 2000, 0), "publish a close-case report");
    CHECK(!pthread_create(&logger, NULL, drain_stats, NULL) && wait_for(report_entered), "hold a copied watchdog report");
    CHECK(render(out, 2000, 0), "publish a second report before close");
    h_au_Close(out);
    CHECK(atomic_load(&reports) == 4, "close drains the pending snapshot while the watchdog retains only a local copy");
    dispatch_semaphore_signal(report_allow); pthread_join(logger, NULL);
    CHECK(atomic_load(&reports) == 5, "the watchdog logs its copied report after the output was freed");
    CHECK(!atomic_load(&signals), "statistics wakes never signal a guest buffer-release event");
    CHECK(!atomic_load(&callback_io), "all callback paths remain free of diagnostic stdio");

    // One buffer per event repeatedly crosses the notifier's disarm/recheck
    // boundary. Bulk PCM stress can coalesce releases and miss a stranded wake.
    atomic_store(&signals, 0);
    CHECK(!h_au_OpenDef(out, ev, prm), "open output for release-handoff stress");
    atomic_store(&ao(out)->state, STATE_STARTED);
    CHECK(render(out, 100000, 1), "every individual release handoff makes progress");
    CHECK(atomic_load(&signals) == 100000, "all 100000 event-driven buffers are released");
    CHECK(!atomic_load(&callback_io), "release-handoff stress keeps diagnostics off the callback");
    h_au_Close(out);
    dispatch_release(report_entered); dispatch_release(report_allow); dispatch_release(report_done); dispatch_release(released_event);
    printf("audio callback statistics: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

void dm_audio_device_changed(void) {}   // renderer notification delivery is checked by t_audren_work
