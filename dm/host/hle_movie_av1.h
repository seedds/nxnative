// AV1 decoding for the Switch 2 decoder API (hle_codec_video.c): VideoToolbox where the chip decodes AV1, dav1d elsewhere; see hle_movie_av1.c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "vpx/vpx_image.h"

typedef struct mav1 mav1;
mav1 *mav1_new(void);
void mav1_free(mav1 *a);
void mav1_drop(mav1 *a);   // a flush / seek: pictures not taken are discarded, and decoding starts again at the next sequence header and key frame
int mav1_decode(mav1 *a, const uint8_t *p, size_t n, int64_t ts);   // one temporal unit (low-overhead OBUs, as WebM keeps them); 0 = taken (or nothing to decode), -1 = error
vpx_image_t *mav1_take(mav1 *a, int64_t *ts);   // the next picture in output order, an owned 8-bit I420 copy (vpx_img_free), and its time; NULL when there is none
const char *mav1_backend(const mav1 *a);         // "VideoToolbox" or "dav1d"
