// Mac spatial audio through the system (audio_sys_mac.m): a six-channel output as 5.1 to AVSampleBufferAudioRenderer, fed from the output's queue
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef void (*dm_sys_pull_fn)(void *ctx, int16_t *dst, uint32_t frames);   // fill frames six-channel int16 frames (interleaved L R C LFE Ls Rs)
void *dm_sys_audio_create(int id, dm_sys_pull_fn pull, void *ctx, int margin_ms, char *err, size_t errn);   // NULL on failure, err says why
int dm_sys_audio_start(void *h, double *delay_ms);   // 0, or -1 when the renderer has failed (make a new one); delay_ms: the output device's reported delay
void dm_sys_audio_stop(void *h);                     // returns with the feeder idle
uint64_t dm_sys_audio_late(void *h);
void dm_sys_audio_post_flush(void *h);               // tests: post the system's "flushed automatically" notification for this renderer
void dm_sys_audio_hold_clock(void *h);               // tests: stop the clock as the system does for a while (the feeder drops at real time, for at most 2 s)                 // times the feed fell behind the clock + device delay
void dm_sys_audio_dispose(void *h);
