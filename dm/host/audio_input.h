#ifndef DM_AUDIO_INPUT_H
#define DM_AUDIO_INPUT_H

#include <TargetConditionals.h>
#include <stdint.h>

typedef struct dm_audio_input_stream dm_audio_input_stream;

enum { DM_AUDIO_INPUT_PCM16 = 2, DM_AUDIO_INPUT_CHANNELS = 1, DM_AUDIO_INPUT_RATE = 48000 };

// Platform hooks. The iOS host intentionally keeps the existing no-input behavior.
#if TARGET_OS_OSX
int dm_audio_input_platform_device_available(void); // Checks hardware and known-denied permission without prompting.
int dm_audio_input_platform_prepare(void);     // Prepares capture only when launcher permission is granted; never prompts.
int dm_audio_input_platform_start(void);
void dm_audio_input_platform_stop(void);
void dm_audio_input_platform_close(void);
int dm_audio_input_platform_failed(void);
void dm_audio_input_platform_synchronize(void); // Wait for callbacks to leave before stream storage is freed.
void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels);
#else
static inline int dm_audio_input_platform_device_available(void) { return 0; }
static inline int dm_audio_input_platform_prepare(void) { return -1; }
static inline int dm_audio_input_platform_start(void) { return -1; }
static inline void dm_audio_input_platform_stop(void) { }
static inline void dm_audio_input_platform_close(void) { }
static inline int dm_audio_input_platform_failed(void) { return 1; }
static inline void dm_audio_input_platform_synchronize(void) { }
static inline void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    (void)name; (void)uid; (void)sample_rate; (void)channels;
}
#endif

int dm_audio_input_available(void);
void dm_audio_input_device_name(char out[0x100]);
dm_audio_input_stream *dm_audio_input_open(int format, int channels, int rate, int capacity_frames);
void dm_audio_input_close(dm_audio_input_stream *stream);
int dm_audio_input_start(dm_audio_input_stream *stream);
void dm_audio_input_stop(dm_audio_input_stream *stream);
void dm_audio_input_purge(dm_audio_input_stream *stream);
int dm_audio_input_state(const dm_audio_input_stream *stream);
int dm_audio_input_capacity(const dm_audio_input_stream *stream);
int dm_audio_input_filled(const dm_audio_input_stream *stream);
int dm_audio_input_free(const dm_audio_input_stream *stream);
int dm_audio_input_read(dm_audio_input_stream *stream, void *dst, int frames);
float dm_audio_input_gain(const dm_audio_input_stream *stream);
void dm_audio_input_set_gain(dm_audio_input_stream *stream, float gain);

// The platform callback supplies planar Float32 samples at the device sample rate.
// Conversion and fan-out use preallocated queues and do not take locks.
void dm_audio_input_process(const float *const *channels, int channel_count, uint32_t frames, double sample_rate);
void dm_audio_input_capture_failed(void);

#endif
