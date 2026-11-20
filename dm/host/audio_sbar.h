// The system renderer for AP_SYSTEM (audio_route.h): AVSampleBufferAudioRenderer + AVSampleBufferRenderSynchronizer, the one output path macOS spatializes itself (WWDC21 10265,
// nxnative issue #2650: the Sound menu's Spatial Audio, Off / Fixed / Head Tracked, with the listener's personalized profile, no entitlement). Multichannel float with an explicit layout
// (MPEG_5_1_A for the game's six) is what it offers the treatment for. It is a push model: the host's pump thread (hle_audio.c) pulls the DeviceSink queue at the
// synchronizer's pace and enqueues one CMSampleBuffer per 5 ms block at a running timestamp, keeping SB_LEAD of audio ahead of the clock.
// audio_spatial_mac.m implements it (the Mac host only, AVFoundation). hle_audio.c has weak stand-ins that make nothing, so the iOS build and the unit tests that
// include hle_audio.c without it have no system path (route_can leaves AR_CAN_SYSTEM out); t_audio_device defines DM_SBAR_EXTERNAL and mocks these
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct dm_sbar dm_sbar;
int dm_sbar_available(void);                                    // 1 when the system renderer can be made (the class exists)
dm_sbar *dm_sbar_create(int ch, double rate, char *why, size_t whyn);   // renderer + synchronizer + the float format with its layout (6: MPEG_5_1_A); NULL with a reason
int dm_sbar_enqueue(dm_sbar *s, const float *frames, int n);    // n interleaved frames at the running timestamp; 0 when enqueued
double dm_sbar_lead(dm_sbar *s);                                // seconds enqueued ahead of the synchronizer's current time
int dm_sbar_play(dm_sbar *s);                                   // start the clock at the first timestamp (once per pre-roll); 0 when it runs
void dm_sbar_stop(dm_sbar *s);                                  // clock stopped, the queue flushed, timestamps from 0 again
enum { SBAR_OK, SBAR_FAILED, SBAR_FLUSHED };
int dm_sbar_status(dm_sbar *s, char *why, size_t whyn);         // SBAR_FAILED (the renderer's error in why), SBAR_FLUSHED (it flushed itself, as on a device change: pre-roll again), else OK
void dm_sbar_destroy(dm_sbar *s);
int dm_sbar_describe(dm_sbar *s, int *ch, uint32_t *layout_tag, double *rate, int *is_float);   // the format description it enqueues with (tests)
void dm_sbar_test_set_time(dm_sbar *s, double seconds);   // tests: move the clock with its rate kept at 0 (as after playing that long), nothing played
