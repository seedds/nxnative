/* The system devices behind output modes 1-2 (DM_AM_* in hle.h), an Objective-C file because both are only
 * reachable through AVAudioConverter / AudioUnit properties. It stays separate from hle_audio.c so the host's
 * audio unit tests can keep including hle_audio.c directly and run with these devices mocked or absent.
 * role[6]: which interleaved slot carries each role, in the fixed order front-left, front-right, centre, LFE,
 *           back-left, back-right (the order hle_audio.c's own fold assumes).
 * tag:      0-3 declare that order as MPEG_5_1_A/B/C/D; -1 = the order has no standard tag and is declared as a
 *           list of channel descriptions instead (the devices follow the roles the descriptions carry).
 */
#ifndef DM_AUDIO_SYS_H
#define DM_AUDIO_SYS_H
#include <stdint.h>

// Mode 1: AVAudioConverter with an explicit downmix=YES. The converter's default downmix=NO picks the front
// pair by role at gain 1.0 and drops the centre - dialogue - entirely (this is the mechanism that once
// silenced voices); YES mixes the channels instead, and a channel map set on the converter is ignored in
// that state. Input and output are 16-bit interleaved, at the rate given here.
void *dm_conv_open(const int *role, int tag, int rate);
int   dm_conv_run(void *h, const int16_t *in, unsigned frames, int16_t *out);   // frames really produced; 0 = nothing this time (the caller folds the block with its own coefficients)
void  dm_conv_close(void *h);

// Mode 2: one spatial-mixer ('3dem') bed over the six channels, with kSpatializationAlgorithm_UseOutputType
// (the value AVFoundation calls Auto): the mixer itself picks the highest-quality algorithm for the output
// hardware it renders to - an HRTF bed on headphones, panning on speakers - so the player never picks between
// the two and nothing watches for headphones. Output is stereo either way. The mixer's source mode must be
// set to an ambience bed explicitly: its default spatializes mono only, which leaves a multi-channel bed
// unspatialized. slice: the fixed frame count of one render, fixed for the life of the instance - a later
// render asking for a different length produces nothing (the caller banks whole slices).
void *dm_ausm_open(const int *role, int tag, unsigned slice);
int   dm_ausm_run(void *h, const int16_t *in6, unsigned frames, int16_t *out2);
void  dm_ausm_close(void *h);

const char *dm_arm_last_err(void);   // why the last open failed, for the log
#endif
