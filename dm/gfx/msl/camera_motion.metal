// Camera-only current-to-previous pixel motion from the depth that rendered the current scene.
// Explicit rows avoid host / shader packing ambiguity. Both transforms use the depth
// convention of the actual depth texture; asymmetric and jittered projections work too.
// Object motion is not represented.
#include <metal_stdlib>
using namespace metal;
struct CameraMotionUniform {
    float4 clip_to_world[4]; // inverse of the current Metal [0,1]-depth world-to-clip matrix, explicit rows
    float4 previous_world_to_clip[4]; // previous matrix in the same clip convention, before presentation scaling
};
kernel void camera_motion(texture2d<float, access::read> depth [[texture(0)]],
                          texture2d<float, access::write> motion [[texture(1)]],
                          constant CameraMotionUniform &p [[buffer(0)]],
                          uint2 pixel [[thread_position_in_grid]]) {
    uint2 extent(depth.get_width(), depth.get_height());
    if (any(pixel >= extent)) return;
    float2 uv = (float2(pixel) + 0.5f) / float2(extent);
    float4 clip(uv * float2(2, -2) + float2(-1, 1), depth.read(pixel).x, 1);
    float4 world(dot(p.clip_to_world[0], clip), dot(p.clip_to_world[1], clip),
                 dot(p.clip_to_world[2], clip), dot(p.clip_to_world[3], clip));
    float4 old_clip(dot(p.previous_world_to_clip[0], world), dot(p.previous_world_to_clip[1], world),
                    dot(p.previous_world_to_clip[2], world), dot(p.previous_world_to_clip[3], world));
    float2 delta(0);
    if (old_clip.w > 1e-6f) {
        float2 old_uv = old_clip.xy / old_clip.w * float2(0.5f, -0.5f) + 0.5f;
        delta = (old_uv - uv) * float2(extent);
        if (!all(isfinite(delta))) delta = 0;
    }
    motion.write(float4(delta, 0, 0), pixel);
}
