// AAC decoding for the movie decoder (hle_movie.c) where it is not Apple's AudioToolbox converter (which hle_movie.c calls itself): Media Foundation's AAC transform on Windows
// (hle_movie_win32.c). Raw AAC frames (as an MP4 demuxer gives them, 1024 samples each) in, interleaved PCM16 at the stream's rate and channel count out; the decoder runs on
// the movie decoder's own host thread (hle_movie.c md_worker), which makes, uses and frees it
#pragma once
#include <stdint.h>

typedef struct maac maac;
// asc: the AudioSpecificConfig (Configure's csd-0; NULL or empty: AAC-LC made from *ch and *rate). *ch and *rate: the MediaData's channel count and rate (0 or less: the
// configuration's), and on return what the PCM has. NULL when there is no decoder: the reason was logged once as a [RED] line, the caller outputs silence
maac *maac_new(const uint8_t *asc, int asc_n, int *ch, int *rate);
// one raw frame; the PCM frames (samples per channel) written to pcm, at most cap_frames: 0 when it gave none yet, -1 on an error (logged once)
int maac_decode(maac *m, const uint8_t *p, int n, int16_t *pcm, int cap_frames);
void maac_free(maac *m);
