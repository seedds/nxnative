// t_vk_vbuf: the first word of vertex streams 0, 14 and 15 and of an attribute no stream feeds (the zero buffer), written to row gl_InstanceIndex of storage buffer 0
#version 450
layout(location = 0) in uint a;
layout(location = 1) in uint b;
layout(location = 2) in uint c;
layout(location = 3) in uint z;
layout(set = 1, binding = 0, std430) buffer O { uvec4 o[]; };
void main() { o[gl_InstanceIndex] = uvec4(a, b, c, z); gl_Position = vec4(0.0, 0.0, 0.0, 1.0); gl_PointSize = 1.0; }
