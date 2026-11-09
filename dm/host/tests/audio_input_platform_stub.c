// No-input platform for the tests that build hle_audio.c: they exercise the audio output path, and the input
// streams answer as they do on a Mac with no microphone (the real backend is ../audio_input_macos.m).
#include "../audio_input.h"

#if TARGET_OS_OSX
int dm_audio_input_platform_device_available(void) { return 0; }
int dm_audio_input_platform_prepare(void) { return -1; }
int dm_audio_input_platform_start(void) { return -1; }
void dm_audio_input_platform_stop(void) { }
void dm_audio_input_platform_close(void) { }
int dm_audio_input_platform_failed(void) { return 1; }
void dm_audio_input_platform_synchronize(void) { }
void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    (void)name; (void)uid; (void)sample_rate; (void)channels;
}
#else
typedef int dm_audio_input_platform_stub_unused; // the header already inlines these hooks
#endif
