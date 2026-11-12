// AV1 decoding on VideoToolbox or dav1d (hle_movie_av1.c), for the nn::codec decoders of the Switch 2's SDK (hle_codec_video.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mav1 mav1;
// What one decode gave: an NV12 picture of w x h in the decoder's memory, valid until the next call, its matrix as vpx_color_space_t's values (BT.601 = 1, BT.709 = 2, BT.2020 = 5),
// and the time its temporal unit came with: dav1d decodes several frames at once, so a picture can come out calls after its unit went in
typedef struct { const uint8_t *y, *uv; size_t ystride, uvstride; int w, h, matrix, full_range; int64_t ts; } mav1_picture;

mav1 *mav1_new(void);
void mav1_free(mav1 *a);
void mav1_reset(mav1 *a);   // a new stream starts at the next packet with a sequence header
// One temporal unit (OBUs with their sizes) and its time. 1 and *out set: a picture, the oldest not yet given; 0: nothing to show now (no sequence header seen yet, a unit
// without a shown frame, or dav1d still decodes it); -1: the decoder refused it
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out);
// More pictures after mav1_decode, one a call until it returns 0. drain 0: those the unit's last part gives (dav1d gave a picture before it had taken the whole unit).
// drain 1: also the pictures of the frames dav1d still decodes, waiting for each: at the end of the stream, or when no unit follows this one for now. Once such a call has
// given a picture, the calls go on until one returns 0 before the next mav1_decode (dav1d's drain cannot be left halfway: dav1d_step)
int mav1_next(mav1 *a, int drain, mav1_picture *out);
const char *mav1_backend(const mav1 *a);   // "VideoToolbox" (its hardware decoder: Apple M3 and A17 Pro on) or "dav1d"
// Exposed for its test: fills the 4 + n bytes of an av1C record from a sequence header OBU (with its header and size); 0 when the OBU cannot be read
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h);
