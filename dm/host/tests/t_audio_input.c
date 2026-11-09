#include "../audio_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int device_available = 1, prepare_result, start_result;
static unsigned prepares, starts, stops, closes;

int dm_audio_input_platform_device_available(void) { return device_available; }
int dm_audio_input_platform_prepare(void) { prepares++; return prepare_result; }
int dm_audio_input_platform_start(void) { starts++; return start_result; }
void dm_audio_input_platform_stop(void) { stops++; }
void dm_audio_input_platform_close(void) { closes++; }
int dm_audio_input_platform_failed(void) { return 0; }
void dm_audio_input_platform_synchronize(void) { }
void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    if (name) snprintf(name, 0x100, "synthetic input");
    if (uid) snprintf(uid, 0x100, "synthetic.uid");
    if (sample_rate) *sample_rate = 48000.0;
    if (channels) *channels = 1;
}

static void feed(const float *samples, uint32_t n, double rate) {
    const float *planes[] = { samples };
    dm_audio_input_process(planes, 1, n, rate);
}

static void test_fanout_and_polling(void) {
    float samples[12];
    for (int i = 0; i < 12; i++) samples[i] = i / 16.0f;
    dm_audio_input_stream *a = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 8);
    dm_audio_input_stream *b = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 8);
    assert(a && b && dm_audio_input_state(a) == 1 && dm_audio_input_capacity(a) == 8);
    assert(dm_audio_input_start(a) == 0 && dm_audio_input_start(b) == 0 && starts == 1);
    feed(samples, 12, 48000);
    assert(dm_audio_input_filled(a) == 8 && dm_audio_input_free(a) == 0);
    int16_t aa[8], bb[8];
    assert(dm_audio_input_read(a, aa, 8) == 8);
    assert(dm_audio_input_read(b, bb, 8) == 8);
    assert(memcmp(aa, bb, sizeof aa) == 0);
    for (int i = 0; i < 8; i++) assert(aa[i] == (int16_t)(samples[i] * 32768.0f));  // full queue drops newest frames
    dm_audio_input_purge(a);
    assert(dm_audio_input_filled(a) == 0);
    dm_audio_input_stop(a);
    assert(dm_audio_input_state(a) == 1);
    dm_audio_input_stop(b);
    assert(dm_audio_input_state(b) == 1 && stops == 1);
    dm_audio_input_close(a);
    dm_audio_input_close(b);
    assert(closes == 1);
}

static void test_conversion_and_gain(void) {
    const uint32_t input_frames = 44101;  // one second plus one source sample
    float *input = malloc(input_frames * sizeof *input);
    assert(input);
    for (uint32_t i = 0; i < input_frames; i++) input[i] = 0.25f;
    dm_audio_input_stream *s = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 48064);
    assert(s && dm_audio_input_start(s) == 0);
    dm_audio_input_set_gain(s, 2.0f);
    feed(input, input_frames, 44100.0);
    int converted = dm_audio_input_filled(s);
    assert(converted == 48001);  // one second plus one source sample becomes one second plus one target sample
    assert(dm_audio_input_gain(s) == 2.0f);
    int16_t *output = malloc(48001 * sizeof *output);
    assert(output && dm_audio_input_read(s, output, 48001) == 48001);
    for (int i = 0; i < 48001; i++) assert(output[i] == 16384);
    dm_audio_input_close(s);
    free(output); free(input);
}

static void test_microphone_gain_preserves_headroom(void) {
    assert(setenv("DM_MIC_GAIN", "4", 1) == 0);
    enum { FRAMES = 480 };
    float samples[FRAMES];
    const float amplitude = 0.05011872f;  // -26 dBFS peak
    for (int i = 0; i < FRAMES; i++) samples[i] = amplitude * sinf((float)(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0));
    dm_audio_input_stream *s = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, FRAMES);
    assert(s && dm_audio_input_start(s) == 0);
    feed(samples, FRAMES, 48000.0);
    int16_t output[FRAMES];
    assert(dm_audio_input_read(s, output, FRAMES) == FRAMES);
    double squares = 0.0; int peak = 0;
    for (int i = 0; i < FRAMES; i++) {
        int value = output[i]; int magnitude = value < 0 ? -value : value;
        if (magnitude > peak) peak = magnitude;
        squares += (double)value * value;
    }
    double peak_level = (double)peak / 32768.0;
    double rms_level = sqrt(squares / FRAMES) / 32768.0;
    assert(peak_level > 0.19 && peak_level < 0.21);  // -26 dBFS + 12 dB = about -14 dBFS peak
    assert(rms_level > 0.14 && rms_level < 0.15);
    dm_audio_input_close(s);

    assert(setenv("DM_MIC_GAIN", "8", 1) == 0);
    const float loud = 0.4f;
    dm_audio_input_stream *limited = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 8);
    assert(limited && dm_audio_input_start(limited) == 0);
    feed(&loud, 1, 48000.0);
    assert(dm_audio_input_read(limited, output, 1) == 1);
    assert(output[0] < INT16_MAX && output[0] > 32000);  // soft limiter stays in range without hard clipping
    dm_audio_input_close(limited);
    assert(setenv("DM_MIC_GAIN", "1", 1) == 0);
}

static void test_denial_open_failure_and_device_loss(void) {
    device_available = 0;
    assert(!dm_audio_input_available());
    assert(!dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 4800));
    device_available = 1;
    prepare_result = -1;
    unsigned before = prepares;
    assert(dm_audio_input_available());  // Enumeration does not request microphone permission.
    dm_audio_input_stream *denied = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 8);
    assert(denied && prepares == before);  // Opening only allocates a bounded queue.
    assert(dm_audio_input_start(denied) != 0 && prepares == before + 1);
    assert(dm_audio_input_state(denied) == 0);  // A denied first capture stays closed.
    dm_audio_input_close(denied);

    device_available = 1;
    prepare_result = 0;
    dm_audio_input_stream *s = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, 48000, 8);
    assert(s && dm_audio_input_start(s) == 0);
    float signal[2] = {0.5f, -0.5f};
    feed(signal, 2, 48000);
    dm_audio_input_capture_failed();
    assert(dm_audio_input_state(s) == 0 && dm_audio_input_filled(s) == 0);
    int16_t output[2] = {123, 123};
    assert(dm_audio_input_read(s, output, 2) == 0 && output[0] == 0 && output[1] == 0);
    dm_audio_input_stop(s);
    dm_audio_input_close(s);
}

int main(void) {
    assert(setenv("DM_MIC_GAIN", "1", 1) == 0);
    test_fanout_and_polling();
    test_conversion_and_gain();
    test_microphone_gain_preserves_headroom();
    test_denial_open_failure_and_device_loss();
    puts("audio input queues, conversion, fanout and closed failures: PASS");
    return 0;
}
