#include "opus_encoder_helper.h"
#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

#define OPUS_FRAME_SIZE 480 // 10ms at 48kHz stereo
#define OPUS_MAX_PACKET 1276

struct opus_helper {
    OpusEncoder *enc;
    int sample_rate;
    int channels;
    opus_packet_callback_t callback;
    void *user_ctx;

    // FIFO buffer for int16 stereo samples (480 stereo = 960 int16)
    int16_t fifo[OPUS_FRAME_SIZE * 2 * 4];
    size_t fifo_samples;
};

opus_helper_t *opus_helper_create(int sample_rate, int channels, int bitrate_kbps, opus_packet_callback_t cb, void *user) {
    if (!cb || channels != 2) return NULL;
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK || !enc) return NULL;

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate_kbps > 0 ? bitrate_kbps * 1000 : 128000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(enc, OPUS_SET_FORCE_CHANNELS(2));

    opus_helper_t *h = (opus_helper_t *)calloc(1, sizeof(opus_helper_t));
    h->enc = enc;
    h->sample_rate = sample_rate;
    h->channels = channels;
    h->callback = cb;
    h->user_ctx = user;
    h->fifo_samples = 0;
    return h;
}

void opus_helper_destroy(opus_helper_t *h) {
    if (!h) return;
    if (h->enc) opus_encoder_destroy(h->enc);
    free(h);
}

void opus_helper_feed_int16(opus_helper_t *h, const int16_t *pcm, size_t frames, uint64_t pts_ns) {
    if (!h || !pcm || frames == 0) return;

    size_t in_samples = frames * 2;
    size_t in_offset = 0;
    uint8_t out_packet[OPUS_MAX_PACKET];

    while (in_offset < in_samples) {
        size_t needed = (OPUS_FRAME_SIZE * 2) - h->fifo_samples;
        size_t avail = in_samples - in_offset;
        size_t to_copy = (avail < needed) ? avail : needed;

        memcpy(&h->fifo[h->fifo_samples], &pcm[in_offset], to_copy * sizeof(int16_t));
        h->fifo_samples += to_copy;
        in_offset += to_copy;

        if (h->fifo_samples == OPUS_FRAME_SIZE * 2) {
            opus_int32 len = opus_encode(h->enc, h->fifo, OPUS_FRAME_SIZE, out_packet, sizeof(out_packet));
            if (len > 0) {
                h->callback(h->user_ctx, out_packet, (size_t)len, pts_ns);
            }
            h->fifo_samples = 0;
        }
    }
}

void opus_helper_feed_float(opus_helper_t *h, const float *pcm, size_t frames, uint64_t pts_ns) {
    if (!h || !pcm || frames == 0) return;
    int16_t sbuf[1024 * 2];
    size_t chunk = 1024;
    for (size_t i = 0; i < frames; i += chunk) {
        size_t cur = (frames - i < chunk) ? frames - i : chunk;
        for (size_t j = 0; j < cur * 2; j++) {
            float v = pcm[(i * 2) + j];
            if (v > 1.0f) v = 1.0f;
            else if (v < -1.0f) v = -1.0f;
            sbuf[j] = (int16_t)(v * 32767.0f);
        }
        opus_helper_feed_int16(h, sbuf, cur, pts_ns);
    }
}
