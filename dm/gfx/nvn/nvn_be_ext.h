// The graphics backend functions a graphics API layer beside NVN calls: the guest Vulkan and OpenGL layers (dm/gfx/vk/hle_vk.m, dm/gfx/gl/hle_gl.m) draw their frames themselves
// and hand them to the backend's present path, paced by its vsync (docs/design/vulkan-backend.md, section 1.5: external producers). A part of the backend interface nvn_be.h, which
// includes it, without the NVN types of nvn_obj.h: an API layer has short type names of its own that nvn_obj.h also uses (hle_gl.m's tex_t, prog_t, sync_t). On Apple each function
// is a static inline one-line forward to the Metal backend's mtl_* function, as in nvn_be.h; elsewhere a prototype, which dm/gfx/nvn/nvn_null.c defines until the Vulkan backend does
// (dm/gfx/hostvk, step M7). Names: the mtl_* ones with be_ in front, except mtl_link_* (the display link), which are be_vsync_*, and mtl_device, which is be_native_device
#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(__APPLE__)
#include "nvn_mtl.h"   // mtl_set_origin_lower_left, mtl_present_wait, mtl_device, mtl_ext_tex, mtl_present_ext
int mtl_fps_target(void); int mtl_link_active(void); int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // declared in nvn_obj.h too, with the same types

static inline void be_set_origin_lower_left(int on) { mtl_set_origin_lower_left(on); }
static inline void be_present_wait(uint64_t ticket) { mtl_present_wait(ticket); }
static inline int be_fps_target(void) { return mtl_fps_target(); }
static inline int be_vsync_active(void) { return mtl_link_active(); }
static inline int be_vsync_wait(int vsyncs, uint64_t timeout_ns) { return mtl_link_wait_tick(vsyncs, timeout_ns); }
static inline void *be_native_device(void) { return mtl_device(); }
static inline uint64_t be_ext_tex(void *tex) { return mtl_ext_tex(tex); }
static inline uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]) { return mtl_present_ext(serial, period, ev, ev_val, crop); }

#else   // other hosts: the backend defines these (the same functions, in the same order, as the Apple block above)

void be_set_origin_lower_left(int on);
void be_present_wait(uint64_t ticket);
int be_fps_target(void);
int be_vsync_active(void);
int be_vsync_wait(int vsyncs, uint64_t timeout_ns);
void *be_native_device(void);
uint64_t be_ext_tex(void *tex);
uint64_t be_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]);

#endif
