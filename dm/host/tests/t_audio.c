// hle_audio.c unit test (silent sink): release events wake the reader, which drains the buffers in order.
// Expected values are computed directly from the sample rate and block length: 4800 frames / 48 kHz = 100 ms.
#include "../hle_audio.c"
#include <time.h>
#include <sys/mman.h>
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }
static _Atomic int g_sig;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add_explicit(&g_sig, 1, memory_order_seq_cst); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }   // the real implementation needs a guest thread + SDK kernel event; the unit test only counts triggers
module_t *module_of(const void *p) { (void)p; return NULL; }   // call sites in the log: no guest modules in the unit test
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }   // hle_svc.c: no guest address space in the unit test, so the big-stack take-overs (Start / Stop / CloseAudioOut, hle_bigstack.c) run on the calling host stack
void dm_host_helper_qos(qos_class_t q) { (void)q; }   // hle_thread.c: the helper threads keep the default class in the unit test

// A callback can finish several buffers, and the notifier can combine pending wakes.
// Only inspect the released queue after an event, then take everything it announced.
static int drain_signaled(void *out, aout_buf *buffers, int count, int *got, int *seen) {
    int signals = atomic_load_explicit(&g_sig, memory_order_acquire);
    if (signals == *seen) return 1;
    *seen = signals;
    aout_buf *r;
    while ((r = h_au_Released(out))) {
        if (*got >= count || r != &buffers[*got]) {
            fprintf(stderr, "unexpected released buffer at index %d of %d\n", *got, count);
            return 0;
        }
        (*got)++;
    }
    return 1;
}

