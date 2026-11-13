// A recording as the microphone (audio_input_file.c) and the capture dump (audio_input.c): a 441 Hz tone written as a stereo 44.1 kHz WAV
// is played to an input stream by DM_MIC_FILE in real time (its channels averaged, resampled to 48 kHz), then silence; DM_MIC_DUMP writes
// what the stream was handed, sample for sample; and that dump, played back as a .pcm file in a fresh process, gives the same samples again.
#include "../audio_input.h"
#include "../audio_input_file.h"
#include <assert.h>
#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// audio_input.c's platform hooks: the file alone, as audio_input_mac.m routes them while DM_MIC_FILE is set
int dm_audio_input_platform_device_available(void) { return 1; }
int dm_audio_input_platform_prepare(void) { return dm_mic_file_prepare(); }
int dm_audio_input_platform_start(void) { return dm_mic_file_start(); }
void dm_audio_input_platform_close(void) { dm_mic_file_close(); }
int dm_audio_input_platform_failed(void) { return 0; }
int dm_audio_input_platform_route_changed(void) { return 0; }
void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) { dm_mic_file_info(name, uid, sample_rate, channels); }

enum { RATE = 48000, CAP = 96000 };

static void write_wav(const char *path) {   // 0.4 s, 16-bit stereo at 44.1 kHz: 441 Hz at half scale on both channels
    enum { WR = 44100, N = WR * 4 / 10 };
    FILE *f = fopen(path, "wb"); assert(f);
    uint32_t data = N * 4, riff = 36 + data, fmt = 16, rate = WR, bytes = WR * 4; uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&bytes, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < N; i++) { int16_t v = (int16_t)(16384.0 * sin(2.0 * M_PI * 441.0 * i / WR)); fwrite(&v, 2, 1, f); fwrite(&v, 2, 1, f); }
    fclose(f);
}

// What a stream reads in `seconds` of the file playing: one read at the end from a queue that holds it all, so nothing depends on polling
static int capture(double seconds, int16_t *out) {
    dm_audio_input_stream *s = dm_audio_input_open(DM_AUDIO_INPUT_PCM16, 1, RATE, CAP);
    assert(s && dm_audio_input_start(s) == 0);
    usleep((useconds_t)(seconds * 1e6));
    int filled = dm_audio_input_filled(s);
    int n = dm_audio_input_read(s, out, filled);
    assert(n == filled);
    dm_audio_input_stop(s);
    dm_audio_input_close(s);
    return n;
}

static long file_frames(const char *path, int16_t *out, long max) {
    FILE *f = fopen(path, "rb"); assert(f);
    long n = (long)fread(out, sizeof(int16_t), (size_t)max, f);
    fclose(f);
    return n;
}

static int replay_raw(const char *dump) {   // the fresh process: the dump as DM_MIC_FILE gives its own samples back
    static int16_t want[CAP], got[CAP];
    long have = file_frames(dump, want, CAP);
    assert(setenv("DM_MIC_FILE", dump, 1) == 0 && unsetenv("DM_MIC_DUMP") == 0 && setenv("DM_MIC_GAIN", "1", 1) == 0);
    int n = capture(0.3, got);
    assert(n >= RATE / 5 && n <= have);
    assert(!memcmp(got, want, (size_t)n * sizeof(int16_t)));
    return 0;
}

int main(int argc, char **argv) {
    assert(setenv("DM_MIC", "1", 1) == 0);   // the opt-in every input stream needs
    if (argc == 3 && !strcmp(argv[1], "raw")) return replay_raw(argv[2]);
    char dir[1024]; const char *tmp = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/t_audio_input_file.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(mkdtemp(dir));
    char wav[1100], dump[1100];
    snprintf(wav, sizeof wav, "%s/tone.wav", dir); snprintf(dump, sizeof dump, "%s/dump.pcm", dir);
    write_wav(wav);
    assert(setenv("DM_MIC_FILE", wav, 1) == 0 && setenv("DM_MIC_DUMP", dump, 1) == 0 && setenv("DM_MIC_GAIN", "1", 1) == 0);
    assert(dm_mic_file_selected());

    static int16_t got[CAP], dumped[CAP];
    int n = capture(0.75, got);
    assert(n >= RATE * 55 / 100 && n <= RATE);   // real time: about 0.75 s of samples, the file's 0.4 s and the silence after it

    // the tone: 441 Hz for 0.35 s is 308 zero crossings, half scale is an rms of 0.354
    int crossings = 0; double sum = 0;
    int tone = RATE * 35 / 100;
    for (int i = 1; i < tone; i++) { crossings += (got[i - 1] < 0) != (got[i] < 0); sum += (double)got[i] * got[i]; }
    double rms = sqrt(sum / tone) / 32768.0;
    assert(crossings >= 304 && crossings <= 312);
    assert(rms > 0.34 && rms < 0.37);
    for (int i = RATE * 45 / 100; i < n; i++) assert(got[i] == 0);   // past the file's end: silence

    // the dump: every sample the stream was handed, the same values (gain 1, below the limiter)
    long have = file_frames(dump, dumped, CAP);
    assert(have >= n);
    assert(!memcmp(got, dumped, (size_t)n * sizeof(int16_t)));

    pid_t pid; int status = 0;
    char *args[] = { argv[0], "raw", dump, NULL };
    assert(posix_spawn(&pid, argv[0], NULL, NULL, args, environ) == 0 && waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    unlink(wav); unlink(dump); rmdir(dir);
    printf("a recording as the microphone: %d crossings in 0.35 s, rms %.3f, then silence; the dump and its replay match: PASS\n", crossings, rms);
    return 0;
}
