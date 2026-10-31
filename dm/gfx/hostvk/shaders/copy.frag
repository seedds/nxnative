// Draw-based texture copies (docs/design/vulkan-backend.md 2.7; step P8; vk_pipeline.c hvk_copy_draw), for what vkCmdBlitImage cannot do, as the Metal backend's copy_t2t_draw:
// KIND 0 a colour copy (float, unorm and sRGB formats) that converts the format, scales (nearest or bilinear, the sampler's) or mirrors; KIND 1 a depth copy between depth formats
// (the depth written from the fragment shader, nearest); KIND 2 the same with the stencil written too (VK_EXT_shader_stencil_export, from a stencil view of the source).
// Push constants: src = the source origin in texels (its far edge when mirrored) and source texels per destination pixel (negative when mirrored); dst = the destination origin
// and the source level's size; layer = the source layer. gen_spirv.sh compiles it for KIND 0-2 (copy_frag.h)
#version 450
#if KIND == 2
#extension GL_ARB_shader_stencil_export : require
#endif
layout(push_constant) uniform P { vec4 src; vec4 dst; float layer; } p;
layout(set = 0, binding = 0) uniform sampler2DArray t;
#if KIND == 0
layout(location = 0) out vec4 o;
void main() {
    vec2 st = p.src.xy + (gl_FragCoord.xy - p.dst.xy) * p.src.zw;
    o = textureLod(t, vec3(st / p.dst.zw, p.layer), 0.0);
}
#else
#if KIND == 2
layout(set = 0, binding = 1) uniform usampler2DArray ts;
#endif
void main() {
    vec2 st = p.src.xy + (gl_FragCoord.xy - p.dst.xy) * p.src.zw;
    ivec3 c = ivec3(ivec2(floor(st)), int(p.layer));
    gl_FragDepth = texelFetch(t, c, 0).r;
#if KIND == 2
    gl_FragStencilRefARB = int(texelFetch(ts, c, 0).r);
#endif
}
#endif
