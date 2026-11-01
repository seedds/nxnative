#version 330 core
// t_vk_guest_gl: LÖVE's dialect (#version 330 core): attributes the program binds with glBindAttribLocation (VertexColor declared first, bound to index 2), a mat4 and a
// float array in the default uniform block, and two varyings declared in the opposite order from the fragment shader's (GL links varyings by name)
in vec4 VertexColor;
in vec2 VertexPosition;
uniform mat4 uTransform;
uniform float uScale[2];
out vec4 vColor;
out float vFade;
void main() {
    vColor = VertexColor;
    vFade = uScale[1];
    gl_Position = uTransform * vec4(VertexPosition * uScale[0], 0.0, 1.0);
}
