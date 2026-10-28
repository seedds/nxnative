// Copyright (c) 2026 MacNativeGames contributors (github.com/ly05010419/MacNativeGames), MIT License; the license text is in dm/third_party/macnativegames/LICENSE.
// MetalFX frame interpolation, adapted from MacNativeGames' totk_mfx.h. See nvn_mfx.m.
#pragma once
#import <Metal/Metal.h>

/* Frame interpolation mode, fixed for the run: DM_MFX_INTERP=interp30 (MFX_30): the game renders 30 frames a second and MetalFX
   makes the frame between ("30+30"); with DM_FPS_TARGET=30 the display link runs at 60 Hz and shows the two pictures one tick
   each (nvn_mtl.m). MacNativeGames' other modes (every second refresh, uncapped) and its run-time switch are not wired here. */
#include "nvn_mfx_mode.h"
const char *mfx_mode_name(int mode);
// Per game frame, from the replay: the scene copy (the canvas copied before the first UI draw, in the canvas format),
// the UI layer (colour attachment 7 of the later canvas passes), the G-buffer depth and the camera block (the first 84 floats
// of the profile's camera_block).
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat format);
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h);
void mfx_note_scene(void);
void mfx_note_depth(id<MTLTexture> depth);
void mfx_note_camera(const float *cam, int n);
id<MTLResidencySet> mfx_residency(void);   // our textures; the queue adds it once
// At the game's present. prepare: checks this frame's inputs and returns the output slot of the interpolated frame
// between the previous and this frame, or -1 (first frame, missing input, scene cut; state already advanced). frame_s:
// the pacing period (two display refreshes; 0 in unpaced measurement runs). encode: motion vectors and the interpolation into cb (Metal 4, own command buffer, GPU order after this
// frame). busy: bit s set = output slot s is still named by a present not yet committed (the present queue holds up to 8);
// prepare picks another, and when every slot is busy it interpolates nothing this frame. skip: instead of prepare, when no
// command buffer is free: this frame's inputs are dropped and the history reset.
int mfx_frame_prepare(double frame_s, unsigned busy);
void mfx_frame_skip(void);
// Keep the returned resources alive until this command buffer's GPU completion.
// Cached interpolators drop their public texture bindings after encoding.
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int slot, id<MTL4CounterHeap> heap, NSUInteger stamp);   // heap: timestamps at stamp / stamp+1 around MetalFX (may be nil)
void mfx_note_gpu(double ms);   // GPU time of one interpolation, for the statistics line
void mfx_history_reset(void);   // before mfx_frame_prepare: this frame's scene does not continue the previous one (another canvas)
id<MTLTexture> mfx_output(int slot);   // RGBA8Unorm view of the sRGB output, like the swapchain textures
void mfx_stats(char *out, size_t n);
void mfx_mv_log(void);
int mfx_dump_due(void);
void mfx_dump(long frame, id<MTLTexture> swap_prev, id<MTLTexture> swap_cur);   // DM_MFX_DUMP=<dir>: scenes, depth, motion and output of three frames while the camera turns (diagnostic)   // DM_MFX_MVLOG=<n>: motion texture median every n interpolated frames (diagnostic)
