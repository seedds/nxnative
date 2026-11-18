// t_vk_noperspective: the position from NVN vertex attribute 0 and, at location 3, the vertex's place in the window in 0..1 from its own NDC, as the translations of the
// game shaders that read the screen position do (Mario vs. Donkey Kong's LevelClear and BlackScreen models). The output carries no interpolation decoration: the chain's vertex
// shaders have none, only their fragment partners' inputs do (npw.frag)
#version 450
layout(location = 0) in vec4 p;
layout(location = 3) out vec4 s;
invariant gl_Position;
void main() {
    gl_Position = p;
    vec2 n = p.xy / p.w;
    s = vec4(n.x * 0.5 + 0.5, 1.0 - (n.y * 0.5 + 0.5), p.w, 1.0);
}
