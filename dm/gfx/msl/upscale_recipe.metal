// Profile-declared sqrt(linear) input and native unsigned-float output.
#include <metal_stdlib>
using namespace metal;
kernel void square_decode(texture2d<float, access::read> src [[texture(0)]], texture2d<half, access::write> dst [[texture(1)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= dst.get_width() || p.y >= dst.get_height()) return;
    float3 c = src.read(p).rgb; dst.write(half4(half3(c * c), 1), p);
}
kernel void native_encode(texture2d<half, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= dst.get_width() || p.y >= dst.get_height()) return;
    dst.write(float4(src.read(p)), p);
}
