// Explicit NVN D24S8 readback from Metal D32FS8. NVN Z24S8 is depth in
// bits 31:8 and stencil in 7:0; copying Metal's eight-byte texel is invalid.
// Independent implementation of the storage format, no community patch values.
#include <metal_stdlib>
using namespace metal;
struct Params { ulong dst; uint x, y, width, height, row, level; };
// Exact round-to-nearest-even of a float32 depth times (2^24-1). The integer
// product avoids a float multiply rounding twice around a D24 half-way point.
static uint depth24(float z) {
    if (!(z > 0)) return 0;
    if (z >= 1) return 0xffffff;
    uint b = as_type<uint>(z), exponent = (b >> 23) & 255;
    if (exponent < 102) return 0;
    uint shift = 150 - exponent;
    ulong n = ulong((b & 0x7fffff) | 0x800000) * 0xfffffful;
    uint q = uint(n >> shift);
    ulong rem = n & ((1ul << shift) - 1), midpoint = 1ul << (shift - 1);
    return q + uint(rem > midpoint || (rem == midpoint && (q & 1)));
}
kernel void pack_ds(constant Params &p [[buffer(0)]], depth2d<float> z [[texture(0)]],
                    texture2d<uint, access::read> s [[texture(1)]], uint2 i [[thread_position_in_grid]]) {
    if (i.x >= p.width || i.y >= p.height) return;
    uint2 pos = i + uint2(p.x, p.y);
    device uint *out = reinterpret_cast<device uint *>(p.dst + ulong(i.y) * p.row);
    out[i.x] = (depth24(z.read(pos, p.level)) << 8) | (s.read(pos).r & 255);
}
