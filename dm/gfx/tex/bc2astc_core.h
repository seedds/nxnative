// BC1 / BC2 / BC3 block → ASTC 4x4 LDR block (16 bytes), integer only. Public C interface: nvn_bc2astc.h. See dm/gfx/tex/README.md ("Devices without BC support") for the layouts and bit budgets.
// The code is in bc2astc_body.inc, which is written in the subset of C that Metal Shading Language also accepts; bc2astc_msl.h turns the same text into the GPU kernel (nvn_mtl.m, tests).
// The C build is the reference and the GPU output must match it bit for bit (dm/gfx/tests/t_bc2astc_gpu.m).
// Rules for editing the body: no floats, no static state, no pointers except the BCA_T arrays, fixed loop counts, 64-bit integers only where they already are, no preprocessor lines or string literals.
#pragma once
#include <stdint.h>
typedef uint32_t u32; typedef int32_t s32; typedef int64_t s64; typedef uint64_t u64; typedef uint8_t u8;
#define BCA_FN static inline   // MSL: inline
#define BCA_T                  // MSL: thread
#define BCA_K static const     // MSL: constant

#define BCA_BODY(...) __VA_ARGS__
#include "bc2astc_body.inc"
#undef BCA_BODY
