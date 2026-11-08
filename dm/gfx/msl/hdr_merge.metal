// HDR output's present copy (nvn_mtl.m hdr_merge): the swapchain's bytes into the ring slot as they are (pic), and the side texture's excess into the slot's highlights texture (hx)
// where the picture's luminance, decoded from sRGB, is still what the tone mapping wrote (within 1.2 % + 5 %, fading out over as much again). scale: the tone mapping target's
// share of the side texture
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
struct M { float2 scale; float2 pad; };
struct O { float4 pic [[color(0)]]; float4 hx [[color(1)]]; };
fragment O fs(V in [[stage_in]], texture2d<float> pic [[texture(0)]], texture2d<float> side [[texture(1)]], sampler s [[sampler(0)]], constant M &m [[buffer(0)]]) {
    float4 v = pic.read(uint2(in.p.xy));
    float3 c = select(pow((v.rgb + 0.055) / 1.055, float3(2.4)), v.rgb / 12.92, v.rgb <= 0.04045);
    float4 g = side.sample(s, in.uv * m.scale);
    float tol = 0.012 + 0.05 * g.a;
    O o; o.pic = v; o.hx = float4(g.rgb * (1 - smoothstep(tol, 2 * tol, abs(dot(c, float3(0.2989, 0.587, 0.1145)) - g.a))), 0);
    return o;
}
