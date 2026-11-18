// Opus Audio Encoder Helper
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*opus_packet_callback_t)(void *user, const uint8_t *packet, size_t size, uint64_t pts_ns);

typedef struct opus_helper opus_helper_t;

opus_helper_t *opus_helper_create(int sample_rate, int channels, int bitrate_kbps, opus_packet_callback_t cb, void *user);
void opus_helper_destroy(opus_helper_t *h);

// Feed interleaved float PCM (-1.0f .. 1.0f)
void opus_helper_feed_float(opus_helper_t *h, const float *pcm, size_t frames, uint64_t pts_ns);

// Feed interleaved int16 PCM (-32768 .. 32767)
void opus_helper_feed_int16(opus_helper_t *h, const int16_t *pcm, size_t frames, uint64_t pts_ns);

#ifdef __cplusplus
}
#endif
