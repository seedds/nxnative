// The Metal source of the BC1/2/3 → ASTC 4x4 transcoder: the text of bc2astc_body.inc (stringified) plus the kernel `bca`. Include in one place per program; the string is nvn_bca_msl.
// Kernel: one thread per BC block. buffer(0) the BC blocks (bytes), buffer(1) BcaP, buffer(2) the ASTC blocks (written as four words per block, so off_out and pitch_out are multiples of 16).
#pragma once
#define BCA_BODY(...) #__VA_ARGS__
static const char nvn_bca_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"typedef uint u32; typedef int s32; typedef long s64; typedef ulong u64; typedef uchar u8;\n"
"#define BCA_FN inline\n#define BCA_T thread\n#define BCA_K constant\n"
#include "bc2astc_body.inc"
"\nstruct BcaP { uint off_in, pitch_in, off_out, pitch_out, nbx, nby, kind; };\n"   // pitch_in: bytes per BC block row; kind: 1 BC1, 2 BC2, 3 BC3
"kernel void bca(device const uchar *src [[buffer(0)]], device const BcaP &p [[buffer(1)]], device uint *dst [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p.nbx || g.y >= p.nby) return;\n"
"  uint bb = p.kind == 1 ? 8 : 16; device const uchar *b = src + p.off_in + g.y * p.pitch_in + g.x * bb;\n"
"  u32 w[4], o[4];\n"
"  for (uint i = 0; i < 4; i++) w[i] = i * 4 < bb ? (u32(b[i * 4]) | (u32(b[i * 4 + 1]) << 8) | (u32(b[i * 4 + 2]) << 16) | (u32(b[i * 4 + 3]) << 24)) : 0;\n"
"  bca_transcode(w, p.kind, o);\n"
"  device uint *d = dst + (p.off_out + g.y * p.pitch_out + g.x * 16) / 4;\n"
"  for (uint i = 0; i < 4; i++) d[i] = o[i]; }\n";
#undef BCA_BODY
