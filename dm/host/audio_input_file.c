// A recording as the host's audio input (audio_input_file.h): the file is decoded into memory as mono Float32 at 48 kHz, and a thread of
// its own hands it to dm_audio_input_process in blocks of 512 frames, as many as the time since the start makes due, so a game reads it at
// the pace it reads a microphone. Past the file's end the blocks are silence.
#include "audio_input_file.h"
#include "audio_input.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { FILE_BLOCK = 512, FILE_MAX_SECONDS = 3600, FILE_READ_FRAMES = 8192 };

static struct {
    pthread_mutex_t lock;
    float *pcm;
    uint64_t frames;
    pthread_t thread;
    int loaded, thread_up;
    uint64_t started_ns;
    char path[1024];   // the file the samples came from: another DM_MIC_FILE at a later capture (DM_CTL's env) is read then
    _Atomic int running;
} g_file = { .lock = PTHREAD_MUTEX_INITIALIZER };

static const char *file_path(void) { const char *p = getenv("DM_MIC_FILE"); return p && *p ? p : NULL; }

int dm_mic_file_selected(void) { return file_path() != NULL; }

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && !strcasecmp(s + n - t, tail);
}

static int grow(float **pcm, uint64_t *cap, uint64_t need) {
    if (need <= *cap) return 1;
    uint64_t n = *cap ? *cap * 2 : 1u << 20;
    while (n < need) n *= 2;
    float *p = realloc(*pcm, n * sizeof(float));
    if (!p) return 0;
    *pcm = p; *cap = n;
    return 1;
}

// 16-bit mono at 48 kHz without a header: DM_MIC_DUMP's own format
static int load_raw(const char *path, float **out, uint64_t *frames) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    float *pcm = NULL; uint64_t cap = 0, n = 0; int16_t block[4096];
    for (;;) {
        size_t got = fread(block, sizeof(int16_t), 4096, f);
        if (got && !grow(&pcm, &cap, n + got)) { free(pcm); fclose(f); return 0; }
        for (size_t i = 0; i < got; i++) pcm[n + i] = (float)block[i] / 32768.0f;
        n += got;
        if (got < 4096 || n >= (uint64_t)FILE_MAX_SECONDS * DM_AUDIO_INPUT_RATE) break;   // a short read is the end of the file or an error: no read after it
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) { free(pcm); return 0; }
    *out = pcm; *frames = n;
    return 1;
}

static int load_decoded(const char *path, float **out, uint64_t *frames) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    ExtAudioFileRef file = NULL;
    OSStatus status = url ? ExtAudioFileOpenURL(url, &file) : kAudio_ParamError;
    if (url) CFRelease(url);
    if (status != noErr) return 0;
    AudioStreamBasicDescription source = {0}; UInt32 size = sizeof source;
    status = ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &source);
    UInt32 channels = source.mChannelsPerFrame;
    if (status != noErr || channels < 1 || channels > 8) { ExtAudioFileDispose(file); return 0; }
    AudioStreamBasicDescription client = {0};   // every channel kept, interleaved: they are averaged here, as the capture averages a device's
    client.mSampleRate = DM_AUDIO_INPUT_RATE; client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = channels; client.mBitsPerChannel = 32; client.mFramesPerPacket = 1;
    client.mBytesPerFrame = client.mBytesPerPacket = channels * sizeof(float);
    status = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client);
    float *pcm = NULL, *block = malloc((size_t)FILE_READ_FRAMES * channels * sizeof(float)); uint64_t cap = 0, n = 0;
    while (status == noErr && block && n < (uint64_t)FILE_MAX_SECONDS * DM_AUDIO_INPUT_RATE) {
        AudioBufferList list = { 1, { { channels, (UInt32)(FILE_READ_FRAMES * channels * sizeof(float)), block } } };
        UInt32 got = FILE_READ_FRAMES;
        status = ExtAudioFileRead(file, &got, &list);
        if (status != noErr || !got) break;
        if (!grow(&pcm, &cap, n + got)) { status = kAudio_MemFullError; break; }
        for (UInt32 i = 0; i < got; i++) {
            float mono = 0.0f;
            for (UInt32 c = 0; c < channels; c++) mono += block[i * channels + c];
            pcm[n + i] = mono / (float)channels;
        }
        n += got;
    }
    free(block);
    ExtAudioFileDispose(file);
    if (status != noErr || !n) { free(pcm); return 0; }
    *out = pcm; *frames = n;
    return 1;
}

