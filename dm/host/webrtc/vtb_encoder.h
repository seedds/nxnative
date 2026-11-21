// VideoToolbox H.264 Ultra-Low-Latency Hardware Encoder for Metal
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*vtb_nalu_callback_t)(void *user, const uint8_t *data, size_t size, bool is_keyframe, uint64_t pts_ns);

typedef struct vtb_encoder vtb_encoder_t;

vtb_encoder_t *vtb_encoder_create(int width, int height, int fps, int bitrate_kbps, vtb_nalu_callback_t cb, void *user);
void vtb_encoder_destroy(vtb_encoder_t *enc);

// Request immediate IDR keyframe (e.g. on new client connect or PLI)
void vtb_encoder_request_keyframe(vtb_encoder_t *enc);

// Reconfigure stream resolution dynamically (e.g. switch 480p <-> 720p)
bool vtb_encoder_reconfigure(vtb_encoder_t *enc, int width, int height, int bitrate_kbps);

// Encode a Metal texture (draws into internal CVPixelBuffer / IOSurface)
// mtl_tex: id<MTLTexture>
// mtl_dev: id<MTLDevice>
// mtl_cmd_queue: id<MTLCommandQueue>
int vtb_encoder_encode_metal(vtb_encoder_t *enc, void *mtl_tex, void *mtl_dev, void *mtl_cmd_queue, uint64_t pts_ns);

#ifdef __cplusplus
}
#endif
