#version 330 core
// t_vk_guest_gl: the stream's colour: the vertex colour faded by vFade; uInvert is a GLSL bool, a uint in the SPIR-V block, reported as bool in the table
in float vFade;
in vec4 vColor;
uniform bool uInvert;
out vec4 fragColor;
void main() {
    vec3 c = vColor.rgb * vFade;
    fragColor = vec4(uInvert ? vec3(1.0) - c : c, vColor.a);
}
