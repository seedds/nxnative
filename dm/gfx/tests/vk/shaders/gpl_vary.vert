// t_vk_gpl_identity, t_vk_prewarm: the position and a colour from NVN vertex attributes 0 and 1 (an invariant position, as the chain's vertex shaders have it), the colour smooth
// with some arithmetic on it, a hashed value flat and one without perspective: interpolation of each kind across the libraries' interface
#version 450
layout(location = 0) in vec4 p;
layout(location = 1) in vec4 c;
layout(location = 0) out vec4 col;
layout(location = 1) flat out uint id;
layout(location = 2) noperspective out vec2 uv;
invariant gl_Position;
void main() {
    gl_Position = p;
    col = c * 0.75 + vec4(0.125) * sin(p.xyxy * vec4(3.1, 2.3, 1.7, 0.9));
    id = uint(gl_VertexIndex + 7 * gl_InstanceIndex) * 2654435761u;
    uv = p.xy * 0.5 + 0.5;
}
