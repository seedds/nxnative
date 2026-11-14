#version 320 es
// t_vk_guest_gl: the flat varying of flat.vert as the colour, as the provoking vertex gave it
precision highp float;
flat in vec4 vFlat;
out vec4 fragColor;
void main() {
    fragColor = vFlat;
}