int dm_mic_file_prepare(void) {
    pthread_mutex_lock(&g_file.lock);
    const char *path = file_path();
    if (g_file.loaded && !g_file.thread_up && path && strcmp(path, g_file.path)) { free(g_file.pcm); g_file.pcm = NULL; g_file.frames = 0; g_file.loaded = 0; }
    if (!g_file.loaded) {
        float *pcm = NULL; uint64_t frames = 0;
        int ok = path && (ends_with(path, ".pcm") || ends_with(path, ".raw") ? load_raw(path, &pcm, &frames) : load_decoded(path, &pcm, &frames));
        if (ok && frames) {
            g_file.pcm = pcm; g_file.frames = frames; g_file.loaded = 1; strlcpy(g_file.path, path, sizeof g_file.path);
            fprintf(stderr, "[dm] audio input: opened file=\"%s\" %.2f s as the microphone (DM_MIC_FILE)\n", path, (double)frames / DM_AUDIO_INPUT_RATE);
        } else {
            free(pcm);
            fprintf(stderr, "[RED] audio input: DM_MIC_FILE %s could not be read as audio\n", path ? path : "");
        }
    }
    int ok = g_file.loaded;
    pthread_mutex_unlock(&g_file.lock);
    return ok ? 0 : -1;
}

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void *file_feed(void *arg) {
    (void)arg;
    pthread_setname_np("dm-mic-file");
    uint64_t t0 = now_ns(), sent = 0; int ended = 0;
    float block[FILE_BLOCK]; const float *planes[1] = { block };
    while (atomic_load_explicit(&g_file.running, memory_order_acquire)) {
        uint64_t due = (now_ns() - t0) / 1000 * DM_AUDIO_INPUT_RATE / 1000000;
        while (sent + FILE_BLOCK <= due && atomic_load_explicit(&g_file.running, memory_order_acquire)) {
            for (uint64_t i = 0; i < FILE_BLOCK; i++) block[i] = sent + i < g_file.frames ? g_file.pcm[sent + i] : 0.0f;
            dm_audio_input_process(planes, 1, FILE_BLOCK, DM_AUDIO_INPUT_RATE);
            sent += FILE_BLOCK;
            if (!ended && sent >= g_file.frames) { ended = 1; fprintf(stderr, "[dm] audio input: DM_MIC_FILE played to its end; silence from here\n"); }
        }
        usleep(2000);
    }
    return NULL;
}

int dm_mic_file_start(void) {
    pthread_mutex_lock(&g_file.lock);
    int ok = g_file.loaded;
    if (ok && !g_file.thread_up) {
        atomic_store_explicit(&g_file.running, 1, memory_order_release);
        if (pthread_create(&g_file.thread, NULL, file_feed, NULL)) { atomic_store_explicit(&g_file.running, 0, memory_order_release); ok = 0; }
        else { g_file.thread_up = 1; g_file.started_ns = now_ns(); fprintf(stderr, "[dm] audio input: DM_MIC_FILE plays from its start\n"); }
    }
    pthread_mutex_unlock(&g_file.lock);
    return ok ? 0 : -1;
}

void dm_mic_file_stop(void) {
    pthread_mutex_lock(&g_file.lock);
    if (g_file.thread_up) {
        atomic_store_explicit(&g_file.running, 0, memory_order_release);
        pthread_join(g_file.thread, NULL);
        g_file.thread_up = 0;
        // a test reads this line: a game that stops listening before the recording is over took what it heard for a song
        fprintf(stderr, "[dm] audio input: DM_MIC_FILE stopped by the game after %.2f s\n", (double)(now_ns() - g_file.started_ns) / 1e9);
    }
    pthread_mutex_unlock(&g_file.lock);
}

void dm_mic_file_close(void) { dm_mic_file_stop(); }   // the samples stay loaded for the next capture

void dm_mic_file_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    const char *path = file_path();
    const char *base = path ? strrchr(path, '/') : NULL;
    if (name) snprintf(name, 0x100, "file %s", base ? base + 1 : path ? path : "");
    if (uid) strlcpy(uid, "DM_MIC_FILE", 0x100);
    if (sample_rate) *sample_rate = DM_AUDIO_INPUT_RATE;
    if (channels) *channels = 1;
}
