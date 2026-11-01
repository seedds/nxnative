#version 320 es
// t_vk_guest_gl: the textured quad's colour, the texture times a default-uniform-block colour
precision highp float;
uniform sampler2D uTex;
uniform vec4 uColor;
in vec2 vUV;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vUV) * uColor;
}
