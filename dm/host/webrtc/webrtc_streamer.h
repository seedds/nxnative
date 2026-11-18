// WebRTC Streamer C interface for nxnative
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inisialisasi WebRTC Streamer pada port yang ditentukan (default 8888)
// web_dir: path direktori static web assets (NULL = default "dm/host/webrtc/web")
int webrtc_streamer_init(int port, const char *web_dir);
void webrtc_streamer_stop(void);
int webrtc_streamer_active(void);
int webrtc_streamer_wants_video(void);
int webrtc_streamer_wants_audio(void);

// Mengirim frame video dari Metal texture
// mtl_texture: id<MTLTexture>
// mtl_dev: id<MTLDevice>
// mtl_cmd_queue: id<MTLCommandQueue>
void webrtc_streamer_send_metal_texture(void *mtl_texture, void *mtl_dev, void *mtl_cmd_queue, uint64_t pts_ns);

// Mengirim audio PCM 48kHz stereo (interleaved float atau int16)
void webrtc_streamer_send_pcm_float(const float *samples, size_t frames, int channels);
void webrtc_streamer_send_pcm_int16(const int16_t *samples, size_t frames, int channels);

// Status koneksi klien
int webrtc_streamer_client_count(void);

#ifdef __cplusplus
}
#endif
