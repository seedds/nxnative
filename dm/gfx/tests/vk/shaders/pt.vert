// t_vk_* tests: a point at the centre per vertex (a geometry or capturing stage makes the rest)
#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }
