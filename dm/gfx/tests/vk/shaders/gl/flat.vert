#version 320 es
// t_vk_guest_gl: a flat-interpolated varying (the provoking vertex): each vertex's own colour, which a primitive takes from one of its vertices, GL's last by default
precision highp float;
in vec2 aPos;
in vec4 aColor;
flat out vec4 vFlat;
void main() {
    vFlat = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
