// The stereo tap of the Metal backend (nvn_mtl.m): each present's picture and the depth of its scene, for a consumer that draws the picture once for each eye
// (dm/host/immersive_mac.m, a Vision Pro through a RemoteImmersiveSpace). Off until mtl_stereo_enable(1); costs two copies and a small scene-reference dispatch per present while on.
#pragma once
#include <stdint.h>
// color: an id<MTLTexture>, BGRA8Unorm_sRGB, the picture upright in its shape (what the screen shows, without bars). depth: an id<MTLTexture> of the game's depth format, or NULL when
// the frame has none (a 2D game, a Vulkan or GL game); the scene is in depth_rect (x, y, w, h, normalized) of it, bottom-up when depth_flip, and its far plane is 0 when depth_rev
// (else 1). base: an id<MTLTexture> like color, the same picture before the game drew its overlays (a HUD) over it, or NULL when the frame has none: a pixel where the two
// differ is an overlay's. The textures stay as they are until mtl_stereo_release
// reference: a small luminance image of the scene colour drawn beside depth, in the
// depth texture's row order. The eye pass aligns it with color after postprocessing:
// the game's composite can flip an offscreen scene independently of the window origin.
typedef struct { void *color, *depth, *base; float depth_rect[4]; int depth_rev, depth_flip; uint64_t seq; int c, d; void *reference; } mtl_stereo_frame;
void mtl_stereo_enable(int on);
// The newest finished frame, pinned; 0 = none yet. seq changes with each new frame. Any thread
int mtl_stereo_acquire(mtl_stereo_frame *f);
void mtl_stereo_release(const mtl_stereo_frame *f);
