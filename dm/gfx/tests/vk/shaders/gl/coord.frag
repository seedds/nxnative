#version 320 es
// t_vk_guest_gl: gl_FragCoord as colour (red across, green up), for the orientation checks: GL's window y counts from the bottom row. min and clamp become
// NMin and NClamp in gl_shaders.py --vk
precision highp float;
uniform vec2 uSize;
out vec4 fragColor;
void main() {
    fragColor = vec4(min(gl_FragCoord.x / uSize.x, 1.0), clamp(gl_FragCoord.y / uSize.y, 0.0, 1.0), 0.0, 1.0);
}