// Exercise the actual device callback, not just a mixing helper. A stereo
// device must hear every channel of a 5.1 guest buffer and release the complete
// six-channel buffers at the same frame count, including a split inside a block.
static int test_surround_output(void) {
    enum { FRAMES = 273, SPLIT = 17 };
    const int16_t expected[8][2] = {
        {10000, 0}, {0, 10000}, {7070, 7070}, {2509, 2509},
        {7070, 0}, {0, 7070}, {32767, 32767}, {-32768, -32768}
    };
    for (int test = 0; test < 8; test++) {
        int16_t in[FRAMES * 6] = {0}, out[FRAMES * 2];
        for (int f = 0; f < FRAMES; f++) for (int ch = 0; ch < 6; ch++)
            in[f * 6 + ch] = test < 6 ? (test == ch ? 10000 : 0) : test == 6 ? 32767 : -32768;
        memset(out, 0xa5, sizeof out);
        aout_buf b[2] = {{.ptr = in, .cap = SPLIT * 12, .size = SPLIT * 12}, {.ptr = in + SPLIT * 6, .cap = (FRAMES - SPLIT) * 12, .size = (FRAMES - SPLIT) * 12}};
        aout a = {.ch = 6};
        atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
        a.q[0] = b; a.q[1] = b + 1; atomic_init(&a.q_tail, 2);
        AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof out, out}}};
        au_render(&a, NULL, NULL, 0, FRAMES, &bl);
        for (int f = 0; f < FRAMES; f++) for (int ch = 0; ch < 2; ch++) {
            if (out[f * 2 + ch] != expected[test][ch]) {
                printf("FAIL surround callback: case %d frame %d channel %d expected %d got %d\n", test, f, ch, expected[test][ch], out[f * 2 + ch]); return 1;
            }
        }
        if (atomic_load(&a.q_head) != 2 || atomic_load(&a.rel_tail) != 2 || a.rel[0] != b || a.rel[1] != b + 1 || a.pos || atomic_load(&a.under_n)) {
            puts("FAIL surround callback: guest buffer consumption / release order"); return 1;
        }
    }
    puts("surround callback: all six channels audible, saturation and split-buffer release verified");
    return 0;
}
// What the sdk's SetAudioOutBufferInfo does (Hades's sdk+0x142ab0, Metro 2033 Redux's sdk+0x80f80, the same in SDK 8 to Switch 2's): the host leaves it and the other descriptor
// functions to the sdk and reads the guest's AudioOutBuffer itself
static void set_buf(aout_buf *b, void *p, uint64_t cap, uint64_t size) { b->ptr = p; b->cap = cap; b->size = size; }
// The host plays a buffer as the SDK describes it: the object spelled out as the SDK has it (not as aout_buf has it, so the check is about the ABI and not about our own names),
// next linked as AudioBufferList links it, the data size 4 bytes short of the capacity. The callback must play exactly the data size from the data pointer and release the buffer.
// The host's old private layout (data +0x00, capacity +0x08, data size +0x10) read next as the data and the capacity as the data size (Metro 2033 Redux)
static int test_buffer_layout(void) {
    enum { FRAMES = 64 };
    static int16_t pcm[FRAMES * 2 + 2], other[4];
    for (int i = 0; i < FRAMES * 2 + 2; i++) pcm[i] = (int16_t)(i < FRAMES * 2 ? 100 + i : -1);
    struct { void *next; void *data; uint64_t buffer_size, data_size, data_offset; } sdk = {other, pcm, sizeof pcm, FRAMES * 4, 0};
    _Static_assert(sizeof sdk == sizeof(aout_buf), "nn::audio::AudioOutBuffer size");
    int16_t out[(FRAMES + 1) * 2]; memset(out, 0x5a, sizeof out);
    aout a = {.ch = 2};
    atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
    a.q[0] = (aout_buf *)&sdk; atomic_init(&a.q_tail, 1);
    AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof out, out}}};
    au_render(&a, NULL, NULL, 0, FRAMES + 1, &bl);
    for (int i = 0; i < FRAMES * 2; i++) if (out[i] != 100 + i) { printf("FAIL buffer layout: sample %d is %d, want %d (the data the SDK described)\n", i, out[i], 100 + i); return 1; }
    if (out[FRAMES * 2] || out[FRAMES * 2 + 1]) { puts("FAIL buffer layout: played past the data size"); return 1; }
    if (atomic_load(&a.rel_tail) != 1 || a.rel[0] != (aout_buf *)&sdk || sdk.next != other) { puts("FAIL buffer layout: the buffer was not released whole, or its next was touched"); return 1; }
    puts("buffer layout: the data at +0x08 played for the data size at +0x18, next at +0x00 left as the game linked it");
    return 0;
}
// The guest AudioOut's front as the SDK fills it (au_info), and its lifetime under a guard like the middleware's: open, play, go to the background, close only if the first word says open,
// make the PCM inaccessible (a consumer left running would fault on it) and come back, 12 times (PR #109). Old pages stay mapped PROT_NONE, so no new buffer lands at a stale address
static int test_output_lifetime(void) {
    enum { CYCLES = 12 };
    uint32_t out[16] = {0}; uint8_t prm[16];
    size_t page = (size_t)getpagesize(), bytes = page * 4;
    void *pcm[CYCLES]; aout_buf *buf[CYCLES];
    h_au_InitParam(prm);
    for (int i = 0; i < CYCLES; i++) {
        pcm[i] = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        buf[i] = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pcm[i] == MAP_FAILED || buf[i] == MAP_FAILED) return 1;
        memset(out, 0, sizeof out);
        if (h_au_OpenDef0(out, prm)) return 1;
        if (out[0] != RATE || out[1] != 2 || out[2] != FMT_PCM16 || out[3] != STATE_STOPPED) {
            printf("FAIL AudioOut open: guest fields %u %u %u %u, expected %u 2 %u %u as the SDK writes them\n", out[0], out[1], out[2], out[3], RATE, FMT_PCM16, STATE_STOPPED); return 1;
        }
        set_buf(buf[i], pcm[i], bytes, bytes);
        if (!h_au_Append(out, buf[i])) return 1;
        h_au_Start(out); usleep(1000); dm_audio_suspend(1);
        if (out[0]) { if (h_au_State(out) == STATE_STARTED) h_au_Stop(out); h_au_Close(out); }
        if (out[0] != RATE || out[3] != STATE_STOPPED || objmap_get(out)) { puts("FAIL AudioOut close: state not stopped or the host output still open"); return 1; }
        for (int k = 0; k < AOMAX; k++) if (g_aos[k]) { puts("FAIL AudioOut close: a consumer is still registered"); return 1; }
        if (mprotect(pcm[i], bytes, PROT_NONE) || mprotect(buf[i], page, PROT_NONE)) return 1;
        dm_audio_suspend(0); usleep(10000);
    }
    for (int i = 0; i < CYCLES; i++) { munmap(pcm[i], bytes); munmap(buf[i], page); }
    printf("AudioOut fields and lifetime (%s): %d guarded open / close / unmap / resume cycles, no consumer left\n", real_audio() ? "real output" : "silent sink", CYCLES);
    return 0;
}
// audout takes 0 or 48000 and refuses any other rate with 0x699 (ResultInvalidSampleRate). Mega Man Legacy Collection (SDK 3.7) asks for 44.1 kHz first and opens again with the
// defaults when refused: the refused open leaves no output and the guest AudioOut as it was, and the second one opens at 48 kHz
static int test_rate_refused(void) {
    uint32_t out[16] = {0}; uint8_t prm[16];
    h_au_InitParam(prm); *(int32_t *)prm = 44100;
    Result r = h_au_OpenDef0(out, prm);
    if (r != 0x699 || objmap_get(out) || out[0] || out[1]) { printf("FAIL 44.1 kHz open: result %#x, output %p, guest fields %u %u (0x699, none, 0 0 expected)\n", r, objmap_get(out), out[0], out[1]); return 1; }
    for (int k = 0; k < AOMAX; k++) if (g_aos[k]) { puts("FAIL 44.1 kHz open: a consumer is registered"); return 1; }
    *(int32_t *)prm = 0;
    if (h_au_OpenDef0(out, prm) || out[0] != RATE || h_au_Rate(out) != RATE) { puts("FAIL the open with the default rate after the refusal"); return 1; }
    h_au_Close(out);
    *(int32_t *)prm = RATE;
    if (h_au_OpenDef0(out, prm)) { puts("FAIL a 48 kHz open"); return 1; }
    h_au_Close(out);
    puts("AudioOut rate: 44.1 kHz refused with 0x699 and nothing opened; 0 and 48000 open at 48 kHz");
    return 0;
}
// DM_AUDIO_DUMP: each output's accepted blocks go to a file of their own (<file> for #0, <file>.<N> after it), blocks refused by a full queue are omitted,
// and a host DeviceSink output unpacks its 2-channel stereo fold on the silent sink instead of dumping the 8-slot internal layout.
static int test_dump_per_output(void) {
    char path[512]; snprintf(path, sizeof path, "%s/t_audio_dump_%d.raw", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    setenv("DM_AUDIO_DUMP", path, 1);
    char o1[64], o2[64], ev[64]; uint8_t prm[16]; aout_buf b1[2], b2[2], bs; static int16_t p1[2][480], p2[2][480], ps[257 * DM_AUDIO_SINK_CHANNELS];
    for (int i = 0; i < 2; i++) for (int j = 0; j < 480; j++) { p1[i][j] = 1; p2[i][j] = 2; }
    for (int i = 0; i < 257; i++) for (int j = 0; j < DM_AUDIO_SINK_CHANNELS; j++) ps[i * DM_AUDIO_SINK_CHANNELS + j] = (int16_t)(1000 * j + i);
    h_au_InitParam(prm);
    int o1_open = !h_au_Open(o1, ev, NULL, prm), o2_open = o1_open && !h_au_Open(o2, ev, NULL, prm);
    if (!o2_open) { if (o1_open) h_au_Close(o1); unsetenv("DM_AUDIO_DUMP"); return 1; }
    void *sink = dm_pcmout_open_sink();
    int id1 = ao(o1)->id, id2 = ao(o2)->id, ids = ao(sink)->id, ok = 1;
    for (int i = 0; ok && i < 2; i++) {   // appended in turn, as Divinity's two outputs were
        set_buf(&b1[i], p1[i], sizeof p1[i], sizeof p1[i]); set_buf(&b2[i], p2[i], sizeof p2[i], sizeof p2[i]);
        ok &= h_au_Append(o1, &b1[i]) && h_au_Append(o2, &b2[i]);
    }
    // Fill o1's queue to QMAX with empty descriptors, then verify a refused non-empty block is not written to o1's dump
    aout_buf pad[QMAX], refused; static int16_t bad[480];
    for (int j = 0; j < 480; j++) bad[j] = 9;
    set_buf(&refused, bad, sizeof bad, sizeof bad);
    for (int i = 2; ok && i < QMAX; i++) { set_buf(&pad[i], NULL, 0, 0); ok &= h_au_Append(o1, &pad[i]) != 0; }
    if (ok && h_au_Append(o1, &refused)) { puts("FAIL DM_AUDIO_DUMP: full queue accepted an extra block"); ok = 0; }
    set_buf(&bs, ps, sizeof ps, sizeof ps);
    if (ok) ok &= dm_pcmout_append(sink, &bs) != 0;
    h_au_Close(o1); h_au_Close(o2); dm_pcmout_close(sink); unsetenv("DM_AUDIO_DUMP");
    for (int k = 0; k < 2; k++) {
        char p[600]; int id = k ? id2 : id1;
        if (id) snprintf(p, sizeof p, "%s.%d", path, id); else snprintf(p, sizeof p, "%s", path);
        FILE *f = fopen(p, "rb"); int16_t v[2000]; size_t n = f ? fread(v, 2, 2000, f) : 0; if (f) fclose(f); unlink(p);
        int same = n == 960; for (size_t i = 0; i < n; i++) same &= v[i] == (k ? 2 : 1);
        if (!same) { printf("FAIL DM_AUDIO_DUMP: output #%d's file %s holds %zu samples, not its own 960\n", id, p, n); ok = 0; }
    }
    char sp[600]; snprintf(sp, sizeof sp, "%s.%d", path, ids);
    FILE *sf = fopen(sp, "rb"); int16_t sv[257 * DM_AUDIO_SINK_CHANNELS]; size_t sn = sf ? fread(sv, 2, sizeof sv / 2, sf) : 0; if (sf) fclose(sf); unlink(sp);
    int sink_ok = sn == 257 * 2;
    for (int i = 0; sink_ok && i < 257; i++) sink_ok &= sv[2 * i] == 6000 + i && sv[2 * i + 1] == 7000 + i;
    if (!sink_ok) { printf("FAIL DM_AUDIO_DUMP: DeviceSink #%d's file %s holds %zu samples, not the 2-channel stereo fold\n", ids, sp, sn); ok = 0; }
    if (ok) printf("DM_AUDIO_DUMP: outputs #%d, #%d and DeviceSink #%d each in a file of their own\n", id1, id2, ids);
    return !ok;
}
int main(void) {
    if (test_dump_per_output()) return 1;
    if (test_buffer_layout() || test_surround_output() || test_output_lifetime() || test_rate_refused()) return 1;
    char out[64], ev[64]; uint8_t prm[16]; aout_buf b[3]; static int16_t pcm[3][4800 * 2];
    h_au_InitParam(prm);
    if (h_au_Open(out, ev, NULL, prm)) return 1;
    if (h_au_Ch(out) != 2 || h_au_Rate(out) != 48000 || h_au_State(out) != STATE_STOPPED) { puts("FAIL parameters"); return 1; }
    for (int i = 0; i < 3; i++) { set_buf(&b[i], pcm[i], sizeof pcm[i], sizeof pcm[i]); if (!h_au_Append(out, &b[i])) return 1; }
    if (h_au_Released(out)) { puts("FAIL buffers released before start"); return 1; }
    uint64_t t0 = now_ns(); h_au_Start(out);
    int got = 0, seen = 0, order_ok = 1;
    while (got < 3 && now_ns() - t0 < 10000000000ull) {
        order_ok = drain_signaled(out, b, 3, &got, &seen);
        if (!order_ok) break;
        if (got < 3) usleep(1000);
    }
    h_au_Close(out);
    printf("released %d/3 in order=%d after %d signals\n", got, order_ok, seen);
    if (!order_ok || got != 3) return 1;

    // Scenario 2: the real audio output sink (PCM all 0, inaudible) plays 6 blocks of 100 ms each, pausing at 150 ms (app to background / incoming call) and resuming at 350 ms.
    // During the pause the silent sink takes over. Release events must let the reader reclaim all six blocks in order at real-time pace.
    setenv("DM_AUDIO", "1", 1);
    if (test_output_lifetime()) return 1;
    char out2[64]; aout_buf b2[6]; static int16_t pcm2[6][4800 * 2]; g_sig = 0;
    if (h_au_Open(out2, ev, NULL, prm)) return 1;
    for (int i = 0; i < 6; i++) { set_buf(&b2[i], pcm2[i], sizeof pcm2[i], sizeof pcm2[i]); h_au_Append(out2, &b2[i]); }
    t0 = now_ns(); h_au_Start(out2);
    int got2 = 0, seen2 = 0, susp = 0, resumed = 0;
    while (got2 < 6 && now_ns() - t0 < 10000000000ull) {
        uint64_t ms = (now_ns() - t0) / 1000000;
        if (!susp && ms >= 150) { dm_audio_suspend(1); susp = 1; }
        // Stopping the AudioUnit can take past the resume deadline while buffers finish.
        if (susp && !resumed && (now_ns() - t0) / 1000000 >= 350) { dm_audio_suspend(0); resumed = 1; }
        order_ok = drain_signaled(out2, b2, 6, &got2, &seen2);
        if (!order_ok) break;
        if (got2 < 6) usleep(1000);
    }
    double total = (now_ns() - t0) / 1e6;
    h_au_Close(out2);
    int ok2 = got2 == 6 && order_ok && susp && resumed && total > 520;
    printf("suspend / resume: released %d/6 in order=%d after %d signals, total duration lower bound 520 ms, got %.0f ms %s\n", got2, order_ok, seen2, total, ok2 ? "✓" : "✗");
    if (!ok2) return 1;

    // Scenario 3: the device stops calling back while the output plays (iOS does this when another audio session takes over, without telling the AudioUnit). 20 blocks of 100 ms; at 150 ms the AudioUnit is stopped behind the host's back.
    // Expected: the watchdog sees no callback for 200 ms, the silent sink takes over (all 20 blocks still released in order, about 2 s plus the 200-250 ms gap), and the AudioUnit is restarted 1 s later and calls back again
    // Allow 5 s for scheduling and AudioUnit stop/start delays on virtual CI Macs; buffer order, the playback lower bound and resumed callbacks still have to hold.
    // The virtual output of a CI Mac can also pause by itself for more than 200 ms after the restart: the watchdog then rightly stands the silent sink in again and retries 1 s later, which can fall after the
    // last release (CI: "AudioUnit restarted", a second "has not called back for 204 ms", then Close 1 s later, "calling back again=0"). So the backend is followed on every poll until a restarted AudioUnit
    // has called back, up to 20 s (the watchdog's back-off of 1, 2, 4 s fits), instead of being read once 100 ms after the last release; it still fails when no restarted AudioUnit ever calls back
    char out3[64]; aout_buf b3[20]; static int16_t pcm3[20][4800 * 2]; g_sig = 0;
    if (h_au_Open(out3, ev, NULL, prm)) return 1;
    for (int i = 0; i < 20; i++) set_buf(&b3[i], pcm3[i], sizeof pcm3[i], sizeof pcm3[i]);
    int app3 = 0; for (; app3 < 20 && h_au_Append(out3, &b3[app3]); app3++) ;
    aout *a3 = ao(out3); t0 = now_ns(); h_au_Start(out3);
    int got3 = 0, order3 = 1, cut = 0, standins = 0, sink = 0, restarted = 0, back = 0; uint64_t cb_restart = 0; double total3 = 0;
    while (!(got3 == 20 && back) && now_ns() - t0 < 20000000000ull) {
        uint64_t ms = (now_ns() - t0) / 1000000;
        if (!cut && ms >= 150) { AudioOutputUnitStop(a3->au); cut = 1; }
        // The watchdog changes the backend under g_aos_lock, and a restart comes at least 1 s after the silent sink stood in, so a 1 ms poll sees every stand-in and every restart
        pthread_mutex_lock(&g_aos_lock);
        if (cut && a3->backend == BK_NULL && a3->stalled) { standins += !sink; sink = 1; restarted = 0; }
        else if (standins && a3->backend == BK_AU && !a3->stalled) {
            uint64_t n = atomic_load(&a3->cb_n); sink = 0;
            if (!restarted) { restarted = 1; cb_restart = n; } else if (n > cb_restart) back = 1;   // callbacks counted after the restart was seen
        }
        pthread_mutex_unlock(&g_aos_lock);
        aout_buf *r = got3 < 20 ? h_au_Released(out3) : NULL;
        if (!r) { usleep(1000); continue; }
        order3 &= r == &b3[got3];
        if (++got3 == 20) total3 = (now_ns() - t0) / 1e6;
        if (app3 < 20) h_au_Append(out3, &b3[app3++]);
    }
    double waited3 = (now_ns() - t0) / 1e6;
    h_au_Close(out3);
    int ok3 = got3 == 20 && order3 && total3 > 1900 && total3 < 5000 && back;
    printf("device stall: released %d/20 in order=%d in %.0f ms (expected 1900-5000), AudioUnit restarted and calling back again=%d (silent sink stood in %d times, %.0f ms) %s\n", got3, order3, total3, back, standins, waited3, ok3 ? "✓" : "✗");
    return !ok3;
}

void dm_audio_device_changed(void) {}   // renderer notification delivery is checked by t_audren_work
