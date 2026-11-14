// hle_audio.c unit test (a fake capture, no microphone): SDK 21 / 22's input streams. One device is listed, and the raw device has a name; both open as the Legend of Zelda:
// Ocarina of Time opens them (PCM16 mono 48 kHz, 4800 frames); the capture starts with the first started stream and stops with the last; captured frames reach every started
// stream, a full ring keeps the newest, a read takes the oldest in the stream's format and gain, a purge empties the ring; DM_MIC=0 lists nothing and an open fails.
// A change of the default input device (or its device going away) restarts the capture while a stream is started, keeping the rings, and does nothing otherwise.
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

static int g_starts, g_stops, g_start_fails;
static int fake_start(void) { g_starts++; if (g_start_fails) { g_start_fails--; return -1; } return 0; }
static void fake_stop(void) { g_stops++; }

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); puts(""); return 1; } while (0)
int main(void) {
    enum { CAP = 4800 };
    g_mic = (mic_source){ fake_start, fake_stop };
    char names[2][0x100], raw[0x100];
    char a[0x350] = {0}, b[0x350] = {0}, f[0x350] = {0};
    int32_t fmt[3] = {IS_FMT_PCM16, 1, 48000};

    setenv("DM_MIC", "0", 1);
    if (h_is_List(names[0], 2) != 0) FAIL("DM_MIC=0 listed a device");
    if (!h_is_Open(a, NULL, 0, IS_DEVICE, fmt, CAP) || h_is_State(a) != IS_NONE) FAIL("DM_MIC=0 opened a stream");

    setenv("DM_MIC", "1", 1);
    memset(names, 0x55, sizeof names);
    if (h_is_List(names[0], 2) != 1 || strcmp(names[0], IS_DEVICE)) FAIL("list: expected one device named %s", IS_DEVICE);
    h_is_RawDev(raw);
    if (!raw[0]) FAIL("no raw device name");
    if (h_is_State(a) != IS_NONE || h_is_Filled(a) != 0) FAIL("a stream never opened is not closed and empty");

    // as the game opens them (main+0xf48168, main+0xf481a8)
    if (h_is_Open(a, NULL, 0, names[0], fmt, CAP) || h_is_Open(b, NULL, 0, raw, fmt, CAP)) FAIL("open");
    if (h_is_State(a) != IS_STOPPED || h_is_Capacity(a) != CAP || h_is_Free(a) != CAP || h_is_Filled(a) != 0) FAIL("after open: state %d, capacity %d, free %d", h_is_State(a), h_is_Capacity(a), h_is_Free(a));
    os_format of = h_is_Format(a);
    if (of.format != IS_FMT_PCM16 || of.channels != 1 || of.rate != 48000) FAIL("format %d %d %d", of.format, of.channels, of.rate);
    int32_t bad[3] = {IS_FMT_PCM16, 1, 44100};
    char c[0x350] = {0};
    if (!h_is_Open(c, NULL, 0, names[0], bad, CAP) || h_is_State(c) != IS_NONE) FAIL("a 44.1 kHz stream opened");
    int16_t none[4];
    if (h_is_Read(c, none, 4) != 0 || h_is_Filled(c) != 0 || h_is_Free(c) != 0) FAIL("a stream never opened answered with frames");

    static int16_t pcm[8000];
    for (int i = 0; i < 8000; i++) pcm[i] = (int16_t)(i - 4000);
    is_feed(pcm, 1000);
    if (h_is_Filled(a) != 0) FAIL("a stopped stream took captured frames");

    // the game starts both when the ocarina comes out (main+0x336ee98): one capture
    if (h_is_Start(a) || h_is_Start(b)) FAIL("start");
    if (h_is_State(a) != IS_STARTED || h_is_State(b) != IS_STARTED || g_starts != 1 || g_stops != 0) FAIL("after start: states %d %d, capture starts %d stops %d", h_is_State(a), h_is_State(b), g_starts, g_stops);
    is_feed(pcm, 1000);
    if (h_is_Filled(a) != 1000 || h_is_Filled(b) != 1000 || h_is_Free(a) != CAP - 1000) FAIL("after 1000 frames: filled %d %d", h_is_Filled(a), h_is_Filled(b));
    // at most 256 frames at a time, oldest first (main+0x336d7d0)
    int16_t out[256 * 2];
    if (h_is_Read(a, out, 256) != 256 || out[0] != -4000 || out[255] != -4000 + 255) FAIL("read: %d .. %d", out[0], out[255]);
    if (h_is_Filled(a) != 744 || h_is_Filled(b) != 1000) FAIL("after a read: filled %d %d", h_is_Filled(a), h_is_Filled(b));
    h_is_SetGain(a, 0.5f, 0);
    if (h_is_GetGain(a) != 0.5f || h_is_Read(a, out, 1) != 1 || out[0] != (-4000 + 256) / 2) FAIL("gain 0.5: %d", out[0]);
    h_is_SetGain(a, 1.0f, 0);
    if (h_is_Read(a, out, 256) != 256 || h_is_Read(a, out, 1000) != 487 || h_is_Filled(a) != 0 || h_is_Read(a, out, 10) != 0) FAIL("draining the ring");

    // the stream the game does not read fills up: it keeps the newest frames
    is_feed(pcm, 8000);
    if (h_is_Filled(b) != CAP) FAIL("a full ring: filled %d", h_is_Filled(b));
    if (h_is_Read(b, out, 1) != 1 || out[0] != pcm[8000 - CAP]) FAIL("a full ring kept %d, not the newest %d frames", out[0], CAP);
    is_feed(pcm, 10);
    if (h_is_Read(b, out, 1) != 1 || out[0] != pcm[8000 - CAP + 10]) FAIL("wrap-around: %d", out[0]);

    // the default input device changes with the ocarina out (AirPods picked, or disconnected): one restart of the capture, the streams and what they hold stay
    h_is_Purge(a); h_is_Purge(b);
    is_feed(pcm, 300);
    int s0 = g_starts, t0 = g_stops;
    if (!mic_restart("test: default input changed") || g_starts != s0 + 1 || g_stops != t0 + 1) FAIL("restart: capture starts %d stops %d", g_starts - s0, g_stops - t0);
    if (h_is_State(a) != IS_STARTED || h_is_State(b) != IS_STARTED || h_is_Filled(a) != 300 || h_is_Filled(b) != 300) FAIL("after a restart: states %d %d, filled %d %d", h_is_State(a), h_is_State(b), h_is_Filled(a), h_is_Filled(b));
    is_feed(pcm + 300, 100);   // the new device's frames follow the old ones
    if (h_is_Read(a, out, 256) != 256 || out[0] != pcm[0] || out[255] != pcm[255] || h_is_Filled(a) != 144) FAIL("frames across a restart: %d .. %d, filled %d", out[0], out[255], h_is_Filled(a));
    if (h_is_Read(a, out, 256) != 144 || out[143] != pcm[399]) FAIL("the new device's frames: %d", out[143]);
    // no input device at all (the last one disconnected): the start fails, the streams stay started and keep their frames; the next change brings the capture back
    g_start_fails = 1;
    if (!mic_restart("test: device gone") || h_is_State(b) != IS_STARTED || h_is_Filled(b) != 400) FAIL("a failed restart: state %d, filled %d", h_is_State(b), h_is_Filled(b));
    if (!mic_restart("test: device back") || g_starts != s0 + 3 || g_stops != t0 + 3) FAIL("restart after a failed one: capture starts %d stops %d", g_starts - s0, g_stops - t0);

    // between songs the game stops, purges and starts both again (main+0x336d778)
    h_is_Stop(a); h_is_Purge(a);
    if (h_is_State(a) != IS_STOPPED || h_is_Filled(a) != 0 || g_stops != t0 + 3) FAIL("stop one: state %d, filled %d, capture stops %d", h_is_State(a), h_is_Filled(a), g_stops);
    h_is_Stop(b); h_is_Purge(b);
    if (g_stops != t0 + 4 || h_is_Filled(b) != 0) FAIL("stop both: capture stops %d, filled %d", g_stops, h_is_Filled(b));
    s0 = g_starts; t0 = g_stops;
    if (mic_restart("test: default input changed, nothing started") || g_starts != s0 || g_stops != t0) FAIL("a device change with no stream started touched the capture");
    if (h_is_Start(a) || h_is_Start(b) || g_starts != s0 + 1) FAIL("start again: capture starts %d", g_starts - s0);
    int starts_after = g_starts, stops_after = g_stops;

    // a float stereo stream, with an event: the frames come out as float in both channels
    int32_t ffmt[3] = {IS_FMT_FLOAT, 2, 48000};
    char ev[64];
    if (h_is_OpenEv(f, ev, NULL, 0, names[0], ffmt, 480, 0) || h_is_Start(f)) FAIL("float stream");
    int sig0 = atomic_load(&g_sig);
    int16_t two[2] = {16384, -32768};
    is_feed(two, 2);
    float fo[4];
    if (h_is_Read(f, fo, 2) != 2 || fo[0] != 0.5f || fo[1] != 0.5f || fo[2] != -1.0f || fo[3] != -1.0f) FAIL("float read: %f %f %f %f", fo[0], fo[1], fo[2], fo[3]);
    if (atomic_load(&g_sig) == sig0) FAIL("no event for captured frames");
    if (g_starts != starts_after) FAIL("a third started stream started the capture again");

    // the game's close path (main+0x336d220): close while started, the capture stops with the last
    h_is_Close(f); h_is_Close(a);
    if (g_stops != stops_after) FAIL("capture stopped with a stream still started");
    h_is_Close(b);
    if (g_stops != stops_after + 1 || h_is_State(a) != IS_NONE || h_is_State(b) != IS_NONE || h_is_State(f) != IS_NONE) FAIL("after close: capture stops %d, states %d %d %d", g_stops, h_is_State(a), h_is_State(b), h_is_State(f));
    is_feed(pcm, 100);

    // DM_MIC_WAV: a stereo 16-bit 48 kHz file is taken as its first channel; a 44.1 kHz one is refused
    char dir[512], wpath[600], dpath[600], tpath[600];
    snprintf(dir, sizeof dir, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    snprintf(wpath, sizeof wpath, "%s/t_input_stream_in.wav", dir);
    FILE *wf = fopen(wpath, "wb"); if (!wf) FAIL("cannot write %s", wpath);
    wav_header(wf, IS_FMT_PCM16, 2, 4 * 4);
    int16_t st[8] = {100, -1, 200, -1, 300, -1, 400, -1};
    fwrite(st, 2, 8, wf); fclose(wf);
    if (wav_load(wpath) || g_wav_frames != 4 || g_wav[0] != 100 || g_wav[3] != 400) FAIL("DM_MIC_WAV load: %d frames, %d .. %d", g_wav_frames, g_wav ? g_wav[0] : 0, g_wav ? g_wav[3] : 0);
    free(g_wav); g_wav = NULL;
    snprintf(wpath, sizeof wpath, "%s/t_input_stream_441.wav", dir);
    wf = fopen(wpath, "wb"); wav_header(wf, IS_FMT_PCM16, 1, 4); fseek(wf, 24, SEEK_SET); uint32_t r441 = 44100; fwrite(&r441, 4, 1, wf); fseek(wf, 44, SEEK_SET); fwrite(st, 2, 2, wf); fclose(wf);
    if (!wav_load(wpath) || g_wav) FAIL("a 44.1 kHz DM_MIC_WAV was taken");

    // DM_MIC_DUMP: what a read returns lands in <file>.<stream>.wav, and one line per read in <file>.tsv
    snprintf(dpath, sizeof dpath, "%s/t_input_stream_dump.wav", dir);
    setenv("DM_MIC_DUMP", dpath, 1); g_dump_on = -1;
    char d[0x350] = {0};
    if (h_is_Open(d, NULL, 0, names[0], fmt, CAP) || h_is_Start(d)) FAIL("dump stream");
    is_feed(pcm, 300);
    int first_read = h_is_Read(d, out, 256), second_read = h_is_Read(d, out, 256);
    if (first_read != 256 || second_read != 44) FAIL("dump reads");
    istream *ds = is_of(d); int did = ds->id;
    h_is_Close(d);
    snprintf(dpath, sizeof dpath, "%s/t_input_stream_dump.%d.wav", dir, did);
    FILE *df = fopen(dpath, "rb"); if (!df) FAIL("no dump file %s", dpath);
    uint8_t hdr[44]; int16_t first, last; uint32_t dbytes;
    if (fread(hdr, 1, 44, df) != 44 || fread(&first, 2, 1, df) != 1) FAIL("dump file short");
    memcpy(&dbytes, hdr + 40, 4); fseek(df, 44 + 299 * 2, SEEK_SET);
    if (fread(&last, 2, 1, df) != 1) FAIL("dump file short");
    fclose(df);
    if (memcmp(hdr, "RIFF", 4) || dbytes != 300 * 2 || first != pcm[0] || last != pcm[299]) FAIL("dump: %u bytes, %d .. %d", dbytes, first, last);
    snprintf(tpath, sizeof tpath, "%s/t_input_stream_dump.tsv", dir);
    FILE *tf = fopen(tpath, "r"); int lines = 0; char lb[128];
    while (tf && fgets(lb, sizeof lb, tf)) lines++;
    if (tf) fclose(tf);
    if (lines != 3) FAIL("dump tsv: %d lines, expected a header and 2 reads", lines);

    // The capture's device: a Bluetooth input that is also the output (AirPods for both) would switch the headset to its call profile, so the built-in microphone is taken,
    // or none without one; the same headset's input and output may be two devices with UIDs "<address>:input" and "<address>:output"
    const uint32_t BT = kAudioDeviceTransportTypeBluetooth, BLE = kAudioDeviceTransportTypeBluetoothLE, USB = kAudioDeviceTransportTypeUSB, BUILTIN = kAudioDeviceTransportTypeBuiltIn;
    mic_dev pods = {71, BT, "AC-90-85-00-00-01:input"}, pods_out = {72, BT, "AC-90-85-00-00-01:output"}, other_out = {73, BT, "AC-90-85-00-00-02:output"},
            same = {71, BLE, ""}, speakers = {80, BUILTIN, "BuiltInSpeakerDevice"}, usb = {90, USB, "usb-mic"};
    if (mic_pick(&pods, &pods, 1) != MIC_TAKE_BUILTIN) FAIL("AirPods as input and output (one device): the built-in microphone expected");
    if (mic_pick(&pods, &pods_out, 1) != MIC_TAKE_BUILTIN) FAIL("AirPods as input and output (two devices, one address): the built-in microphone expected");
    if (mic_pick(&same, &same, 1) != MIC_TAKE_BUILTIN) FAIL("a Bluetooth LE device as input and output: the built-in microphone expected");
    if (mic_pick(&pods, &pods_out, 0) != MIC_TAKE_NONE) FAIL("AirPods as input and output without a built-in microphone: no capture expected");
    if (mic_pick(&pods, &speakers, 1) != MIC_TAKE_DEFAULT) FAIL("AirPods as input, the speakers as output: the AirPods expected");
    if (mic_pick(&pods, &other_out, 1) != MIC_TAKE_DEFAULT) FAIL("AirPods as input, other Bluetooth headphones as output: the AirPods expected");
    if (mic_pick(&usb, &usb, 1) != MIC_TAKE_DEFAULT) FAIL("a USB headset as input and output: the headset expected");

    printf("InputStream: listed %s, raw %s; capture started %d and stopped %d times (3 of each by device changes); file feed, dump and Bluetooth device choice checked\n", names[0], raw, g_starts, g_stops);
    return 0;
}
