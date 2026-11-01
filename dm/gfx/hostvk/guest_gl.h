// The guest OpenGL layer on the Vulkan backend (guest_gl.c; docs/design/vulkan-backend.md 8.2, step P9b): what its test reads of it (dm/gfx/tests/vk/t_vk_guest_gl.c).
// The game reaches the layer only through its EGL and GL entries in the import table; these counters say which of the layer's paths its calls took. Plain C, Windows and Linux.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t frames;             // eglSwapBuffers
    uint64_t submits;            // command buffer submissions on the layer's timeline (each frame's, and those a fence wait, glFinish or glReadPixels commits early)
    uint64_t early_commits;      // of those, made before the frame's end (glClientWaitSync on a fence of the frame being recorded, glFinish, glReadPixels, a stalled map)
    uint64_t passes, draws, pipelines;
    uint64_t uploads_pre;        // texture uploads recorded before the frame's commands (the texture was not read in this frame yet)
    uint64_t uploads_ordered;    // texture uploads recorded among them, in command order (the frame had already read the texture)
    uint64_t orphans;            // glBufferData / an invalidating map of a buffer the GPU may still read: new memory
    uint64_t copies_on_write;    // glBufferSubData into a buffer the GPU may still read: a copy, then the write
    uint64_t map_waits;          // a synchronized glMapBufferRange that waited for the GPU
    uint64_t clears_load;        // whole-framebuffer clears that became a pass's load operation
    uint64_t clears_rect;        // scissored clears (vkCmdClearAttachments)
    uint64_t clears_draw;        // masked clears (the layer's clear pipeline)
    uint64_t blits, mipmaps, readpixels;
    uint64_t varyings_relocated; // fragment inputs given the location of the vertex output of their name when a program linked
    uint64_t astc_decoded;       // ASTC levels decoded on the CPU (a device without ASTC)
} guest_gl_stats;
void guest_gl_stats_get(guest_gl_stats *s);
// Every Vulkan object of the layer released, after its last submission and present (a test, before the backend's shutdown; the game's eglTerminate keeps them, as hle_gl.m's)
void guest_gl_shutdown(void);
