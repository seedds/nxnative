// t_vk_gpl_identity, t_vk_prewarm: a colour from the interpolated inputs and the fragment position through transcendental and fused arithmetic, which an optimizing link could
// reorder
#version 450
layout(location = 0) in vec4 col;
layout(location = 1) flat in uint id;
layout(location = 2) noperspective in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    vec2 f = gl_FragCoord.xy * 0.03125;
    float w = exp2(-dot(f, f) * 0.25) + fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453) * 0.25;
    o = vec4(col.rgb * w, clamp(col.a * 0.5 + float(id & 255u) / 512.0, 0.0, 1.0));
}
