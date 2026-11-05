// Default-device microphone capture for Mac and iPhone. Callbacks deliver mono
// float PCM at the requested rate on a private serial dispatch queue, never on
// the hardware render thread. Stop/close drain callbacks before returning.
#pragma once
#include <stdint.h>
typedef struct dm_capture dm_capture;
typedef void (*dm_capture_samples)(void *context, const float *pcm, uint32_t frames);
dm_capture *dm_capture_open(uint32_t rate, dm_capture_samples samples, void *context);
int dm_capture_start(dm_capture *capture); // asks OS permission; failure is not a silent capture device
void dm_capture_stop(dm_capture *capture);
void dm_capture_close(dm_capture *capture);
void dm_capture_suspend(int suspended);
void dm_capture_reset(void);
int dm_capture_audio_session(void); // iOS: Playback or PlayAndRecord according to live input streams
